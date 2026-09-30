param(
    [string]$OutputDirectory='tmp/presentation-proof',
    [string]$Experience='ORIGINAL',
    [int]$Frames=48,
    [int]$Fps=60,
    [int]$Message=-1,
    [string]$DisplayMode='16_9',
    [ValidateSet('GPU','SOFTWARE')][string]$Renderer='GPU',
    [switch]$FinalTarget,
    [switch]$Meter,
    [switch]$UpgradeFlash,
    [switch]$ScrambleWipe
)
$ErrorActionPreference='Stop'
$proofPath=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $proofPath | Out-Null
$env:SDL_AUDIODRIVER='dummy'
$env:STARFOX_TEST_HIDDEN='1'
$env:STARFOX_TEST_RENDERER=$Renderer
if($FinalTarget) {
    $env:STARFOX_TEST_SKIP_PREROLL='1'
    $env:STARFOX_CAPTURE_PRESENTATION_PATH=Join-Path $proofPath 'presentation.bmp'
}
$env:STARFOX_TEST_FRAMES="$Frames"
$env:STARFOX_TEST_PREROLL_TICKS='1000'
$env:STARFOX_TEST_EXPERIENCE=$Experience
$env:STARFOX_TEST_DISPLAY_MODE=$DisplayMode
$env:STARFOX_TEST_TIMING_MODE='ORIGINAL'
$env:STARFOX_TEST_PRESENTATION_FPS="$Fps"
$env:STARFOX_TEST_UNPACED='1'
$env:STARFOX_TEST_VSYNC='0'
$env:STARFOX_TEST_RENDER_SCALE='2'
$env:STARFOX_TEST_BLOOM='0'
$env:STARFOX_TEST_ENHANCED_SHADOWS='0'
$env:STARFOX_TEST_RAY_TRACING='0'
$env:STARFOX_CAPTURE_DIR=$proofPath
$env:STARFOX_CAPTURE_INTERVAL='1'
Remove-Item Env:STARFOX_TEST_MESSAGE,Env:STARFOX_TEST_SCRAMBLE_WIPE,Env:STARFOX_TEST_MESSAGE_METER,Env:STARFOX_TEST_UPGRADE_FLASH -ErrorAction SilentlyContinue
if($Message -ge 0) {$env:STARFOX_TEST_MESSAGE="$Message"}
if($ScrambleWipe) {$env:STARFOX_TEST_SCRAMBLE_WIPE='1'}
if($Meter) {$env:STARFOX_TEST_MESSAGE_METER='1'}
if($UpgradeFlash) {$env:STARFOX_TEST_UPGRADE_FLASH='1'}
$arguments=if($Experience -eq 'EX') {'tmp/runtime-inputs/starfox-ex/SFES.SFC assets/symbols/starfox-ex.txt LEVEL1_1'} else {'upstream-ultrastarfox/SF.SFC upstream-ultrastarfox/SYMBOLS.TXT LEVEL1_1'}
$proofProcess=Start-Process -FilePath build/current/starfox_pc.exe -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardError "$proofPath/runtime.log"
$proofProcessHandle=$proofProcess.Handle
if(-not $proofProcess.WaitForExit(60000)) {throw "Capture still running (PID $($proofProcess.Id))"}
if($proofProcess.ExitCode -ne 0) {throw "Capture failed: exit $($proofProcess.ExitCode); see runtime.log"}
if($FinalTarget -and !(Test-Path -LiteralPath $env:STARFOX_CAPTURE_PRESENTATION_PATH)) {
    throw 'Final presentation target was not captured'
}
Write-Output "Captured $Frames source-driven presentation frames in $proofPath"
