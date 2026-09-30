param(
    [string]$OutputDirectory='tmp/background-audit',
    [ValidateSet('ORIGINAL','EX')][string]$Experience='ORIGINAL',
    [string[]]$Levels=@(),
    [ValidateRange(0,8000)][int[]]$Ticks=@(1000,6000),
    [string[]]$Aspects=@('4_3','16_9','32_9'),
    [string]$Executable='build/current/starfox_pc.exe',
    [switch]$Layers,
    [switch]$PpuSnapshot,
    [switch]$IncludeSpecialRoutes
)
$ErrorActionPreference='Stop'
$auditPath=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $auditPath | Out-Null
$rom=if($Experience -eq 'EX') {'tmp/runtime-inputs/starfox-ex/SFES.SFC'} else {'upstream-ultrastarfox/SF.SFC'}
$symbols=if($Experience -eq 'EX') {'assets/symbols/starfox-ex.txt'} else {'upstream-ultrastarfox/SYMBOLS.TXT'}
$availableLevels=@(Get-Content -LiteralPath $symbols | ForEach-Object {
    if($_ -match '^(LEVEL(?:[1-7]_[1-9]|_BLACKHOLE|_SPECIAL|_COMET))\s') {$Matches[1]}
} | Sort-Object -Unique)
if(!$Levels.Count) {
    $Levels=@($availableLevels | Where-Object {$IncludeSpecialRoutes -or $_ -match '^LEVEL[1-7]_[1-9]$'})
}
if(!$Levels.Count) {throw 'No level entry points found'}
foreach($level in $Levels) {
    if($level -notin $availableLevels) {throw "Unknown playable level for ${Experience}: $level"}
}
# Isolate this fixture from other diagnostic hooks in the invoking shell.
$savedEnvironment=@{}
Get-ChildItem Env: | Where-Object {$_.Name -match '^(STARFOX_|SDL_AUDIODRIVER$)'} | ForEach-Object {
    $savedEnvironment[$_.Name]=$_.Value
    Remove-Item -LiteralPath "Env:$($_.Name)"
}
try {
    $env:SDL_AUDIODRIVER='dummy'
    $env:STARFOX_TEST_HIDDEN='1'
    $env:STARFOX_TEST_FRAMES='1'
    $env:STARFOX_TEST_SKIP_PREROLL='1'
    $env:STARFOX_TEST_UNPACED='1'
    $env:STARFOX_TEST_EXPERIENCE=$Experience
    $env:STARFOX_TEST_RENDERER='GPU'
    $env:STARFOX_TEST_RENDER_SCALE='1'
    $env:STARFOX_TEST_PRESENTATION_FPS='60'
    $env:STARFOX_TEST_TIMING_MODE='ORIGINAL'
    $env:STARFOX_TEST_BLOOM='0'
    $env:STARFOX_TEST_MSU1='0'
    $env:STARFOX_TEST_VSYNC='0'
    $env:STARFOX_TEST_ENHANCED_SHADOWS='0'
    $env:STARFOX_TEST_RAY_TRACING='0'
    # Record the reached flow and background ID, not just the requested level.
    # A direct-entry sample can already be at its boss or another background.
    $env:STARFOX_TRACE_RENDER_STATE='1'
    foreach($level in $Levels) {
        foreach($tick in $Ticks) {
            if($tick -lt 0) {throw 'Negative preroll'}
            foreach($aspect in $Aspects) {
                if($aspect -notin @('4_3','16_9','32_9')) {throw "Unsupported audit aspect: $aspect"}
                $stem="$Experience-$level-$tick-$aspect"
                $env:STARFOX_TEST_PREROLL_TICKS="$tick"
                $env:STARFOX_TEST_DISPLAY_MODE=$aspect
                $env:STARFOX_CAPTURE_PATH=Join-Path $auditPath "$stem.bmp"
                if($Layers) {$env:STARFOX_CAPTURE_TITLE_LAYERS=Join-Path $auditPath "$stem-layer"}
                if($PpuSnapshot) {
                    $env:STARFOX_TEST_PPU_DUMP='1'
                    $env:STARFOX_CAPTURE_DIR=Join-Path $auditPath "$stem-snapshot"
                }
                $process=Start-Process -FilePath $Executable -ArgumentList "`"$rom`" `"$symbols`" $level" -WindowStyle Hidden -PassThru -RedirectStandardError (Join-Path $auditPath "$stem.log")
                $captureProcessHandle=$process.Handle
                if(!$process.WaitForExit(60000)) {throw "Capture still running: $stem, PID $($process.Id); inspect before restarting"}
                if($process.ExitCode -ne 0 -or !(Test-Path -LiteralPath $env:STARFOX_CAPTURE_PATH)) {throw "Capture failed: $stem"}
                Write-Output "Captured $stem"
            }
        }
    }
} finally {
    Get-ChildItem Env: | Where-Object {$_.Name -match '^(STARFOX_|SDL_AUDIODRIVER$)'} | ForEach-Object {Remove-Item -LiteralPath "Env:$($_.Name)"}
    foreach($entry in $savedEnvironment.GetEnumerator()) {Set-Item -LiteralPath "Env:$($entry.Key)" -Value $entry.Value}
}
