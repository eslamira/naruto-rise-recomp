# Builds VK_LAYER_KHRONOS_validation from the Khronos sources at a pinned SDK tag,
# for the Vulkan proofs of the native renderer (the machine has no Vulkan SDK).
# The layer is not installed system-wide: a run points VK_ADD_LAYER_PATH at
# <Root>/install/bin and enables it with VK_LOADER_LAYERS_ENABLE.
# Usage: pwsh -NoProfile -File build-validation-layer.ps1 -Root <dir> [-Tag vulkan-sdk-1.4.363.0]
param(
    [Parameter(Mandatory = $true)][string]$Root,
    [string]$Tag = 'vulkan-sdk-1.4.363.0'
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $Root | Out-Null
Set-Location -LiteralPath $Root
$vcEnvLines = & "$env:SystemRoot\System32\cmd.exe" /d /s /c '"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64 >nul && set'
if ($LASTEXITCODE -ne 0) { throw 'Visual Studio environment initialization failed' }
foreach ($vcEnvLine in $vcEnvLines) {
    if ($vcEnvLine -match '^(INCLUDE|LIB|LIBPATH|PATH)=(.*)$') {
        [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}
$cmakeExe = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ninjaExe = 'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
if (-not (Test-Path -LiteralPath src)) {
    & git clone --depth 1 --branch $Tag https://github.com/KhronosGroup/Vulkan-ValidationLayers.git src *> clone.log
    if ($LASTEXITCODE -ne 0) { Get-Content clone.log -Tail 20; throw 'Clone failed' }
}
& git -C src rev-parse HEAD | Set-Content -Encoding ascii source-commit.txt
& $cmakeExe -S src -B build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninjaExe" -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DUPDATE_DEPS=ON -DBUILD_WERROR=OFF -DBUILD_TESTS=OFF `
    "-DCMAKE_INSTALL_PREFIX=$Root/install" *> configure.log
if ($LASTEXITCODE -ne 0) { Get-Content configure.log -Tail 40; throw 'Configure failed' }
& $cmakeExe --build build --parallel 12 *> build.log
if ($LASTEXITCODE -ne 0) { Get-Content build.log -Tail 40; throw 'Build failed' }
& $cmakeExe --install build *> install.log
if ($LASTEXITCODE -ne 0) { Get-Content install.log -Tail 20; throw 'Install failed' }
Get-ChildItem -LiteralPath "$Root/install/bin" | ForEach-Object {
    '{0}  {1}' -f (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLower(), $_.Name
} | Set-Content -Encoding ascii install-sha256.txt
Get-Content install-sha256.txt
