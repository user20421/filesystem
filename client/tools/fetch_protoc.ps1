param(
    [string]$Version = "29.3"
)

$ErrorActionPreference = "Stop"
$toolsRoot = Join-Path $PSScriptRoot "../.tools"
$destination = Join-Path $toolsRoot "protoc"
$protoc = Join-Path $destination "bin/protoc.exe"

if (Test-Path $protoc) {
    & $protoc --version
    exit $LASTEXITCODE
}

New-Item -ItemType Directory -Force -Path $toolsRoot | Out-Null
$archive = Join-Path $toolsRoot "protoc-$Version-win64.zip"
$url = "https://github.com/protocolbuffers/protobuf/releases/download/v$Version/protoc-$Version-win64.zip"
Write-Host "Downloading host-only code generator from $url"
Invoke-WebRequest -Uri $url -OutFile $archive
Expand-Archive -Path $archive -DestinationPath $destination -Force
Remove-Item $archive -Force
& $protoc --version
