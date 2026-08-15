Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$IdfArguments = @($args)

if (-not $IdfArguments) {
    $IdfArguments = @("build")
}

$stageProject = Join-Path $env:LOCALAPPDATA "ai-toy-idf\led_blink_test"
$stageMain = Join-Path $stageProject "main"

New-Item -ItemType Directory -Path $stageMain -Force | Out-Null

Copy-Item -LiteralPath (Join-Path $PSScriptRoot "CMakeLists.txt") -Destination $stageProject -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "main\CMakeLists.txt") -Destination $stageMain -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "main\Kconfig.projbuild") -Destination $stageMain -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "main\idf_component.yml") -Destination $stageMain -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "main\led_blink_test_main.c") -Destination $stageMain -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "main\backend_client.c") -Destination $stageMain -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "main\backend_client.h") -Destination $stageMain -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "main\max98357_audio.c") -Destination $stageMain -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "main\max98357_audio.h") -Destination $stageMain -Force

Write-Host "ESP-IDF staging project: $stageProject"

Push-Location -LiteralPath $stageProject
$idfExitCode = 1
try {
    & idf.py @IdfArguments
    $idfExitCode = $LASTEXITCODE
}
finally {
    Pop-Location
}

exit $idfExitCode
