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
    if (-not (Get-StressEnvironmentValue -Name 'STRESS_FILE_KEK' -DefaultValue '')) {
        $kek = [Convert]::ToBase64String((New-StressRandomBytes -Size 32))
        $append += "STRESS_FILE_KEK=$kek"
    }
    if ($append.Count -gt 0) {
        [System.IO.File]::AppendAllLines($path, [string[]]$append, (New-Object System.Text.UTF8Encoding($false)))
    }
    Write-Host "Using stress-only credentials and keys: $path"
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
        try {
            $response = Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:18080/ready' -TimeoutSec 3
            if ($response.StatusCode -eq 200) {
                Write-Host 'Stress backend is ready: http://127.0.0.1:18080/ready'
                Write-Host 'Debug stats: http://127.0.0.1:18080/debug/stats'
                return
            }
        }
        catch {
            Start-Sleep -Seconds 2
        }
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
