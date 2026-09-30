param([string]$OutputDirectory='tmp/ray-tracing-proof', [ValidateRange(1,600)][int]$Frames=60,
    [ValidateSet('ORIGINAL','EX')][string]$Experience='ORIGINAL',
    [ValidatePattern('^[A-Z0-9_]+$')][string]$Level='LEVEL1_1',
    [ValidateRange(0,100000)][int]$PrerollTicks=1000,
    [ValidateRange(1,4)][int]$RenderScale=2,
    [switch]$CompareCpu, [switch]$CompareGeometry, [switch]$ComparePresentation,
    [switch]$RequireResidentGeometry)
$ErrorActionPreference='Stop'
$proofPath=[IO.Path]::GetFullPath((Join-Path $OutputDirectory ([guid]::NewGuid().ToString('N'))))
New-Item -ItemType Directory -Path $proofPath -Force | Out-Null
$savedEnvironment=@{}
Get-ChildItem Env: | Where-Object {$_.Name -match '^(STARFOX_|SDL_AUDIODRIVER$)'} | ForEach-Object {
    $savedEnvironment[$_.Name]=$_.Value
    Remove-Item -LiteralPath "Env:$($_.Name)"
}
try {
$env:SDL_AUDIODRIVER='dummy'
$env:STARFOX_TEST_HIDDEN='1'
$env:STARFOX_TEST_FRAMES="$Frames"
$env:STARFOX_TEST_PREROLL_TICKS="$PrerollTicks"
$env:STARFOX_TEST_SKIP_PREROLL='1'
$env:STARFOX_TEST_EXPERIENCE=$Experience
$env:STARFOX_TEST_RENDERER='GPU'
$env:STARFOX_TEST_DISPLAY_MODE='16_9'
$env:STARFOX_TEST_TIMING_MODE='ORIGINAL'
$env:STARFOX_TEST_PRESENTATION_FPS='60'
$env:STARFOX_TEST_UNPACED='1'
$env:STARFOX_TEST_VSYNC='0'
$env:STARFOX_TEST_RENDER_SCALE="$RenderScale"
$env:STARFOX_TEST_BLOOM='0'
$env:STARFOX_TEST_EFFECT='0'
$env:STARFOX_TEST_WORLD_EFFECT='0'
$env:STARFOX_TEST_MODEL_SMOOTHING='0'
$env:STARFOX_TEST_HDR_EFFECT='0'
$env:STARFOX_TEST_CHROMATIC_ABERRATION='0'
$env:STARFOX_TRACE_GPU='1'
$env:STARFOX_TRACE_GPU_RAYS=if($RequireResidentGeometry){'1'}else{$null}
Remove-Item Env:STARFOX_DISABLE_DXR,Env:STARFOX_DISABLE_GPU_NATIVE,Env:STARFOX_CAPTURE_DIR -ErrorAction SilentlyContinue
$hashes=@{}
$modes=@('off','legacy','dxr','resident','unavailable')
if($CompareCpu) {$modes+= 'reference'}
if($CompareGeometry) {$modes+= 'cpu-geometry'}
if($ComparePresentation) {$modes+= 'cpu-presentation'}
foreach($mode in $modes) {
    # The removed override must not re-enable shadows independently of DXR.
    $env:STARFOX_TEST_ENHANCED_SHADOWS=if($mode -eq 'legacy') {'1'} else {'0'}
    $env:STARFOX_TEST_RAY_TRACING=if($mode -in @('dxr','resident','unavailable','reference','cpu-geometry','cpu-presentation')) {'1'} else {'0'}
    if($mode -eq 'cpu-presentation') {$env:STARFOX_DISABLE_GPU_NATIVE='1'}
    else {Remove-Item Env:STARFOX_DISABLE_GPU_NATIVE -ErrorAction SilentlyContinue}
    if($mode -eq 'cpu-geometry') {$env:STARFOX_DISABLE_GPU_GEOMETRY='1'}
    else {Remove-Item Env:STARFOX_DISABLE_GPU_GEOMETRY -ErrorAction SilentlyContinue}
    if($mode -eq 'reference') {$env:STARFOX_TEST_SHADOW_REFERENCE='1'}
    else {Remove-Item Env:STARFOX_TEST_SHADOW_REFERENCE -ErrorAction SilentlyContinue}
    if($mode -eq 'unavailable') {$env:STARFOX_DISABLE_DXR='1'}
    else {Remove-Item Env:STARFOX_DISABLE_DXR -ErrorAction SilentlyContinue}
    $capture=Join-Path $proofPath "$mode.bmp"
    $log=Join-Path $proofPath "$mode.log"
    $env:STARFOX_CAPTURE_PRESENTATION_PATH=$capture
    $env:STARFOX_TEST_SHADOW_MASK=Join-Path $proofPath "$mode-mask.bmp"
    if($mode -eq 'resident') {Remove-Item Env:STARFOX_TEST_SHADOW_MASK -ErrorAction SilentlyContinue}
    $cartridgeArguments=if($Experience -eq 'EX') {
        "tmp/runtime-inputs/starfox-ex/SFES.SFC assets/symbols/starfox-ex.txt $Level"
    } else { "upstream-ultrastarfox/SF.SFC upstream-ultrastarfox/SYMBOLS.TXT $Level" }
    $process=Start-Process build/current/starfox_pc.exe -ArgumentList $cartridgeArguments -WindowStyle Hidden -PassThru -RedirectStandardError $log
    $processHandle=$process.Handle
    if(!$process.WaitForExit(60000)) {throw "Capture still running: PID $($process.Id), $log"}
    if($process.ExitCode -ne 0) {throw "Capture failed: $mode, $log"}
    if($mode -eq 'dxr' -and !(Select-String -Path $log -Pattern 'shadow-backend: (Hardware DXR 1.1:|GPU-resident hardware DXR shadows)' -Quiet)) {
        throw "Hardware DXR did not execute: $log"
    }
    if($mode -eq 'dxr' -and ($Level -like 'LEVEL*' -or $Level -eq 'TRAININGMAP') -and !(Select-String -Path $log -Pattern 'shadow-receivers: ground=[1-9][0-9]* model=' -Quiet)) {
        throw "Hardware DXR did not produce a ground shadow in the gameplay fixture: $log"
    }
    if($mode -eq 'unavailable' -and (Select-String -Path $log -Pattern 'shadow-backend:' -Quiet)) {
        throw "Unavailable DXR unexpectedly selected a replacement shadow backend: $log"
    }
    $hashes[$mode]=(Get-FileHash -LiteralPath $capture -Algorithm SHA256).Hash
    if($mode -eq 'resident' -and !(Select-String -Path $log -Pattern 'shadow-backend: GPU-resident hardware DXR shadows' -Quiet)) {
        throw "Normal resident DXR path did not execute: $log"
    }
    if($mode -eq 'resident' -and $RequireResidentGeometry -and !(Select-String -Path $log -SimpleMatch 'GPU caster geometry' -Quiet)) {
        throw "Resident caster geometry did not reach DXR: $log"
    }
    if($mode -eq 'reference') {
        $comparison=Select-String -Path $log -Pattern 'shadow-reference:|shadow-float-geometry-reference:'
        if(!$comparison) {throw "Live CPU/DXR comparison did not execute: $log"}
        $comparison.Line | Write-Output
    }
}
if($CompareCpu) {Write-Output "CPU-reference/DXR presentation byte-identical: $($hashes.reference -eq $hashes.dxr)"}
if($CompareGeometry) {Write-Output "CPU/GPU geometry with DXR presentation byte-identical: $($hashes.'cpu-geometry' -eq $hashes.dxr)"}
if($ComparePresentation) {Write-Output "CPU/GPU composition with DXR presentation byte-identical: $($hashes.'cpu-presentation' -eq $hashes.dxr)"}
if($hashes.off -eq $hashes.dxr) {throw "Ray tracing did not change the displayed frame: $proofPath"}
if($hashes.resident -ne $hashes.dxr) {throw "Resident/readback DXR presentation mismatch: $proofPath"}
if($hashes.off -ne $hashes.legacy) {throw "Removed Enhanced Shadows override still affects output: $proofPath"}
if($hashes.off -ne $hashes.unavailable) {throw "Unavailable DXR did not preserve native-shadow presentation: $proofPath"}
if($Experience -eq 'ORIGINAL' -and $Level -eq 'LEVEL1_1' -and $PrerollTicks -eq 300 -and $Frames -eq 6 -and $RenderScale -eq 2) {
    # This floor sample is beside the left pillar, outside native silhouettes.
    # Counting a nonempty mask alone missed the scenery-tag regression.
    Add-Type -AssemblyName System.Drawing
    $floorImages=@()
    try {
        foreach($name in @('off','dxr','dxr-mask')) {
            $floorImages+=[Drawing.Bitmap]::FromFile((Join-Path $proofPath "$name.bmp"))
        }
        $before=$floorImages[0].GetPixel(150,275)
        $after=$floorImages[1].GetPixel(150,275)
        $shade=$floorImages[2].GetPixel(150,275).R
        if($shade -lt 100) {throw 'Pillar fixture no longer covers the ground sample'}
        foreach($channel in @('R','G','B')) {
            $expected=[math]::Floor($before.$channel*(255-$shade)/255)
            if([math]::Abs($after.$channel-$expected) -gt 1) {
                throw "Pillar ground mask was not composited: $channel expected $expected, got $($after.$channel)"
            }
        }
        Write-Output 'Pillar ground shadow reaches the displayed grass with all other visual effects disabled.'
    } finally {foreach($item in $floorImages) {$item.Dispose()}}
}
Write-Output "DXR changed the final presentation; resident/readback outputs match; removed Enhanced Shadows override has no effect; unavailable DXR preserves native output. Proof: $proofPath"
} finally {
    Get-ChildItem Env: | Where-Object {$_.Name -match '^(STARFOX_|SDL_AUDIODRIVER$)'} | ForEach-Object {
        Remove-Item -LiteralPath "Env:$($_.Name)"
    }
    foreach($entry in $savedEnvironment.GetEnumerator()) {Set-Item -LiteralPath "Env:$($entry.Key)" -Value $entry.Value}
}
