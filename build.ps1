# war3d 构建脚本（PowerShell 版）
# 全程使用绝对路径，自行装配 MSVC 环境变量，不依赖 PATH、不调用 cmd /c
param(
    [switch]$Clean,
    [switch]$Debug
)

$ErrorActionPreference = "Stop"

$VSDIR = "F:\vs"
$MSVC = (Get-ChildItem "$VSDIR\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1).FullName
$CL = Join-Path $MSVC "bin\Hostx64\x64\cl.exe"
$CMAKE = Join-Path $VSDIR "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$NINJA = Join-Path $VSDIR "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

foreach ($p in @($CL, $CMAKE, $NINJA)) {
    if (-not (Test-Path $p)) { throw "找不到工具：$p" }
}

$KITROOT = "C:\Program Files (x86)\Windows Kits\10"
$SDK = (Get-ChildItem (Join-Path $KITROOT "Include") -Directory | Sort-Object Name | Select-Object -Last 1).Name

$env:INCLUDE = (@(
    (Join-Path $MSVC "include"),
    (Join-Path $KITROOT "Include\$SDK\ucrt"),
    (Join-Path $KITROOT "Include\$SDK\um"),
    (Join-Path $KITROOT "Include\$SDK\shared"),
    (Join-Path $KITROOT "Include\$SDK\winrt")
) -join ";")

$env:LIB = (@(
    (Join-Path $MSVC "lib\x64"),
    (Join-Path $KITROOT "Lib\$SDK\ucrt\x64"),
    (Join-Path $KITROOT "Lib\$SDK\um\x64")
) -join ";")

$SDKBIN = Join-Path $KITROOT "bin\$SDK\x64"
$RC = Join-Path $SDKBIN "rc.exe"
$MT = Join-Path $SDKBIN "mt.exe"
foreach ($p in @($RC, $MT)) {
    if (-not (Test-Path $p)) { throw "找不到 SDK 工具：$p" }
}
# 传给 CMake 的路径统一用正斜杠，否则缓存文件里 '\P' 会被当成转义序列
$RCCM = $RC -replace '\\', '/'
$MTCM = $MT -replace '\\', '/'

# 只保留 MSVC 与 SDK 的路径，避免 PATH 里其它 rc.exe / link.exe（如 Open Watcom）被误用
$env:PATH = (@(
    (Join-Path $MSVC "bin\Hostx64\x64"),
    $SDKBIN,
    "C:\Windows\system32",
    "C:\Windows"
) -join ";")

$ROOT = $PSScriptRoot
$BUILD = Join-Path $ROOT "build"
$BUILDTYPE = "Release"
if ($Debug) { $BUILDTYPE = "Debug" }

if ($Clean -and (Test-Path $BUILD)) { Remove-Item $BUILD -Recurse -Force }

& $CMAKE -G Ninja -S $ROOT -B $BUILD `
    "-DCMAKE_BUILD_TYPE=$BUILDTYPE" `
    "-DCMAKE_MAKE_PROGRAM=$NINJA" `
    "-DCMAKE_C_COMPILER=$CL" `
    "-DCMAKE_CXX_COMPILER=$CL" `
    "-DCMAKE_RC_COMPILER=$RCCM" `
    "-DCMAKE_MT=$MTCM"
if ($LASTEXITCODE -ne 0) { throw "CMake 配置失败" }

& $CMAKE --build $BUILD
if ($LASTEXITCODE -ne 0) { throw "构建失败" }

Write-Host ""
Write-Host "[ok] 输出：$BUILD\war3d.exe"
