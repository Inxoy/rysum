# Builds the two release zips for GitHub (Windows, x64):
#   Rysum-<tag>-win64.zip          the plugin (VST3), the standalone app and the scrambled samples
#   Rysum-Konzert-<tag>-win64.zip  the listener app, the listener as a VST3 (for venues) and the scrambled samples
# The samples go in as "rysum-samples.rysum" (RysumPack: FLAC, scrambled) - never as WAVs.
# Built in plugin/build-release without ASIO (the ASIO SDK's licence does not cover handing it out).
#
#   powershell -ExecutionPolicy Bypass -File scripts\make_release.ps1 -Tag v2026.11.27
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [string]$Samples = "",
    [string]$Out = ""
)
$ErrorActionPreference = "Stop"
# (PowerShell 5.1 knows the script's folder only here, not in the parameters' defaults)
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = (Resolve-Path (Join-Path $here "..")).Path
if ($Samples -eq "") { $Samples = Join-Path $root "rysum-samples" }
if ($Out -eq "") { $Out = Join-Path $root "release" }
$build = Join-Path $root "plugin\build-release"
$Samples = (Resolve-Path $Samples).Path

if (-not (Test-Path (Join-Path $build "CMakeCache.txt"))) {
    cmake -S (Join-Path $root "plugin") -B $build -G "Visual Studio 17 2022" -A x64 -DRYSUM_DEV=OFF -DRYSUM_ASIO=OFF -DRYSUM_BUILD_TESTS=OFF
    if ($LASTEXITCODE -ne 0) { throw "configure failed" }
}
cmake --build $build --config Release --target Rysum_VST3 Rysum_Standalone RysumKonzert RysumKonzertPlugin_VST3 RysumPack
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$art = Join-Path $build "Rysum_artefacts\Release"
$konz = Join-Path $build "RysumKonzert_artefacts\Release"
$konzVst = Join-Path $build "RysumKonzertPlugin_artefacts\Release\VST3\Rysum Konzert.vst3"
$pack = Join-Path $build "RysumPack_artefacts\Release\RysumPack.exe"

if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force $Out | Out-Null
$arc = Join-Path $Out "rysum-samples.rysum"
& $pack $Samples $arc
if ($LASTEXITCODE -ne 0) { throw "packing the samples failed" }

$readmePlugin = @"
Rysum $Tag - the organ of Rysum (East Frisia) as a spatial instrument
=====================================================================

Contents
  Rysum.vst3            the plugin (VST3, 64-bit)
  Rysum.exe             the same as a standalone program
  rysum-samples.rysum   the organ's samples (packed; the program reads them itself)

Install
  1. Copy "rysum-samples.rysum" into  Documents\Rysum\   (create the folder if needed).
     The plugin and the standalone both find it there. (Next to Rysum.exe works for the
     standalone too.)
  2. Plugin: copy the folder "Rysum.vst3" into  C:\Program Files\Common Files\VST3\
     and let your DAW rescan.
  3. Standalone: start Rysum.exe.

Windows may warn that the program is from an unknown publisher (it is not signed):
"More info" -> "Run anyway".

Konzert (play for listeners on other computers): see the README on GitHub. Listeners use the
separate download "Rysum Konzert".

The recordings of the organ belong to the author; they are for playing this instrument,
not for taking out of it.
https://github.com/Inxoy/rysum
"@

$readmeKonzert = @"
Rysum Konzert $Tag - listen to a Rysum player from anywhere
===========================================================

Unpack this folder anywhere and start "Rysum Konzert.exe". Keep "rysum-samples.rysum" next to it.

Type the address the player gives you (at home an IP such as 192.168.1.20, over the internet
something like  server.org/room), the password if there is one, and click "Zuhoeren".
Then walk: W A S D, drag the mouse to look, space to jump, I for the immersive view.
Your computer renders the organ itself, from where you stand in the 3D Saal - use headphones
(Binaural) or choose your loudspeakers at the top.

Windows may warn that the program is from an unknown publisher (it is not signed):
"More info" -> "Run anyway". The first time, allow it on the network when Windows asks.

For a venue with its own loudspeakers and decoder: "Rysum Konzert.vst3" is the same listener as a
plugin. Copy it into  C:\Program Files\Common Files\VST3\  and "rysum-samples.rysum" into
Documents\Rysum\ (or next to the .vst3 folder). In Reaper: a track with 36 channels (Routing ->
Track channels: 36), insert "Rysum Konzert" as an instrument, then your Ambisonics decoder after it
(e.g. IEM AllRADecoder, order 5, AmbiX / SN3D). The plugin starts in "AmbiX 5"; the menu in its window
also offers AmbiX 1-4, stereo, binaural and loudspeakers. Join and walk in its window as in the app
(click into the 3D view first so the keys go to the plugin). Keep the track monitored or the
transport running so the host keeps processing; if Reaper's "anticipative FX" makes it stutter,
choose a longer delay.

Player and listener must use the same version ($Tag).
https://github.com/Inxoy/rysum
"@

function New-Zip($name, $files, $readme) {
    $dir = Join-Path $Out $name
    New-Item -ItemType Directory -Force $dir | Out-Null
    foreach ($f in $files) { Copy-Item -Recurse -Force $f $dir }
    Set-Content -Path (Join-Path $dir "LIESMICH.txt") -Value $readme -Encoding UTF8
    $zip = Join-Path $Out "$name.zip"
    Compress-Archive -Path (Join-Path $dir "*") -DestinationPath $zip -Force
    Write-Host "wrote $zip ($([math]::Round((Get-Item $zip).Length / 1MB)) MB)"
}

New-Zip "Rysum-$Tag-win64" @((Join-Path $art "VST3\Rysum.vst3"), (Join-Path $art "Standalone\Rysum.exe"), $arc) $readmePlugin
New-Zip "Rysum-Konzert-$Tag-win64" @((Join-Path $konz "Rysum Konzert.exe"), $konzVst, $arc) $readmeKonzert
