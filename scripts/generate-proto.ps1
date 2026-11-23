[CmdletBinding()]
param()

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

# 只使用仓内离线工具重新生成 Go 代码；Qt 的生成仍由 CMake 在构建目录完成。
$projectRoot = Split-Path -Parent $PSScriptRoot
$protoc = Join-Path $projectRoot 'client/.tools/protoc/bin/protoc.exe'
$pluginDirectory = Join-Path $projectRoot 'backend/.tools'
$plugin = Join-Path $pluginDirectory 'protoc-gen-go.exe'
foreach ($tool in @($protoc, $plugin)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "缺少离线 Protobuf 工具：$tool。请保留 client/.tools 和 backend/.tools。"
    }
}

$protoFiles = @(
    'proto/im/v1/envelope.proto',
    'proto/im/v1/messages.proto'
)
foreach ($relativePath in $protoFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $projectRoot $relativePath) -PathType Leaf)) {
        throw "缺少协议源文件：$relativePath"
    }
}

Push-Location $projectRoot
try {
    $env:PATH = "$pluginDirectory;$env:PATH"
    & $protoc --go_out=. --go_opt=module=secure-im @protoFiles
    if ($LASTEXITCODE -ne 0) {
        throw "Protobuf Go 代码生成失败，退出码为 $LASTEXITCODE。"
    }
}
finally {
    Pop-Location
}

Write-Host '已按 proto/im/v1 重新生成 backend/internal/proto/impb；请用 git diff 检查生成结果。'
