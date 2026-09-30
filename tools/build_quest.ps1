param(
    [Parameter(Mandatory=$true)][string]$JdkRoot,
    [Parameter(Mandatory=$true)][string]$SdkRoot
)
$ErrorActionPreference='Stop'
$questSource=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$questJdk=(Resolve-Path $JdkRoot).Path
$questSdk=(Resolve-Path $SdkRoot).Path
if(!(Test-Path (Join-Path $questJdk 'bin/javac.exe'))){throw 'JdkRoot must contain a full JDK (17 or newer), not a JRE'}
$questAar=Join-Path $questSource 'platform/android/app/libs/SDL3-3.4.14.aar'
if(!(Test-Path $questAar)){throw 'Missing pinned SDL AAR. Prepare the SDL dependency with tools/build_android.sh or the documented verified archive.'}
# Set the toolchain only for this process and restore the caller's environment.
$questOldJava=$env:JAVA_HOME
$questOldSdk=$env:ANDROID_HOME
$questOldSdkRoot=$env:ANDROID_SDK_ROOT
try {
    $env:JAVA_HOME=$questJdk
    $env:ANDROID_HOME=$questSdk
    $env:ANDROID_SDK_ROOT=$questSdk
    & (Join-Path $questSource 'platform/android/gradlew.bat') -p (Join-Path $questSource 'platform/android') --no-daemon ':quest:assembleDebug'
    if($LASTEXITCODE -ne 0){throw "Quest build failed ($LASTEXITCODE)"}
    $questApk=Join-Path $questSource 'platform/quest/build/outputs/apk/debug/quest-debug.apk'
    if(!(Test-Path $questApk)){throw 'Gradle returned success without the Quest APK'}
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $questPayload=[IO.Compression.ZipFile]::OpenRead($questApk)
    try {
        foreach($questEntry in @('lib/arm64-v8a/libstarfox_quest.so','lib/arm64-v8a/libSDL3.so','AndroidManifest.xml')) {
            $questItem=$questPayload.GetEntry($questEntry)
            if(!$questItem -or $questItem.Length -eq 0){throw "Quest APK is missing $questEntry"}
        }
        if($questPayload.GetEntry('lib/arm64-v8a/libmain.so')){throw 'Quest APK unexpectedly contains the flat game entry library'}
        foreach($questEntry in $questPayload.Entries) {
            if($questEntry.FullName -match '^lib/[^/]+/lib(android|log)\.so$') {
                throw "Quest APK contains an NDK platform link stub: $($questEntry.FullName)"
            }
        }
    } finally {$questPayload.Dispose()}
    Write-Output "Development APK: $questApk"
    Write-Output 'Building an APK does not verify headset rendering or presentation parity.'
} finally {
    $env:JAVA_HOME=$questOldJava
    $env:ANDROID_HOME=$questOldSdk
    $env:ANDROID_SDK_ROOT=$questOldSdkRoot
}
