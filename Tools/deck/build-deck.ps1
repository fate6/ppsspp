# Builds a portable Linux x86_64 SDL tarball of the fork's committed HEAD in Docker.
#   powershell -ExecutionPolicy Bypass -File Tools\deck\build-deck.ps1 [-Out C:\claude\deck_build]
param([string]$Out = "C:\claude\deck_build")
$ErrorActionPreference = "Stop"
$src = Resolve-Path "$PSScriptRoot\..\.."
New-Item -ItemType Directory -Force $Out | Out-Null
docker build -t ppsspp-deck-build "$PSScriptRoot"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
docker run --rm -v "${src}:/src:ro" -v ppsspp-deck-work:/work -v "${Out}:/out" `
    ppsspp-deck-build bash /src/Tools/deck/build.sh
exit $LASTEXITCODE
