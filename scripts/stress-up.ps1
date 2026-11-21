[CmdletBinding()]
param(
    [int]$TimeoutSeconds = 180
)

. (Join-Path $PSScriptRoot '_common.ps1')

function New-StressRandomBytes {
    param([int]$Size = 32)
    $bytes = New-Object byte[] $Size
    $random = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try {
        $random.GetBytes($bytes)
    }
    finally {
        $random.Dispose()
    }
    return $bytes
}

function New-StressHexPassword {
    return ((New-StressRandomBytes -Size 24) | ForEach-Object { $_.ToString('x2') }) -join ''
}

function Initialize-StressEnvironmentFile {
    $path = Get-StressEnvironmentFile
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        $header = '# Generated stress-only secrets. This file is ignored by Git.'
        [System.IO.File]::WriteAllLines($path, [string[]]@($header), (New-Object System.Text.UTF8Encoding($false)))
    }
    $append = @()
    if (-not (Get-StressEnvironmentValue -Name 'STRESS_MYSQL_USER' -DefaultValue '')) {
        $append += 'STRESS_MYSQL_USER=secure_im_stress'
    }
    if (-not (Get-StressEnvironmentValue -Name 'STRESS_MYSQL_PASSWORD' -DefaultValue '')) {
        $append += "STRESS_MYSQL_PASSWORD=$(New-StressHexPassword)"
    }
    if (-not (Get-StressEnvironmentValue -Name 'STRESS_JWT_SECRET' -DefaultValue '')) {
        $secret = [Convert]::ToBase64String((New-StressRandomBytes -Size 48))
        $append += "STRESS_JWT_SECRET=$secret"
    }
    if ($append.Count -gt 0) {
        [System.IO.File]::AppendAllLines($path, [string[]]$append, (New-Object System.Text.UTF8Encoding($false)))
    }
    Write-Host "Using stress-only credentials: $path"
}

function Invoke-StressRootAuthRequest {
    param([Parameter(Mandatory = $true)][string]$Path)

    $uri = "http://127.0.0.1:18080$Path"
    $payload = @{ username = 'ROOT'; password = '123456' }
    if ($Path -eq '/api/v1/auth/register') {
        $payload.display_name = 'ROOT'
    }
    $body = $payload | ConvertTo-Json -Compress
    try {
        $response = Invoke-WebRequest -UseBasicParsing -Uri $uri -Method Post `
            -ContentType 'application/json' -Body $body -TimeoutSec 10
        return [pscustomobject]@{
            StatusCode = [int]$response.StatusCode
            Content = [string]$response.Content
        }
    }
    catch {
        if ($_.Exception.Response) {
            return [pscustomobject]@{
                StatusCode = [int]$_.Exception.Response.StatusCode
                Content = ''
            }
        }
        throw "ROOT authentication request failed: $($_.Exception.Message)"
    }
}

function Assert-StressRootCredentials {
    $registration = Invoke-StressRootAuthRequest -Path '/api/v1/auth/register'
    $alreadyExists = $registration.StatusCode -eq 409
    if (-not $alreadyExists -and ($registration.StatusCode -lt 200 -or $registration.StatusCode -ge 300)) {
        throw "ROOT registration failed with HTTP $($registration.StatusCode)."
    }

    # Always perform a real login, including after HTTP 409, so an existing ROOT
    # account with another password cannot silently break subsequent stress runs.
    $login = Invoke-StressRootAuthRequest -Path '/api/v1/auth/login'
    if ($login.StatusCode -lt 200 -or $login.StatusCode -ge 300) {
        if ($alreadyExists -and ($login.StatusCode -eq 401 -or $login.StatusCode -eq 403)) {
            throw "ROOT already exists with a different password; ROOT/123456 login verification returned HTTP $($login.StatusCode)."
        }
        throw "ROOT/123456 login verification failed with HTTP $($login.StatusCode)."
    }
    try {
        $auth = $login.Content | ConvertFrom-Json
        $data = if ($auth.data) { $auth.data } else { $auth }
        $accessToken = [string]$data.tokens.access_token
    }
    catch {
        throw 'ROOT login succeeded but returned an invalid authentication response.'
    }
    if ([string]::IsNullOrWhiteSpace($accessToken)) {
        throw 'ROOT login succeeded but the response did not contain an access token.'
    }
    Write-Host 'ROOT registration/login verified for the stress backend.'
}

function Initialize-StressDatabase {
    $rootPassword = Get-EnvironmentValue -Name 'MYSQL_ROOT_PASSWORD' -DefaultValue ''
    if (-not $rootPassword) {
        throw 'MYSQL_ROOT_PASSWORD is missing from the base environment file.'
    }
    $user = Get-StressEnvironmentValue -Name 'STRESS_MYSQL_USER' -DefaultValue 'secure_im_stress'
    $password = Get-StressEnvironmentValue -Name 'STRESS_MYSQL_PASSWORD' -DefaultValue ''
    if ($user -notmatch '^[A-Za-z0-9_]{1,32}$' -or $password -notmatch '^[A-Fa-f0-9]{32,128}$') {
        throw 'Stress MySQL credentials are invalid; regenerate deploy/.env.stress.'
    }
    $sql = "CREATE DATABASE IF NOT EXISTS ``secure_im_stress`` CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci; " +
        "CREATE USER IF NOT EXISTS '$user'@'%' IDENTIFIED BY '$password'; " +
        "ALTER USER '$user'@'%' IDENTIFIED BY '$password'; " +
        "GRANT ALL PRIVILEGES ON ``secure_im_stress``.* TO '$user'@'%';"
    Invoke-DockerCompose -ComposeArguments @('exec', '-T', '-e', "MYSQL_PWD=$rootPassword", 'mysql', 'mysql', '--user=root', '--batch', "--execute=$sql")
    Write-Host 'Stress database and grants are ready (secure_im_stress).'
}

try {
    Assert-Docker
    if (-not (Get-EnvironmentFile)) {
        throw 'No base .env file was found. Run scripts/dev-up.ps1 first.'
    }
    & docker network inspect secure-im-net | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw 'The secure-im-net network is missing. Run scripts/dev-up.ps1 first.'
    }
    foreach ($service in @('mysql', 'minio')) {
        if ((Get-ServiceHealth -ServiceName $service) -ne 'healthy') {
            throw "Base service $service is not healthy. Run scripts/dev-up.ps1 first."
        }
    }
    Initialize-StressEnvironmentFile
    Initialize-StressDatabase

    Write-Host 'Building and starting only the isolated stress backend...'
    # Build the Linux binary on the host so startup does not depend on registry access.
    $env:GOMODCACHE = Join-Path $script:ProjectRoot 'backend/.cache/gomod'
    $env:GOCACHE = Join-Path $script:ProjectRoot 'backend/.cache/gobuild'
    $binaryDir = Join-Path $script:ProjectRoot 'backend/docker'
    [System.IO.Directory]::CreateDirectory($binaryDir) | Out-Null
    Push-Location (Join-Path $script:ProjectRoot 'backend')
    try {
        $env:CGO_ENABLED = '0'
        $env:GOOS = 'linux'
        $env:GOARCH = 'amd64'
        & go build -trimpath -ldflags='-s -w' -o (Join-Path $binaryDir 'secure-im-server') ./cmd/server
        if ($LASTEXITCODE -ne 0) { throw 'Host Go build failed.' }
    }
    finally {
        Pop-Location
    }
    Invoke-StressDockerCompose -ComposeArguments @('up', '--detach', '--build', 'backend')
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $ready = $false
        try {
            $response = Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:18080/ready' -TimeoutSec 3
            $ready = $response.StatusCode -eq 200
        }
        catch {
            $ready = $false
        }
        if ($ready) {
            # Authentication failures are actionable and must not be hidden by the readiness retry loop.
            Assert-StressRootCredentials
            Write-Host 'Stress backend is ready: http://127.0.0.1:18080/ready'
            Write-Host 'Debug stats: http://127.0.0.1:18080/debug/stats'
            return
        }
        Start-Sleep -Seconds 2
    }
    throw "Timed out after $TimeoutSeconds seconds waiting for the stress backend."
}
catch {
    Write-Host "Stress startup failed: $($_.Exception.Message)" -ForegroundColor Red
    if (Test-Path -LiteralPath (Get-StressEnvironmentFile) -PathType Leaf) {
        Invoke-StressDockerCompose -ComposeArguments @('ps', '-a')
        Invoke-StressDockerCompose -ComposeArguments @('logs', '--tail', '40', 'backend')
    }
    exit 1
}
