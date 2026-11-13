[CmdletBinding()]
param()

. (Join-Path $PSScriptRoot '_common.ps1')

$failures = 0

function Write-Check {
    param(
        [string]$Name,
        [bool]$Passed,
        [string]$Detail = ''
    )

    if ($Passed) {
        Write-Host "[OK]   $Name $Detail" -ForegroundColor Green
    }
    else {
        Write-Host "[FAIL] $Name $Detail" -ForegroundColor Red
        $script:failures++
    }
}

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    Write-Check -Name 'Docker CLI' -Passed $false -Detail 'docker was not found on PATH'
    exit 1
}
Write-Check -Name 'Docker CLI' -Passed $true -Detail (& docker --version)

& docker version --format '{{.Server.Version}}' | Out-Null
Write-Check -Name 'Docker daemon' -Passed ($LASTEXITCODE -eq 0)
& docker compose version | Out-Null
Write-Check -Name 'Docker Compose v2' -Passed ($LASTEXITCODE -eq 0)

$envFile = Get-EnvironmentFile
Write-Check -Name 'Environment file' -Passed ([bool]$envFile) -Detail $envFile
if ($failures -gt 0 -or -not $envFile) {
    Write-Host "Doctor found $failures failure(s)." -ForegroundColor Red
    exit 1
}

Invoke-DockerCompose -ComposeArguments @('ps', '-a')

$serviceHealthy = @{}
foreach ($service in @('mysql', 'redis', 'minio')) {
    $health = Get-ServiceHealth -ServiceName $service
    $serviceHealthy[$service] = ($health -eq 'healthy')
    Write-Check -Name "Container $service" -Passed $serviceHealthy[$service] -Detail $health
}

if ($serviceHealthy['mysql']) {
    $mysqlUser = Get-EnvironmentValue -Name 'MYSQL_USER' -DefaultValue 'secure_im'
    $mysqlPassword = Get-EnvironmentValue -Name 'MYSQL_PASSWORD' -DefaultValue ''
    $mysqlArguments = @('exec', '-T', '-e', "MYSQL_PWD=$mysqlPassword", 'mysql', 'mysql', '--protocol=TCP', '-h', '127.0.0.1', '-u', $mysqlUser, 'secure_im', '-e', 'SELECT 1 AS ok;')
    & docker @(Get-DockerComposeArguments -AdditionalArguments $mysqlArguments)
    Write-Check -Name 'MySQL secure_im query' -Passed ($LASTEXITCODE -eq 0)
}
else {
    Write-Check -Name 'MySQL secure_im query' -Passed $false -Detail 'container is not healthy'
}

if ($serviceHealthy['redis']) {
    $redisPassword = Get-EnvironmentValue -Name 'REDIS_PASSWORD' -DefaultValue ''
    $redisArguments = @('exec', '-T', '-e', "REDISCLI_AUTH=$redisPassword", 'redis', 'redis-cli', '--no-auth-warning', 'ping')
    & docker @(Get-DockerComposeArguments -AdditionalArguments $redisArguments)
    Write-Check -Name 'Redis authenticated PING' -Passed ($LASTEXITCODE -eq 0)
}
else {
    Write-Check -Name 'Redis authenticated PING' -Passed $false -Detail 'container is not healthy'
}

if ($serviceHealthy['minio']) {
    & docker @(Get-DockerComposeArguments -AdditionalArguments @('exec', '-T', 'minio', 'curl', '--silent', '--fail', 'http://127.0.0.1:9000/minio/health/live'))
    Write-Check -Name 'MinIO health endpoint' -Passed ($LASTEXITCODE -eq 0)

    try {
        Test-MinioBucket
        Write-Check -Name 'MinIO bucket secure-im-files' -Passed $true
        Test-MinioAnonymousAccessDenied
        Write-Check -Name 'MinIO anonymous access denied' -Passed $true
    }
    catch {
        Write-Check -Name 'MinIO bucket/privacy checks' -Passed $false -Detail $_.Exception.Message
    }
}
else {
    Write-Check -Name 'MinIO health endpoint' -Passed $false -Detail 'container is not healthy'
    Write-Check -Name 'MinIO private bucket secure-im-files' -Passed $false -Detail 'container is not healthy'
}

if ($failures -gt 0) {
    Write-Host "Doctor found $failures failure(s)." -ForegroundColor Red
    exit 1
}

Write-Host 'All infrastructure checks passed.' -ForegroundColor Green
