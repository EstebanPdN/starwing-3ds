param([string]$OutputDirectory='tmp/background-runtime-gpu-proof',
    [string[]]$Levels=@('LEVEL1_1'),[int]$PrerollTicks=1000,
    [ValidateSet('4_3','16_9','32_9')][string]$DisplayMode='16_9',
    [ValidateRange(1,4)][int]$RenderScale=2,
    [ValidateRange(0,2)][int]$Stereo=0,[switch]$Effects,[switch]$RequireExLogoRepair,
    [string]$Presses='', [ValidateRange(1,10000)][int]$Frames=6)
$ErrorActionPreference='Stop'
$proof=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $proof | Out-Null
$saved=@{}
Get-ChildItem Env: | Where-Object {$_.Name -match '^(STARFOX_|SDL_AUDIODRIVER$)'} | ForEach-Object {
    $saved[$_.Name]=$_.Value;Remove-Item -LiteralPath "Env:$($_.Name)"
}
try {
    $settings=@{
        SDL_AUDIODRIVER='dummy';STARFOX_TEST_HIDDEN='1';STARFOX_TEST_FRAMES="$Frames"
        STARFOX_TEST_SKIP_PREROLL='1';STARFOX_TEST_PREROLL_TICKS="$PrerollTicks"
        STARFOX_TEST_DISPLAY_MODE=$DisplayMode;STARFOX_TEST_RENDER_SCALE="$RenderScale"
        STARFOX_TEST_STEREO_OUTPUT="$Stereo"
        STARFOX_TEST_PRESENTATION_FPS='60';STARFOX_TEST_TIMING_MODE='ORIGINAL'
        STARFOX_TEST_UNPACED='1';STARFOX_TEST_VSYNC='0';STARFOX_TEST_MSU1='0'
        STARFOX_TEST_RENDERER='GPU';STARFOX_TEST_RAY_TRACING='0'
        STARFOX_TEST_BLOOM='0';STARFOX_TEST_BLOOM_2D='0';STARFOX_TEST_EFFECT='0'
        STARFOX_TEST_WORLD_EFFECT='0';STARFOX_TEST_MODEL_SMOOTHING='0'
        STARFOX_TEST_HDR_EFFECT='0';STARFOX_TEST_CHROMATIC_ABERRATION='0';STARFOX_TRACE_GPU='1'
    }
    foreach($key in $settings.Keys) {[Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')}
    if($Presses){$env:STARFOX_TEST_PRESSES=$Presses}
    if($Effects) {
        $env:STARFOX_TEST_BLOOM='1';$env:STARFOX_TEST_BLOOM_2D='1'
        $env:STARFOX_TEST_EFFECT='1';$env:STARFOX_TEST_WORLD_EFFECT='2'
        $env:STARFOX_TEST_HDR_EFFECT='1';$env:STARFOX_TEST_CHROMATIC_ABERRATION='1'
    }
    foreach($experience in @('ORIGINAL','EX')) {foreach($level in $Levels) {
        $env:STARFOX_TEST_EXPERIENCE=$experience
        $arguments=if($experience -eq 'EX') {"tmp/runtime-inputs/starfox-ex/SFES.SFC assets/symbols/starfox-ex.txt $level"}
            else {"upstream-ultrastarfox/SF.SFC upstream-ultrastarfox/SYMBOLS.TXT $level"}
        $hashes=@{}
        $modes=if($Stereo) {@('gpu','cpu-background')}else{@('gpu','cpu-background','fallback','cpu-models')}
        foreach($mode in $modes) {
            $env:STARFOX_DISABLE_GPU_BACKGROUND=if($mode -eq 'cpu-background') {'1'}else{$null}
            $env:STARFOX_DISABLE_GPU_LATE_CARTRIDGE=if($mode -eq 'cpu-background') {'1'}else{$null}
            $env:STARFOX_TEST_FAIL_BACKGROUND_GPU=if($mode -eq 'fallback') {'1'}else{$null}
            $env:STARFOX_DISABLE_GPU_GEOMETRY=if($mode -eq 'cpu-models') {'1'}else{$null}
            $name="$experience-$level-$mode"
            $env:STARFOX_CAPTURE_PRESENTATION_PATH=Join-Path $proof "$name.bmp"
            $log=Join-Path $proof "$name.log"
            $process=Start-Process build/current/starfox_pc.exe -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardError $log
            $handle=$process.Handle
            if(!$process.WaitForExit(60000)) {throw "Capture still running: PID $($process.Id), $log"}
            if($process.ExitCode -ne 0) {throw "Capture failed: $log"}
            if($mode -eq 'gpu' -and !(Select-String -LiteralPath $log -SimpleMatch 'native-background: GPU resident ordered layers' -Quiet)) {
                throw "Scene did not exercise the resident background path: $log"
            }
            if($RequireExLogoRepair -and $experience -eq 'EX' -and $mode -eq 'gpu' -and
                !(Select-String -LiteralPath $log -SimpleMatch 'native-background: GPU EX logo repair' -Quiet)) {
                throw "Scene did not exercise EX logo repair: $log"
            }
            if($level -eq 'TITLEMAP' -and $mode -eq 'gpu' -and
                !(Select-String -LiteralPath $log -SimpleMatch 'native-pipeline: GPU late title layers' -Quiet)) {
                throw "Title foreground did not remain GPU resident: $log"
            }
            $hashes[$mode]=(Get-FileHash -LiteralPath $env:STARFOX_CAPTURE_PRESENTATION_PATH -Algorithm SHA256).Hash
        }
        foreach($mode in ($modes | Where-Object {$_ -ne 'gpu'})) {
            if($hashes[$mode] -ne $hashes.gpu) {throw "Background presentation differs: $experience $level $mode"}
        }
        Write-Output "$experience ${level}: $($modes -join ', ') captures byte-identical (stereo=$Stereo)"
    }}
} finally {
    Get-ChildItem Env: | Where-Object {$_.Name -match '^(STARFOX_|SDL_AUDIODRIVER$)'} | ForEach-Object {Remove-Item -LiteralPath "Env:$($_.Name)"}
    foreach($key in $saved.Keys) {[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}
}
