param(
    [ValidateSet('ORIGINAL','EX','BOTH')][string]$Experience='BOTH',
    [ValidateRange(1,10000)][int]$Ticks=600,
    [string]$Executable='build/vr-dev/starfox_vr_runtime_check.exe',
    [switch]$VerifyGeometryCache
)
$ErrorActionPreference='Stop'
$passed=0
$failed=@()
foreach($variant in @('ORIGINAL','EX')) {
    if($Experience -ne 'BOTH' -and $Experience -ne $variant) {continue}
    $rom=if($variant -eq 'EX') {'tmp/runtime-inputs/starfox-ex/SFES.SFC'} else {'upstream-ultrastarfox/SF.SFC'}
    $symbols=if($variant -eq 'EX') {'assets/symbols/starfox-ex.txt'} else {'upstream-ultrastarfox/SYMBOLS.TXT'}
    $levels=@(Get-Content -LiteralPath $symbols | ForEach-Object {
        if($_ -match '^(LEVEL[1-7]_[1-9])\s') {$Matches[1]}
    } | Sort-Object -Unique)
    if(!$levels.Count) {throw "No numbered stages found for $variant"}
    foreach($level in $levels) {
        $arguments=@('--preflight',$rom,$symbols,$level,$Ticks)
        if($VerifyGeometryCache) {$arguments+='--verify-geometry-cache'}
        $output=& $Executable @arguments 2>&1
        $code=$LASTEXITCODE
        if($code -ne 0) {
            $failed+="$variant/$level"
            Write-Output "FAIL $variant/$level (exit $code)"
            Write-Output $output
        } else {
            ++$passed
            Write-Output "PASS $variant/$level ($Ticks source ticks, models/dust/grid assembly only)"
        }
    }
}
Write-Output "World preflight totals: $passed passed; $($failed.Count) failed. No GPU or headset verification."
if($failed.Count) {throw "Failed stages: $($failed -join ', ')"}
