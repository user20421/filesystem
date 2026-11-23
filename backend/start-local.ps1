[CmdletBinding()]
param(
    [string]$ListenAddr = '127.0.0.1:8080'
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot '../scripts/_common.ps1')
$envFile = Get-EnvironmentFile
if (-not $envFile) {
    throw 'No environment file found. Run scripts/dev-up.ps1 first.'
}

function Get-LocalSetting {
    param([string]$Name, [string]$DefaultValue = '')
    return Get-DotEnvValue -Path $envFile -Name $Name -DefaultValue $DefaultValue
}

function New-RandomBase64 {
    param([int]$Size = 32)
    $bytes = New-Object byte[] $Size
    $random = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try { $random.GetBytes($bytes) } finally { $random.Dispose() }
    return [Convert]::ToBase64String($bytes)
}

# Persist the JWT secret so existing access tokens remain valid across backend
# restarts. This file is ignored by Git.
$jwtSecret = Get-LocalSetting 'JWT_SECRET'
$append = @()
if (-not $jwtSecret) {
    $jwtSecret = New-RandomBase64 48
    $append += "JWT_SECRET=$jwtSecret"
}
if ($append.Count -gt 0) {
    [System.IO.File]::AppendAllLines($envFile, [string[]]$append, (New-Object System.Text.UTF8Encoding($false)))
}

$mysqlPort = Get-LocalSetting 'MYSQL_PORT' '13306'
$mysqlUser = Get-LocalSetting 'MYSQL_USER' 'secure_im'
$mysqlPassword = Get-LocalSetting 'MYSQL_PASSWORD'
$minioPort = Get-LocalSetting 'MINIO_API_PORT' '19000'
$minioUser = Get-LocalSetting 'MINIO_ROOT_USER' 'minioadmin'
$minioPassword = Get-LocalSetting 'MINIO_ROOT_PASSWORD'
if (-not $mysqlPassword -or -not $minioPassword) {
    throw 'Infrastructure credentials are incomplete. Recreate deploy/.env with scripts/dev-up.ps1.'
}

$env:LISTEN_ADDR = $ListenAddr
$env:MYSQL_DSN = "${mysqlUser}:${mysqlPassword}@tcp(127.0.0.1:${mysqlPort})/secure_im?parseTime=true&loc=UTC&time_zone=%27%2B00:00%27&charset=utf8mb4&collation=utf8mb4_unicode_ci&timeout=5s"
$env:MINIO_ENDPOINT = "127.0.0.1:$minioPort"
$env:MINIO_ACCESS_KEY = $minioUser
$env:MINIO_SECRET_KEY = $minioPassword
$env:MINIO_BUCKET = 'secure-im-files'
$env:MINIO_USE_SSL = 'false'
$env:JWT_SECRET = $jwtSecret
$env:GOMODCACHE = Join-Path $PSScriptRoot '.cache/gomod'
$env:GOCACHE = Join-Path $PSScriptRoot '.cache/gobuild'

$binaryDirectory = Join-Path $PSScriptRoot '.cache/bin'
$binary = Join-Path $binaryDirectory 'secure-im-server.exe'
[System.IO.Directory]::CreateDirectory($binaryDirectory) | Out-Null
Set-Location $PSScriptRoot
& go build -o $binary ./cmd/server
if ($LASTEXITCODE -ne 0) { throw 'Backend build failed.' }
Write-Host "Starting IMFT backend at http://$ListenAddr"
& $binary
