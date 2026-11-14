[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [string]$ServerUrl = 'http://127.0.0.1:8080'
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$qtBin = 'D:\software\Qt\6.11.2\llvm-mingw_64\bin'
$cmake = 'D:\software\Qt\Tools\CMake_64\bin\cmake.exe'
$executable = Join-Path $PSScriptRoot 'build/qt-windows/bin/secure-im-simulator.exe'

if (-not $SkipBuild) {
    Push-Location $PSScriptRoot
    try {
        $protoc = Join-Path $PSScriptRoot '.tools/protoc/bin/protoc.exe'
        if (-not (Test-Path -LiteralPath $protoc)) {
            & (Join-Path $PSScriptRoot 'tools/fetch_protoc.ps1')
            if ($LASTEXITCODE -ne 0) { throw 'Failed to prepare local protoc.' }
        }
        & $cmake --preset qt-windows
        if ($LASTEXITCODE -ne 0) { throw 'Client CMake configuration failed.' }
        & $cmake --build --preset qt-windows -j 4
        if ($LASTEXITCODE -ne 0) { throw 'Client build failed.' }

        $stressSource = Join-Path $PSScriptRoot '../backend/cmd/stress-runner'
        if (Test-Path -LiteralPath $stressSource) {
            $stressRunner = Join-Path $PSScriptRoot 'build/qt-windows/bin/secure-im-stress-runner.exe'
            Push-Location (Join-Path $PSScriptRoot '../backend')
            try {
                & go build -o $stressRunner ./cmd/stress-runner
                if ($LASTEXITCODE -ne 0) { throw 'Stress runner build failed.' }
            } finally {
                Pop-Location
            }
        } else {
            Write-Warning 'Stress runner source is not ready; system-test UI will use the existing executable when available.'
        }
    } finally {
        Pop-Location
    }
}

if (-not (Test-Path -LiteralPath $executable)) {
    throw "Simulator not found: $executable"
}
$env:PATH = "$qtBin;$env:PATH"
$env:SECURE_IM_SERVER_URL = $ServerUrl
Set-Location (Join-Path $PSScriptRoot '..')
& $executable
