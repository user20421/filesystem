[CmdletBinding()]
param(
    # -Integration 额外启动隔离后端执行真实 Go/Qt 集成测试；-CMakePath 可覆盖本机 CMake 路径。
    [switch]$Integration,
    [string]$CMakePath = 'D:/software/Qt/Tools/CMake_64/bin/cmake.exe'
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
[System.Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$script:ProjectRoot = Split-Path -Parent $PSScriptRoot
$script:BackendDirectory = Join-Path $script:ProjectRoot 'backend'
$script:ClientDirectory = Join-Path $script:ProjectRoot 'client'
$script:DeployDirectory = Join-Path $script:ProjectRoot 'deploy'

# 输出统一的测试阶段标题，便于快速定位失败步骤。
function Write-TestStep {
    param([Parameter(Mandatory = $true)][string]$Message)
    Write-Host "`n==== $Message ====" -ForegroundColor Cyan
}

# 在指定目录执行命令并恢复工作目录，非零退出码转换为带步骤名的异常。
function Invoke-CheckedCommand {
    param(
        [Parameter(Mandatory = $true)][string]$Description,
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][string[]]$ArgumentList,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory
    )

    $command = Get-Command $FilePath -ErrorAction SilentlyContinue
    if (-not $command) {
        throw "$Description 失败：找不到命令 $FilePath。"
    }

    $exitCode = 0
    Push-Location $WorkingDirectory
    try {
        & $command.Source @ArgumentList
        $exitCode = $LASTEXITCODE
    }
    finally {
        Pop-Location
    }
    if ($exitCode -ne 0) {
        throw "$Description 失败，退出码为 $exitCode。"
    }
}

# 读取 env 指定项，文件或键不存在时返回默认值，不输出文件中的其他秘密。
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

# 默认测试必须保持离线；仅 -Integration 分支会重新注入这些变量。
foreach ($name in @(
    'SECURE_IM_INTEGRATION_URL', 'INTEGRATION_MYSQL_DSN',
    'INTEGRATION_MINIO_ENDPOINT', 'INTEGRATION_MINIO_ACCESS_KEY',
    'INTEGRATION_MINIO_SECRET_KEY', 'INTEGRATION_MINIO_BUCKET',
    'CLIENT_BACKEND_URL'
)) {
    Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue
}

$env:GOMODCACHE = Join-Path $script:BackendDirectory '.cache/gomod'
$env:GOCACHE = Join-Path $script:BackendDirectory '.cache/gobuild'
[System.IO.Directory]::CreateDirectory($env:GOMODCACHE) | Out-Null
[System.IO.Directory]::CreateDirectory($env:GOCACHE) | Out-Null

try {
    Write-TestStep '1/11 检查 Go 格式（gofmt）'
    $goFiles = Get-ChildItem -LiteralPath $script:BackendDirectory -Recurse -File -Filter '*.go' |
        Where-Object {
            $_.FullName -notlike (Join-Path $script:BackendDirectory '.cache\*') -and
            $_.FullName -notlike (Join-Path $script:BackendDirectory '.tools\*')
        } |
        Select-Object -ExpandProperty FullName
    if (-not $goFiles) {
        throw 'Go 格式检查失败：backend 下没有找到 Go 源文件。'
    }
    $unformatted = @(& gofmt -l $goFiles)
    if ($LASTEXITCODE -ne 0) {
        throw "gofmt 检查失败，退出码为 $LASTEXITCODE。"
    }
    if ($unformatted.Count -gt 0) {
        throw "以下 Go 文件未通过 gofmt 检查：`n$($unformatted -join [Environment]::NewLine)"
    }

    Write-TestStep '2/11 运行 Go 单元测试'
    Invoke-CheckedCommand -Description 'go test ./...' -FilePath 'go' `
        -ArgumentList @('test', './...') -WorkingDirectory $script:BackendDirectory

    Write-TestStep '3/11 运行 Go 竞态检测'
    Invoke-CheckedCommand -Description 'go test -race ./...' -FilePath 'go' `
        -ArgumentList @('test', '-race', './...') -WorkingDirectory $script:BackendDirectory

    Write-TestStep '4/11 运行 Go 静态检查'
    Invoke-CheckedCommand -Description 'go vet ./...' -FilePath 'go' `
        -ArgumentList @('vet', './...') -WorkingDirectory $script:BackendDirectory

    $cmake = Get-Command $CMakePath -ErrorAction SilentlyContinue
    if (-not $cmake) {
        throw "Qt 配置失败：找不到 CMake：$CMakePath。可通过 -CMakePath 指定。"
    }
    $resolvedCMakePath = $cmake.Source
    $ctestPath = Join-Path (Split-Path -Parent $resolvedCMakePath) 'ctest.exe'
    if (-not (Test-Path -LiteralPath $ctestPath -PathType Leaf)) {
        throw "Qt 测试失败：CMake 同目录下找不到 ctest.exe：$ctestPath。"
    }

    Write-TestStep '5/11 配置 Qt 客户端'
    Invoke-CheckedCommand -Description 'Qt CMake configure' -FilePath $resolvedCMakePath `
        -ArgumentList @('--preset', 'qt-windows') -WorkingDirectory $script:ClientDirectory

    Write-TestStep '6/11 构建 Qt 客户端及测试'
    Invoke-CheckedCommand -Description 'Qt CMake build' -FilePath $resolvedCMakePath `
        -ArgumentList @('--build', '--preset', 'qt-windows', '-j', '4') -WorkingDirectory $script:ClientDirectory

    Write-TestStep '7/11 运行 Qt CTest'
    Invoke-CheckedCommand -Description 'Qt CTest' -FilePath $ctestPath `
        -ArgumentList @('--preset', 'qt-windows') -WorkingDirectory $script:ClientDirectory

    Write-TestStep '8/11 检查全仓 PowerShell 语法'
    $syntaxErrors = @()
    $powerShellFiles = Get-ChildItem -LiteralPath $script:ProjectRoot -Recurse -Force -File -Filter '*.ps1' |
        Sort-Object -Property FullName
    foreach ($file in $powerShellFiles) {
        $tokens = $null
        $errors = $null
        [System.Management.Automation.Language.Parser]::ParseFile(
            $file.FullName, [ref]$tokens, [ref]$errors) | Out-Null
        foreach ($parseError in $errors) {
            $syntaxErrors += "$($file.FullName):$($parseError.Extent.StartLineNumber)：$($parseError.Message)"
        }
    }
    if ($syntaxErrors.Count -gt 0) {
        throw "PowerShell 语法检查失败：`n$($syntaxErrors -join [Environment]::NewLine)"
    }
    Write-Host "已检查 $($powerShellFiles.Count) 个 PowerShell 脚本。"

    Write-TestStep '9/11 检查正常环境 Compose 示例配置'
    Invoke-CheckedCommand -Description '正常环境 Compose 示例配置检查' -FilePath 'docker' `
        -ArgumentList @(
            'compose', '--env-file', (Join-Path $script:DeployDirectory '.env.example'),
            '-f', (Join-Path $script:DeployDirectory 'docker-compose.yml'), 'config', '--quiet'
        ) -WorkingDirectory $script:ProjectRoot

    Write-TestStep '10/11 检查压测环境 Compose 示例配置'
    Invoke-CheckedCommand -Description '压测环境 Compose 示例配置检查' -FilePath 'docker' `
        -ArgumentList @(
            'compose', '--env-file', (Join-Path $script:DeployDirectory '.env.example'),
            '--env-file', (Join-Path $script:DeployDirectory '.env.stress.example'),
            '-f', (Join-Path $script:DeployDirectory 'docker-compose.stress.yml'), 'config', '--quiet'
        ) -WorkingDirectory $script:ProjectRoot

    if ($Integration) {
        Write-TestStep '11/11 运行真实后端集成测试'
        $stressUp = Join-Path $PSScriptRoot 'stress-up.ps1'
        $stressDown = Join-Path $PSScriptRoot 'stress-down.ps1'
        try {
            # 集成用例覆盖默认的每用户 3 个活跃上传限制。
            $env:STRESS_MAX_ACTIVE_FILES = '3'
            Write-Host '正在启动独立压测后端（不运行大规模负载）...'
            & $stressUp
            if ($LASTEXITCODE -ne 0) {
                throw "独立压测后端启动失败，退出码为 $LASTEXITCODE。"
            }

            $baseEnv = Join-Path $script:ProjectRoot '.env'
            if (-not (Test-Path -LiteralPath $baseEnv -PathType Leaf)) {
                $baseEnv = Join-Path $script:DeployDirectory '.env'
            }
            $stressEnv = Join-Path $script:DeployDirectory '.env.stress'
            $mysqlUser = Get-DotEnvValue -Path $stressEnv -Name 'STRESS_MYSQL_USER' -DefaultValue 'secure_im_stress'
            $mysqlPassword = Get-DotEnvValue -Path $stressEnv -Name 'STRESS_MYSQL_PASSWORD'
            $mysqlPort = Get-DotEnvValue -Path $baseEnv -Name 'MYSQL_PORT' -DefaultValue '13306'
            $minioPort = Get-DotEnvValue -Path $baseEnv -Name 'MINIO_API_PORT' -DefaultValue '19000'
            $minioAccessKey = Get-DotEnvValue -Path $baseEnv -Name 'MINIO_ROOT_USER' -DefaultValue 'minioadmin'
            $minioSecretKey = Get-DotEnvValue -Path $baseEnv -Name 'MINIO_ROOT_PASSWORD'
            if (-not $mysqlPassword -or -not $minioSecretKey) {
                throw '集成测试环境缺少必需的本地凭据，请重新运行 scripts/stress-up.ps1。'
            }

            # stress-up.ps1 会为离线镜像设置 Linux 构建参数；真实 Go 测试仍运行在 Windows。
            $env:GOOS = 'windows'
            $env:GOARCH = 'amd64'
            $env:CGO_ENABLED = '1'

            # 凭据只注入子进程环境，不写入命令行或日志。
            $env:SECURE_IM_INTEGRATION_URL = 'http://127.0.0.1:18080'
            $env:INTEGRATION_MYSQL_DSN = "${mysqlUser}:${mysqlPassword}@tcp(127.0.0.1:${mysqlPort})/secure_im_stress?parseTime=true&loc=UTC&time_zone=%27%2B00:00%27"
            $env:INTEGRATION_MINIO_ENDPOINT = "127.0.0.1:$minioPort"
            $env:INTEGRATION_MINIO_ACCESS_KEY = $minioAccessKey
            $env:INTEGRATION_MINIO_SECRET_KEY = $minioSecretKey
            $env:INTEGRATION_MINIO_BUCKET = 'secure-im-stress-files'
            Invoke-CheckedCommand -Description 'Go 真实后端集成测试' -FilePath 'go' `
                -ArgumentList @('test', '-run', '^TestReal', '-v', '.') -WorkingDirectory $script:BackendDirectory

            $presetPath = Join-Path $script:ClientDirectory 'CMakePresets.json'
            $preset = Get-Content -LiteralPath $presetPath -Raw | ConvertFrom-Json
            $qtPaths = @($preset.configurePresets[0].environment.PATH -split ';' |
                Where-Object { $_ -and $_ -ne '$penv{PATH}' -and (Test-Path -LiteralPath $_ -PathType Container) })
            $env:PATH = (@($qtPaths) + @($env:PATH)) -join ';'
            $env:CLIENT_BACKEND_URL = 'http://127.0.0.1:18080'
            $qtIntegration = Join-Path $script:ClientDirectory 'build/qt-windows/test_backend_integration.exe'
            if (-not (Test-Path -LiteralPath $qtIntegration -PathType Leaf)) {
                throw "Qt 真实后端集成测试失败：找不到 $qtIntegration。"
            }
            Invoke-CheckedCommand -Description 'Qt test_backend_integration' -FilePath $qtIntegration `
                -ArgumentList @() -WorkingDirectory $script:ClientDirectory
        }
        finally {
            Write-Host '正在清理独立压测后端，保留基础设施与数据...'
            & $stressDown
            if ($LASTEXITCODE -ne 0) {
                throw "独立压测后端清理失败，退出码为 $LASTEXITCODE。"
            }
        }
    }
    else {
        Write-TestStep '11/11 跳过可选集成测试'
        Write-Host '默认测试完成；如需真实后端集成测试，请追加 -Integration。'
    }

    Write-Host "`n全部请求的测试均已通过。" -ForegroundColor Green
}
catch {
    Write-Host "`n测试失败：$($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
