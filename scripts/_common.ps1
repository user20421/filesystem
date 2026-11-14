Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$script:ProjectRoot = Split-Path -Parent $PSScriptRoot
$script:DeployDirectory = Join-Path $script:ProjectRoot 'deploy'
$script:ComposeFile = Join-Path $script:DeployDirectory 'docker-compose.yml'
$script:ComposeProject = 'secure-im'

function Get-EnvironmentFile {
    $rootEnv = Join-Path $script:ProjectRoot '.env'
    $deployEnv = Join-Path $script:DeployDirectory '.env'

    if (Test-Path -LiteralPath $rootEnv -PathType Leaf) {
        return $rootEnv
    }
    if (Test-Path -LiteralPath $deployEnv -PathType Leaf) {
        return $deployEnv
    }
    return $null
}

function Assert-Docker {
    if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
        throw 'Docker CLI was not found on PATH. Start Docker Desktop and try again.'
    }

    & docker version --format '{{.Server.Version}}' | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw 'The Docker daemon is unavailable. Start Docker Desktop and try again.'
    }

    & docker compose version | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw 'Docker Compose v2 is unavailable.'
    }
}

function Get-DockerComposeArguments {
    param([string[]]$AdditionalArguments)

    $envFile = Get-EnvironmentFile
    if (-not $envFile) {
        throw 'No .env file was found. Run scripts/dev-up.ps1 to create deploy/.env.'
    }

    return @('compose', '--env-file', $envFile, '-f', $script:ComposeFile, '-p', $script:ComposeProject) + $AdditionalArguments
}

function Invoke-DockerCompose {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]]$ComposeArguments)

    $dockerArguments = Get-DockerComposeArguments -AdditionalArguments $ComposeArguments
    & docker @dockerArguments
    if ($LASTEXITCODE -ne 0) {
        throw "docker compose $($ComposeArguments -join ' ') failed with exit code $LASTEXITCODE."
    }
}

function Get-ComposeContainerId {
    param(
        [Parameter(Mandatory = $true)][string]$ServiceName,
        [switch]$All
    )

    $psArguments = @('ps', '-q')
    if ($All) {
        $psArguments += '-a'
    }
    $psArguments += $ServiceName
    $dockerArguments = Get-DockerComposeArguments -AdditionalArguments $psArguments
    $containerId = & docker @dockerArguments 2>$null | Select-Object -First 1
    return $containerId
}

function Get-ServiceHealth {
    param([Parameter(Mandatory = $true)][string]$ServiceName)

    $containerId = Get-ComposeContainerId -ServiceName $ServiceName
    if (-not $containerId) {
        return 'missing'
    }

    $health = & docker inspect --format '{{if .State.Health}}{{.State.Health.Status}}{{else}}{{.State.Status}}{{end}}' $containerId 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $health) {
        return 'unknown'
    }
    return $health
}

function Wait-ServiceHealth {
    param([int]$TimeoutSeconds = 180)

    $services = @('mysql', 'redis', 'minio')
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)

    while ([DateTime]::UtcNow -lt $deadline) {
        $allHealthy = $true
        foreach ($service in $services) {
            if ((Get-ServiceHealth -ServiceName $service) -ne 'healthy') {
                $allHealthy = $false
            }
        }

        if ($allHealthy) {
            Write-Host 'MySQL, Redis, and MinIO are healthy.'
            return
        }
        Start-Sleep -Seconds 3
    }

    $statuses = $services | ForEach-Object { "$_=$(Get-ServiceHealth -ServiceName $_)" }
    throw "Timed out waiting for healthy services: $($statuses -join ', ')."
}

function Invoke-MinioClient {
    param([Parameter(Mandatory = $true)][string]$Command)

    # Compose expands these local-only credentials before creating the ephemeral client.
    # Use the client bundled in the already-running MinIO image, avoiding a
    # separate minio/mc image pull when the registry is unavailable.
    $clientCommand = 'export MC_CONFIG_DIR=/tmp/secure-im-mc; /usr/bin/mc alias set local http://minio:9000 $MINIO_ROOT_USER $MINIO_ROOT_PASSWORD >/dev/null && ' + $Command
    $runArguments = @('run', '--rm', '--no-deps', '--entrypoint', '/bin/sh', 'minio', '-c', $clientCommand)
    $dockerArguments = Get-DockerComposeArguments -AdditionalArguments $runArguments
    & docker @dockerArguments | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw 'The MinIO client command failed.'
    }
}

function Initialize-MinioBucket {
    Invoke-MinioClient -Command 'mc mb --ignore-existing local/secure-im-files && mc anonymous set none local/secure-im-files'
    Test-MinioBucket
    Test-MinioAnonymousAccessDenied
    Write-Host 'MinIO bucket secure-im-files exists and is private.'
}

function Test-MinioBucket {
    try {
        Invoke-MinioClient -Command 'mc stat local/secure-im-files >/dev/null'
    }
    catch {
        throw 'MinIO bucket secure-im-files is missing or inaccessible.'
    }
}

function Get-EnvironmentValue {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$DefaultValue
    )

    $envFile = Get-EnvironmentFile
    if ($envFile) {
        foreach ($line in [System.IO.File]::ReadLines($envFile)) {
            if ($line -match ('^\s*' + [regex]::Escape($Name) + '=(.*)$')) {
                return $matches[1].Trim()
            }
        }
    }
    return $DefaultValue
}

function Test-MinioAnonymousAccessDenied {
    $port = Get-EnvironmentValue -Name 'MINIO_API_PORT' -DefaultValue '19000'
    $uri = "http://127.0.0.1:$port/secure-im-files/"
    try {
        $response = Invoke-WebRequest -UseBasicParsing -Uri $uri -TimeoutSec 10
        throw "Anonymous MinIO bucket access returned unexpected HTTP $([int]$response.StatusCode)."
    }
    catch {
        if ($_.Exception.Response -and [int]$_.Exception.Response.StatusCode -eq 403) {
            return
        }
        throw "Anonymous MinIO bucket access was not denied with HTTP 403: $($_.Exception.Message)"
    }
}

function Get-StressEnvironmentFile {
    return (Join-Path $script:DeployDirectory '.env.stress')
}

function Get-StressEnvironmentValue {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$DefaultValue
    )
    $envFile = Get-StressEnvironmentFile
    if (Test-Path -LiteralPath $envFile -PathType Leaf) {
        foreach ($line in [System.IO.File]::ReadLines($envFile)) {
            if ($line -match ('^\s*' + [regex]::Escape($Name) + '=(.*)$')) {
                return $matches[1].Trim()
            }
        }
    }
    return $DefaultValue
}

function Get-StressDockerComposeArguments {
    param([string[]]$AdditionalArguments)

    $envFile = Get-EnvironmentFile
    if (-not $envFile) {
        throw 'No base .env file was found. Run scripts/dev-up.ps1 first.'
    }
    $stressEnv = Get-StressEnvironmentFile
    if (-not (Test-Path -LiteralPath $stressEnv -PathType Leaf)) {
        throw 'No stress environment file was found. Run scripts/stress-up.ps1 first.'
    }
    $composeFile = Join-Path $script:DeployDirectory 'docker-compose.stress.yml'
    return @('compose', '--env-file', $envFile, '--env-file', $stressEnv, '-f', $composeFile, '-p', 'secure-im-stress') + $AdditionalArguments
}

function Invoke-StressDockerCompose {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]]$ComposeArguments)

    $dockerArguments = Get-StressDockerComposeArguments -AdditionalArguments $ComposeArguments
    & docker @dockerArguments
    if ($LASTEXITCODE -ne 0) {
        throw "docker compose $($ComposeArguments -join ' ') failed with exit code $LASTEXITCODE."
    }
}
