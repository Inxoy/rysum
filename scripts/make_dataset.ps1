<#
.SYNOPSIS
    Samples the Rysum plugin into a dataset for training a neural synthesis network.

.DESCRIPTION
    Builds the sampler (plugin\tests\Dataset.cpp) if needed and runs it. The plugin plays by
    itself - single pipes, registrations playing phrases, factory patches - and every clip is
    written with the notes that were played and the settings it was played with:

        <Out>\audio\000000.wav ...    the clips
        <Out>\midi\000000.mid ...     the notes of each clip
        <Out>\metadata.jsonl          one line per clip: file, train / valid, kind, stops, notes,
                                      levels, the parameters that differ from the opened plugin
        <Out>\dataset.json            the format and the table of all parameters

    Ten hours of stereo at 48 kHz are about 7 GB as 16-bit WAV (half as FLAC) and take roughly
    twenty minutes. Stopped (Ctrl+C) and started again with the same options, it carries on
    where it was. The same seed gives the same notes and settings (the sound itself has the
    organ's own chance in it - wind, speech - so two runs are not identical sample for sample).

.EXAMPLE
    .\scripts\make_dataset.ps1
    Ten hours into ..\rysum-dataset (next to the repository).

.EXAMPLE
    .\scripts\make_dataset.ps1 -Hours 1 -Clip 4 -Dry -Mono -Out D:\data\rysum-dry
    One hour of dry mono clips of four seconds.
#>
param(
    [double]$Hours = 10,          # how much audio in total
    [double]$Clip = 10,           # seconds per clip (at least 4)
    [int]$Rate = 48000,           # sample rate
    [switch]$Mono,                # mono instead of stereo
    [ValidateSet(16, 24)][int]$Bits = 16,
    [switch]$Flac,                # FLAC instead of WAV
    [long]$Seed = 1,              # another seed = another dataset
    [string]$Mix = "40,40,20",    # per cent: one pipe / a registration playing / a factory patch playing
    [switch]$Dry,                 # no room in any clip (impulse response and Saal off)
    [double]$Valid = 5,           # per cent of the clips marked as validation set
    [string]$Out = "",            # where to (default: rysum-dataset next to the repository)
    [string]$Samples = ""         # the sample library (default: rysum-samples in the repository)
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$plugin = Join-Path $repo "plugin"
if ($Samples -eq "") { $Samples = Join-Path $repo "rysum-samples" }
if ($Out -eq "") { $Out = Join-Path (Split-Path -Parent $repo) "rysum-dataset" }
if (-not (Test-Path $Samples)) { throw "No sample library at $Samples (pass -Samples)." }

# room for it? (16-bit stereo at 48 kHz: 0.69 GB per hour)
$bytes = $Hours * 3600 * $Rate * ($Bits / 8)
if (-not $Mono) { $bytes = $bytes * 2 }
if ($Flac) { $bytes = $bytes * 0.6 }
$parent = Split-Path -Parent $Out
if (-not (Test-Path $parent)) { New-Item -ItemType Directory -Force $parent | Out-Null }
$drive = (Get-Item $parent).PSDrive
if ($drive.Free -and $drive.Free -lt $bytes * 1.1) {
    throw ("Not enough room on {0}: about {1:N1} GB needed, {2:N1} GB free." -f $drive.Name, ($bytes / 1GB), ($drive.Free / 1GB))
}
Write-Host ("About {0:N1} GB will be written to {1}" -f ($bytes / 1GB), $Out)

# the sampler: built in the plugin's own build folder (the first time this takes a few minutes)
$build = Join-Path $plugin "build"
$exe = Join-Path $build "RysumDataset_artefacts\Release\RysumDataset.exe"
Write-Host "Building the sampler ..."
if (Test-Path (Join-Path $build "CMakeCache.txt")) {
    cmake -S $plugin -B $build -DRYSUM_BUILD_DATASET=ON | Out-Null
} else {
    cmake -S $plugin -B $build -G "Visual Studio 17 2022" -A x64 -DRYSUM_BUILD_DATASET=ON | Out-Null
}
if ($LASTEXITCODE -ne 0) { throw "cmake could not configure the build." }
cmake --build $build --config Release --target RysumDataset | Select-String -Pattern " error |RysumDataset.exe"
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $exe)) { throw "The sampler did not build." }

$opts = @("--hours", $Hours, "--clip", $Clip, "--rate", $Rate, "--bits", $Bits, "--seed", $Seed,
          "--mix", $Mix, "--valid", $Valid)
if ($Mono) { $opts += @("--channels", 1) }
if ($Flac) { $opts += "--flac" }
if ($Dry) { $opts += "--dry" }
& $exe $Samples $Out @opts
if ($LASTEXITCODE -ne 0) { throw "The sampler stopped early (exit code $LASTEXITCODE)." }
