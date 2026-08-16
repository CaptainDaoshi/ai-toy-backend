Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$modelRoot = Join-Path $env:LOCALAPPDATA "ai-toy-vosk"
$modelDirectory = Join-Path $modelRoot "vosk-model-small-cn-0.22"
$modelZipPath = Join-Path $env:TEMP "vosk-model-small-cn-0.22.zip"
$modelUrl = "https://alphacephei.com/vosk/models/vosk-model-small-cn-0.22.zip"

if (Test-Path -LiteralPath (Join-Path $modelDirectory "am")) {
    Write-Host "Vosk model is already installed: $modelDirectory"
    exit 0
}

New-Item -ItemType Directory -Path $modelRoot -Force | Out-Null
Write-Host "Downloading the 42 MB Chinese Vosk model (an interrupted download can be resumed)..."
& curl.exe -L --fail --retry 3 -C - --output $modelZipPath $modelUrl
if ($LASTEXITCODE -ne 0) {
    throw "Model download failed. Run this script again to resume from the partial ZIP."
}

Expand-Archive -LiteralPath $modelZipPath -DestinationPath $modelRoot -Force
foreach ($requiredDirectory in @("am", "conf", "graph")) {
    $requiredPath = Join-Path $modelDirectory $requiredDirectory
    if (-not (Test-Path -LiteralPath $requiredPath)) {
        throw "Downloaded model is incomplete: missing $requiredPath"
    }
}

Write-Host "Vosk model installed: $modelDirectory"
