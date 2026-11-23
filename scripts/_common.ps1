Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$script:ProjectRoot = Split-Path -Parent $PSScriptRoot
$script:DeployDirectory = Join-Path $script:ProjectRoot 'deploy'
$script:ComposeFile = Join-Path $script:DeployDirectory 'docker-compose.yml'
$script:ComposeProject = 'secure-im'

# 优先返回项目根目录 .env，其次返回 deploy/.env；均不存在时返回 $null。
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

# 依次检查 Docker CLI、守护进程与 Compose v2，环境不可用时立即报错。
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

# 组装正常环境 Compose 参数，并要求存在本地环境文件。
function Get-DockerComposeArguments {
    param([string[]]$AdditionalArguments)

    $envFile = Get-EnvironmentFile
    if (-not $envFile) {
        throw 'No .env file was found. Run scripts/dev-up.ps1 to create deploy/.env.'
    }

    return @('compose', '--env-file', $envFile, '-f', $script:ComposeFile, '-p', $script:ComposeProject) + $AdditionalArguments
}

# 执行 Compose 命令，非零退出码转换为异常，避免启动流程继续误判。
function Invoke-DockerCompose {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]]$ComposeArguments)

    $dockerArguments = Get-DockerComposeArguments -AdditionalArguments $ComposeArguments
    & docker @dockerArguments
    if ($LASTEXITCODE -ne 0) {
        throw "docker compose $($ComposeArguments -join ' ') failed with exit code $LASTEXITCODE."
    }
}

# 查询服务容器 ID；-All 同时包含已停止容器。
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

# 返回服务健康状态，容器不存在或无法判断时返回 missing/unknown。
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

# 轮询 MySQL/MinIO 健康状态，超时后抛出各服务当前状态。
function Wait-ServiceHealth {
    param([int]$TimeoutSeconds = 180)

    $services = @('mysql', 'minio')
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)

    while ([DateTime]::UtcNow -lt $deadline) {
        $allHealthy = $true
        foreach ($service in $services) {
            if ((Get-ServiceHealth -ServiceName $service) -ne 'healthy') {
                $allHealthy = $false
            }
        }

        if ($allHealthy) {
            Write-Host 'MySQL and MinIO are healthy.'
            return
        }
        Start-Sleep -Seconds 3
    }

    $statuses = $services | ForEach-Object { "$_=$(Get-ServiceHealth -ServiceName $_)" }
    throw "Timed out waiting for healthy services: $($statuses -join ', ')."
}

# 复用运行中 MinIO 镜像的临时客户端，避免额外下载 mc 镜像。
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

# 幂等创建私有 Bucket，并验证存在性和匿名访问拒绝。
function Initialize-MinioBucket {
    Invoke-MinioClient -Command 'mc mb --ignore-existing local/secure-im-files && mc anonymous set none local/secure-im-files'
    Test-MinioBucket
    Test-MinioAnonymousAccessDenied
    Write-Host 'MinIO bucket secure-im-files exists and is private.'
}

# 验证 Bucket 存在且当前凭据可访问。
function Test-MinioBucket {
    try {
        Invoke-MinioClient -Command 'mc stat local/secure-im-files >/dev/null'
    }
    catch {
        throw 'MinIO bucket secure-im-files is missing or inaccessible.'
    }
}

# 从显式指定的 env 文件读取一个键；只返回目标值，不打印或改写其他配置。
function Get-DotEnvValue {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Name,
        [AllowEmptyString()][string]$DefaultValue = ''
    )

    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        foreach ($line in [System.IO.File]::ReadLines($Path)) {
            if ($line -match ('^\s*' + [regex]::Escape($Name) + '=(.*)$')) {
                return $matches[1].Trim()
            }
        }
    }
    return $DefaultValue
}

# 从所选 env 文件读取指定值，文件或键不存在时返回默认值。
function Get-EnvironmentValue {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$DefaultValue
    )

    $envFile = Get-EnvironmentFile
    if ($envFile) {
        return Get-DotEnvValue -Path $envFile -Name $Name -DefaultValue $DefaultValue
    }
    return $DefaultValue
}

# 匿名访问必须返回 HTTP 403，其他结果均视为 Bucket 权限配置失败。
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

# 返回固定的压测环境文件路径。
function Get-StressEnvironmentFile {
    return (Join-Path $script:DeployDirectory '.env.stress')
}

# 读取压测环境变量，缺失时返回调用方提供的默认值。
function Get-StressEnvironmentValue {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$DefaultValue
    )
    return Get-DotEnvValue -Path (Get-StressEnvironmentFile) -Name $Name -DefaultValue $DefaultValue
}

# 按基础 env、压测 env 的顺序组装双环境 Compose 参数。
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

# 执行压测 Compose 并检查退出码，失败时终止后续压测流程。
function Invoke-StressDockerCompose {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]]$ComposeArguments)

    $dockerArguments = Get-StressDockerComposeArguments -AdditionalArguments $ComposeArguments
    & docker @dockerArguments
    if ($LASTEXITCODE -ne 0) {
        throw "docker compose $($ComposeArguments -join ' ') failed with exit code $LASTEXITCODE."
    }
}
