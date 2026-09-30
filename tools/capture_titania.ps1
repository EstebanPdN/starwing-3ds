param([string]$OutputDirectory='tmp/titania-proof',
    [ValidateSet('4_3','16_9','32_9')][string]$DisplayMode='16_9',
    [ValidateSet('ORIGINAL','EX')][string]$Experience='ORIGINAL',
    [string]$Executable='build/current/starfox_pc.exe')
$ErrorActionPreference='Stop'
$proofPath=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $proofPath | Out-Null
$savedEnvironment=@{}
Get-ChildItem Env: | Where-Object {$_.Name -match '^(STARFOX_|SDL_AUDIODRIVER$)'} | ForEach-Object {
    $savedEnvironment[$_.Name]=$_.Value
    Remove-Item -LiteralPath "Env:$($_.Name)"
}
try {
$env:SDL_AUDIODRIVER='dummy'
$env:STARFOX_TEST_HIDDEN='1'
$env:STARFOX_TEST_SKIP_PREROLL='1'
$env:STARFOX_TEST_FRAMES='180'
$env:STARFOX_TEST_UNPACED='1'
$env:STARFOX_TEST_EXPERIENCE=$Experience
$env:STARFOX_TEST_PRESENTATION_FPS='60'
$env:STARFOX_TEST_DISPLAY_MODE=$DisplayMode
$env:STARFOX_TEST_RENDER_SCALE='2'
$env:STARFOX_TEST_MSU1='0'
$env:STARFOX_TEST_VSYNC='0'
$env:STARFOX_TEST_RENDERER='GPU'
$env:STARFOX_TEST_TIMING_MODE='ORIGINAL'
$env:STARFOX_TEST_RTX_LIGHTING='0'
$env:STARFOX_TEST_RAY_TRACING='0'
$env:STARFOX_TEST_2D_FILTER='OFF'
$env:STARFOX_TEST_BLOOM='0'
$env:STARFOX_TEST_EFFECT='0'
$env:STARFOX_TEST_WORLD_EFFECT='0'
$env:STARFOX_TEST_HDR_EFFECT='0'
$env:STARFOX_TEST_CHROMATIC_ABERRATION='0'
$env:STARFOX_TEST_MODEL_SMOOTHING='0'
$env:STARFOX_TRACE_RENDER_STATE='1'
$env:STARFOX_TEST_TITANIA_END='1'
$env:STARFOX_TEST_PREROLL_TICKS='200'
$env:STARFOX_CAPTURE_PATH=Join-Path $proofPath 'titania.bmp'
$arguments=if($Experience -eq 'EX') {'tmp/runtime-inputs/starfox-ex/SFES.SFC assets/symbols/starfox-ex.txt LEVEL2_3'} else {'upstream-ultrastarfox/SF.SFC upstream-ultrastarfox/SYMBOLS.TXT LEVEL2_3'}
$process=Start-Process -FilePath $Executable -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardError "$proofPath/runtime.log"
$processHandle=$process.Handle # Keep the process handle alive for reliable ExitCode retrieval.
if(-not $process.WaitForExit(30000)) {throw "Capture still running: PID $($process.Id)"}
if($process.ExitCode -ne 0 -or !(Test-Path -LiteralPath $env:STARFOX_CAPTURE_PATH)) {throw "Capture failed; see runtime.log"}
$renderState=Get-Content -LiteralPath "$proofPath/runtime.log" | Where-Object {$_ -like 'render-state *'} | Select-Object -Last 1
if($renderState -notmatch 'mode=1 ' -or $renderState -notmatch 'hofs=1(?: |$)') {
    throw "Titania capture did not reach the authored Mode 1 water scanline background: $renderState"
}
Write-Output "Captured Titania: $proofPath"
} finally {
    Get-ChildItem Env: | Where-Object {$_.Name -match '^(STARFOX_|SDL_AUDIODRIVER$)'} | ForEach-Object {
        Remove-Item -LiteralPath "Env:$($_.Name)"
    }
    foreach($entry in $savedEnvironment.GetEnumerator()) {Set-Item -LiteralPath "Env:$($entry.Key)" -Value $entry.Value}
}
