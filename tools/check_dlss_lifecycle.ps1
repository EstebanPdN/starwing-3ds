param([string]$OutputDirectory='tmp/dlss-game-lifecycle-fixed',[switch]$Evaluate,[switch]$CompareSerialized,[switch]$RequireContinuousHistory,[switch]$CaptureBackground,[switch]$DumpPpu,[switch]$AuditTerrain,
    [ValidateRange(1,10000)][int]$Frames=16,[ValidateRange(20,1000)][int]$PresentationFPS=60,
    [ValidatePattern('^[A-Za-z0-9_]+$')][string]$Level='LEVEL1_1',
    [ValidateSet('DLAA','QUALITY','BALANCED','PERFORMANCE')][string]$DlssMode='DLAA')
if($CompareSerialized -and !$Evaluate){throw 'Serialization comparison requires Evaluate'}
if($AuditTerrain -and (!$Evaluate -or $Frames -lt 2)){throw 'Terrain audit requires Evaluate and at least two frames'}
$ErrorActionPreference='Stop'
$proof=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $proof){throw 'Use a new proof directory'}
New-Item -ItemType Directory -Path $proof | Out-Null
$saved=@{}
Get-ChildItem Env: | Where-Object {$_.Name -match '^(STARFOX_|SDL_AUDIODRIVER$|SDL_GPU_DRIVER$)'} | ForEach-Object {
    $saved[$_.Name]=$_.Value;Remove-Item -LiteralPath "Env:$($_.Name)"
}
try {
    $settings=@{
        SDL_GPU_DRIVER='direct3d12';SDL_AUDIODRIVER='dummy';STARFOX_TEST_HIDDEN='1';STARFOX_TEST_FRAMES="$Frames"
        STARFOX_TEST_SKIP_PREROLL='1';STARFOX_TEST_PREROLL_TICKS='1000';STARFOX_TEST_RENDERER='GPU'
        STARFOX_TEST_UNPACED='1';STARFOX_TEST_MSU1='0';STARFOX_TEST_TEMPORAL_INPUTS='1';STARFOX_TRACE_GPU='1'
        STARFOX_TEST_DISPLAY_MODE='16_9';STARFOX_TEST_RENDER_SCALE='2';STARFOX_TEST_PRESENTATION_FPS="$PresentationFPS"
        STARFOX_TEST_TIMING_MODE='ORIGINAL';STARFOX_TEST_VSYNC='0';STARFOX_TEST_RAY_TRACING='0';STARFOX_TEST_STEREO_OUTPUT='0'
        STARFOX_TEST_DLSS_MODE=$DlssMode
        STARFOX_DLSS_ADAPTER=(Resolve-Path build/streamline-probe-msvc/starfox_dlss_native.dll).Path
        STARFOX_DLSS_BINARIES=(Resolve-Path tmp/streamline-sdk-2.14.1/sdk/bin/x64).Path
    }
    foreach($key in $settings.Keys){[Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')}
    foreach($experience in @('ORIGINAL','EX')) {
        $env:STARFOX_TEST_EXPERIENCE=$experience
        $arguments=if($experience -eq 'EX'){"tmp/runtime-inputs/starfox-ex/SFES.SFC assets/symbols/starfox-ex.txt $Level"}
            else{"upstream-ultrastarfox/SF.SFC upstream-ultrastarfox/SYMBOLS.TXT $Level"}
        $hashes=@{}
        $modes=if($CompareSerialized){@('off','on','serialized')}else{@('off','on')}
        foreach($mode in $modes) {
            $env:STARFOX_TEST_DLSS_LIFECYCLE=if($mode -ne 'off'){'1'}else{$null}
            $env:STARFOX_TEST_DLSS_EVALUATE=if($mode -ne 'off' -and $Evaluate){'1'}else{$null}
            $env:STARFOX_TEST_DLSS_SERIALIZE=if($mode -eq 'serialized'){'1'}else{$null}
            $env:STARFOX_TEST_DLSS_AUDIT_TERRAIN=if($AuditTerrain -and $mode -ne 'off'){'1'}else{$null}
            $env:STARFOX_TEST_DLSS_WORLD_CAPTURE=if($mode -eq 'on' -and $Evaluate){Join-Path $proof "$experience-world.bmp"}else{$null}
            $env:STARFOX_CAPTURE_PRESENTATION_PATH=Join-Path $proof "$experience-$mode.bmp"
            $env:STARFOX_CAPTURE_TITLE_LAYERS=if($CaptureBackground){Join-Path $proof "$experience-$mode"}else{$null}
            $env:STARFOX_TEST_PPU_DUMP=if($DumpPpu){'1'}else{$null}
            $env:STARFOX_CAPTURE_DIR=if($DumpPpu){Join-Path $proof "$experience-$mode-ppu"}else{$null}
            $env:STARFOX_CAPTURE_START=if($DumpPpu){[string]($Frames-1)}else{$null}
            $env:STARFOX_CAPTURE_INTERVAL=if($DumpPpu){[string]$Frames}else{$null}
            $log=Join-Path $proof "$experience-$mode.log"
            $p=Start-Process build/current/starfox_pc.exe -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardError $log -RedirectStandardOutput (Join-Path $proof "$experience-$mode-stdout.log")
            $handle=$p.Handle
            if(!$p.WaitForExit(60000)){throw "Still running PID $($p.Id): $log"}
            if($p.ExitCode -ne 0){throw "Game failed ($($p.ExitCode)): $log"}
            if($mode -ne 'off') {
                foreach($message in @('initialized before SDL','actual game GPU bound','shutdown before renderer destruction','dlss-presentation: upgraded','dlss-presentation: restored')) {
                    if(!(Select-String -LiteralPath $log -SimpleMatch $message -Quiet)){throw "Missing $message in $log"}
                }
                if($Evaluate -and (!(Select-String -LiteralPath $log -SimpleMatch 'dlss-gameplay: evaluated' -Quiet) -or
                    (Select-String -LiteralPath $log -Pattern 'dlss-gameplay: failed|dlss-sdk-error:' -Quiet))) {throw "Gameplay evaluation failed/missing: $log"}
                if($Evaluate) {
                    $expectedMode=@{QUALITY=1;BALANCED=2;PERFORMANCE=3;DLAA=4}[$DlssMode]
                    if(!(Select-String -LiteralPath $log -Pattern " mode=$expectedMode render=[1-9][0-9]*x[1-9][0-9]*" -Quiet)) {
                        throw "Requested DLSS mode was not evaluated: $log"
                    }
                    if($AuditTerrain -and !(Select-String -LiteralPath $log -Pattern 'dlss-terrain-audit: covered=[1-9][0-9]* valid_depth=[1-9][0-9]*' -Quiet)) {
                        throw "Missing usable terrain depth: $log"
                    }
                    if($AuditTerrain -and !(Select-String -LiteralPath $log -Pattern 'valid_motion=[1-9][0-9]* moving=[0-9]+ mismatches=0' -Quiet)) {
                        throw "Missing verified terrain motion: $log"
                    }
                    $evaluations=@(Select-String -LiteralPath $log -SimpleMatch 'dlss-gameplay: evaluated')
                    if($evaluations.Count -ne $Frames){throw "Expected $Frames evaluations, got $($evaluations.Count): $log"}
                    if($RequireContinuousHistory -and @($evaluations | Where-Object {$_.Line -match 'reset=1'}).Count -ne 1){
                        throw "Expected only initial history reset in stable scene: $log"
                    }
                }
            }
            $hashes[$mode]=(Get-FileHash -LiteralPath $env:STARFOX_CAPTURE_PRESENTATION_PATH).Hash
        }
        if(!$Evaluate -and $hashes.off -ne $hashes.on){throw "SDK lifecycle changed gameplay: $experience"}
        if($Evaluate -and $hashes.off -eq $hashes.on){throw "Evaluated output was not displayed: $experience"}
        if($CompareSerialized -and $hashes.on -ne $hashes.serialized){throw "Queue-ordered/serialized output differs: $experience"}
        "${experience}: SDK startup/binding/shutdown passed; evaluated=$Evaluate"
    }
} finally {
    Get-ChildItem Env: | Where-Object {$_.Name -match '^(STARFOX_|SDL_AUDIODRIVER$|SDL_GPU_DRIVER$)'} | ForEach-Object {Remove-Item -LiteralPath "Env:$($_.Name)"}
    foreach($key in $saved.Keys){[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}
}
