[CmdletBinding()]
param()

. (Join-Path $PSScriptRoot '_common.ps1')

try {
    Assert-Docker
    Write-Host 'Stopping only the isolated stress backend; base containers and all volumes are preserved...'
    Invoke-StressDockerCompose -ComposeArguments @('down', '--remove-orphans')
}
catch {
    Write-Host "Stress shutdown failed: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
