[CmdletBinding()]
param(
    [switch]$RemoveVolumes
)

. (Join-Path $PSScriptRoot '_common.ps1')

try {
    Assert-Docker
    if (-not (Get-EnvironmentFile)) {
        throw 'No .env file was found; nothing can be stopped with this configuration.'
    }

    if ($RemoveVolumes) {
        Write-Host 'Stopping services and PERMANENTLY deleting local database, Redis, and MinIO volumes...' -ForegroundColor Yellow
        Invoke-DockerCompose -ComposeArguments @('down', '--volumes', '--remove-orphans')
    }
    else {
        Write-Host 'Stopping services while preserving all named volumes...'
        Invoke-DockerCompose -ComposeArguments @('down', '--remove-orphans')
    }
}
catch {
    Write-Host "Shutdown failed: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
