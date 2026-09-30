param([string]$OutputDirectory='tmp/game-over-gpu-proof',
    [ValidateSet('16_9','32_9')][string]$DisplayMode='16_9',
    [ValidateRange(1,4)][int]$RenderScale=2,
    [ValidateRange(0,600)][int]$PrerollTicks=30)
$ErrorActionPreference='Stop'
$proof=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $proof | Out-Null
$saved=@{}
Get-ChildItem Env: | Where-Object {$_.Name -match '^(STARFOX_|SDL_AUDIODRIVER$)'} | ForEach-Object {
    $saved[$_.Name]=$_.Value; Remove-Item -LiteralPath "Env:$($_.Name)"
}
try {
    $settings=@{
        SDL_AUDIODRIVER='dummy'; STARFOX_TEST_HIDDEN='1'; STARFOX_TEST_FRAMES='12'
        STARFOX_TEST_SKIP_PREROLL='1'; STARFOX_TEST_PREROLL_TICKS="$PrerollTicks"
        STARFOX_TEST_DISPLAY_MODE=$DisplayMode; STARFOX_TEST_RENDER_SCALE="$RenderScale"
        STARFOX_TEST_PRESENTATION_FPS='60'; STARFOX_TEST_TIMING_MODE='ORIGINAL'
        STARFOX_TEST_UNPACED='1'; STARFOX_TEST_VSYNC='0'; STARFOX_TEST_MSU1='0'
        STARFOX_TEST_RENDERER='GPU'; STARFOX_TEST_RAY_TRACING='0'
        STARFOX_TEST_BLOOM='0'; STARFOX_TEST_BLOOM_2D='0'; STARFOX_TEST_EFFECT='0'
        STARFOX_TEST_WORLD_EFFECT='0'; STARFOX_TEST_MODEL_SMOOTHING='0'
        STARFOX_TEST_HDR_EFFECT='0'; STARFOX_TEST_CHROMATIC_ABERRATION='0'
        STARFOX_TRACE_GPU='1'
    }
    foreach($key in $settings.Keys) {[Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')}
    foreach($experience in @('ORIGINAL','EX')) {
        $env:STARFOX_TEST_EXPERIENCE=$experience
        $arguments=if($experience -eq 'EX') {'tmp/runtime-inputs/starfox-ex/SFES.SFC assets/symbols/starfox-ex.txt GAMEOVER'}
            else {'upstream-ultrastarfox/SF.SFC upstream-ultrastarfox/SYMBOLS.TXT GAMEOVER'}
        $hashes=@{}
        foreach($mode in @('gpu','cpu','fallback','stereo-fallback')) {
            Remove-Item Env:STARFOX_DISABLE_GPU_LATE_DUST,Env:STARFOX_TEST_FAIL_LATE_GPU,Env:STARFOX_TEST_STEREO_OUTPUT -ErrorAction SilentlyContinue
            if($mode -eq 'cpu') {$env:STARFOX_DISABLE_GPU_LATE_DUST='1'}
            if($mode -in @('fallback','stereo-fallback')) {$env:STARFOX_TEST_FAIL_LATE_GPU='1'}
            if($mode -eq 'stereo-fallback') {$env:STARFOX_TEST_STEREO_OUTPUT='2'}
            $env:STARFOX_CAPTURE_PRESENTATION_PATH=Join-Path $proof "$experience-$mode.bmp"
            $log=Join-Path $proof "$experience-$mode.log"
            $process=Start-Process build/current/starfox_pc.exe -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardError $log
            $handle=$process.Handle
            if(!$process.WaitForExit(60000)) {throw "Capture still running: PID $($process.Id), $log"}
            if($process.ExitCode -ne 0) {throw "Capture failed: $log"}
            if($mode -eq 'gpu' -and !(Select-String -LiteralPath $log -SimpleMatch 'native-pipeline: GPU late margin stars' -Quiet)) {
                throw "Late GPU stars not selected: $log"
            }
            if($mode -in @('fallback','stereo-fallback') -and
                ((Select-String -LiteralPath $log -SimpleMatch 'native-pipeline: GPU late margin stars' -Quiet) -or
                !(Select-String -LiteralPath $log -Pattern 'native-pipeline: (GPU scene readback|CPU scene replay)' -Quiet))) {
                throw "Late GPU failure did not execute complete fallback: $log"
            }
            if($mode -eq 'stereo-fallback' -and !(Select-String -LiteralPath $log -SimpleMatch 'stereo failure: eye effects:' -Quiet)) {
                throw "Stereo failure was not exercised: $log"
            }
            $hashes[$mode]=(Get-FileHash -LiteralPath $env:STARFOX_CAPTURE_PRESENTATION_PATH -Algorithm SHA256).Hash
        }
        if($hashes.gpu -ne $hashes.cpu -or $hashes.gpu -ne $hashes.fallback -or $hashes.gpu -ne $hashes.'stereo-fallback') {
            throw "Game Over GPU/CPU/fallback mismatch: $experience"
        }
        Add-Type -AssemblyName System.Drawing
        $bitmap=[Drawing.Bitmap]::FromFile((Join-Path $proof "$experience-gpu.bmp"))
        try {
            $left=($bitmap.Width-256*$RenderScale)/2
            $right=$bitmap.Width-$left
            $ink=@(0,0)
            for($y=0;$y -lt $bitmap.Height;++$y) {
                for($x=0;$x -lt $left;++$x) {if(($bitmap.GetPixel($x,$y).ToArgb() -band 0xffffff) -ne 0){++$ink[0]}}
                for($x=$right;$x -lt $bitmap.Width;++$x) {if(($bitmap.GetPixel($x,$y).ToArgb() -band 0xffffff) -ne 0){++$ink[1]}}
            }
            # Native stars are sparse and moving; they need not occupy both
            # margins on the same frame. Require actual extended ink so an
            # empty GPU/CPU/fallback comparison cannot pass accidentally.
            if(($ink[0]+$ink[1]) -eq 0) {throw "Game Over fixture has no margin stars: $experience"}
        } finally {$bitmap.Dispose()}
        Write-Output "$experience Game Over: visible margins; GPU, CPU, mono/stereo forced fallback byte-identical"
    }
} finally {
    Get-ChildItem Env: | Where-Object {$_.Name -match '^(STARFOX_|SDL_AUDIODRIVER$)'} | ForEach-Object {Remove-Item -LiteralPath "Env:$($_.Name)"}
    foreach($key in $saved.Keys) {[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}
}
