[CmdletBinding()]
param()

. (Join-Path $PSScriptRoot '_common.ps1')

function New-LocalPassword {
    $bytes = New-Object byte[] 24
    $random = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try {
        $random.GetBytes($bytes)
    }
    finally {
        $random.Dispose()
    }
    return (($bytes | ForEach-Object { $_.ToString('x2') }) -join '')
}

function Initialize-EnvironmentFile {
    $existing = Get-EnvironmentFile
    if ($existing) {
        Write-Host "Using environment file: $existing"
        return
    }

    $example = Join-Path $script:DeployDirectory '.env.example'
    $destination = Join-Path $script:DeployDirectory '.env'
    $content = [System.IO.File]::ReadAllText($example)
    $content = $content.Replace('local-only-mysql-root-password-change-me', (New-LocalPassword))
    $content = $content.Replace('local-only-mysql-user-password-change-me', (New-LocalPassword))
    $content = $content.Replace('local-only-redis-password-change-me', (New-LocalPassword))
    $content = $content.Replace('local-only-minio-password-change-me', (New-LocalPassword))
    [System.IO.File]::WriteAllText($destination, $content, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "Created $destination with random local-development passwords."
}

try {
    Assert-Docker
    Initialize-EnvironmentFile
    $envFile = Get-EnvironmentFile

    Write-Host 'Pulling pinned images if needed and starting local infrastructure...'
    Invoke-DockerCompose -ComposeArguments @('up', '--detach')
    Wait-ServiceHealth
    Initialize-MinioBucket

    Write-Host ''
    Write-Host 'Local infrastructure is ready.'
    $mysqlPort = Get-EnvironmentValue -Name 'MYSQL_PORT' -DefaultValue '13306'
    $redisPort = Get-EnvironmentValue -Name 'REDIS_PORT' -DefaultValue '16379'
    $minioApiPort = Get-EnvironmentValue -Name 'MINIO_API_PORT' -DefaultValue '19000'
    $minioConsolePort = Get-EnvironmentValue -Name 'MINIO_CONSOLE_PORT' -DefaultValue '19001'
    Write-Host "MySQL: 127.0.0.1:$mysqlPort (database secure_im; see .env for credentials)"
    Write-Host "Redis: 127.0.0.1:$redisPort (password required; see .env for credentials)"
    Write-Host "MinIO API: http://127.0.0.1:$minioApiPort"
    Write-Host "MinIO Console: http://127.0.0.1:$minioConsolePort"
    Write-Host 'Bucket: secure-im-files (private)'
    Write-Host "Credentials/configuration: $envFile"
}
catch {
    Write-Host "Startup failed: $($_.Exception.Message)" -ForegroundColor Red
    if (Get-EnvironmentFile) {
        Invoke-DockerCompose -ComposeArguments @('ps', '-a')
        Invoke-DockerCompose -ComposeArguments @('logs', '--tail', '40')
    }
    exit 1
}
