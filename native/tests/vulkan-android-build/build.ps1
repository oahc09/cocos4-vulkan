# Reproducible Android arm64 cocos_engine build + final-link probe (M0-G)
# Usage: powershell -ExecutionPolicy Bypass -File build.ps1
[CmdletBinding()]
param(
    [string]$Ndk = "D:/Android/Sdk/ndk/26.1.10909125",
    [int]$Jobs = 4
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path "$PSScriptRoot\..\..\..").Path
$buildDir = Join-Path $repo 'native\build\vulkan-modernization\android-engine'
$cacheDir = Join-Path $repo 'native\build\vulkan-modernization\upstream'

Write-Host "repo: $repo"
Write-Host "build: $buildDir"

& cmake -S "$repo\native\tests\vulkan-android-build" -B $buildDir -G Ninja `
    "-DCMAKE_TOOLCHAIN_FILE=$Ndk/build/cmake/android.toolchain.cmake" `
    "-DANDROID_ABI=arm64-v8a" "-DANDROID_PLATFORM=android-26" "-DANDROID_STL=c++_static" `
    "-DCMAKE_BUILD_TYPE=Release" "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" `
    "-DCC_SHADER_UPSTREAM_CACHE=$cacheDir" `
    "-DUSE_PHYSICS_PHYSX=OFF"
if ($LASTEXITCODE -ne 0) { throw "configure failed: $LASTEXITCODE" }

& cmake --build $buildDir --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw "build failed: $LASTEXITCODE" }

Write-Host "OK: $buildDir\libvk_android_probe.so"
exit 0
