# One-time bootstrap for the collision pipeline's Embree dependency.
#
# The visibility work links Embree 4 statically into ArcRaiders.exe (see
# Project/Project.vcxproj and docs in Project/Functions/CollisionBvh.cpp). The
# library is a third-party build product, so it is NOT committed: this script
# produces it into ThirdParty\, which .gitignore already excludes.
#
# It is idempotent: if the static libs are already there it says so and exits.
# Run it once per machine, then scripts\build.ps1 works as before.
#
# Why these options:
#   EMBREE_STATIC_LIB=ON      link into the single exe; no new DLL ships
#   EMBREE_TASKING_SYSTEM=INTERNAL  no TBB: no tbb12.dll dependency at all
#   EMBREE_ISPC_SUPPORT=OFF   no ISPC toolchain needed to build it
#   EMBREE_TUTORIALS=OFF      tutorials are examples, not needed here
#   EMBREE_MAX_ISA=NONE       build every CPU device, so the BVH picks the best
#                             one at runtime instead of being locked to one
#   geometry types off        the pipeline rasterises triangle meshes only, and
#                             each unused type is a large amount of code to
#                             compile for nothing

param(
    [string]$Version = "v4.4.0",
    [string]$RepoUrl = "https://github.com/RenderKit/embree.git"
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$srcDir = Join-Path $root "ThirdParty\embree"
$buildDir = Join-Path $root "ThirdParty\embree-build"
$releaseDir = Join-Path $buildDir "Release"

function Find-CMake {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsRoot = & $vswhere -latest -products * -property installationPath 2>$null
        if ($vsRoot) {
            $candidate = Join-Path $vsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
            if (Test-Path $candidate) { return $candidate }
        }
    }
    foreach ($p in @("$env:ProgramFiles\CMake\bin\cmake.exe", "$env:ProgramFiles(x86)\CMake\bin\cmake.exe")) {
        if (Test-Path $p) { return $p }
    }
    $onPath = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    throw "cmake.exe not found. Install the CMake component of Visual Studio, or CMake itself."
}

function Find-CMakeGenerator {
    param([string]$CMake)
    $generators = & $CMake --help 2>&1
    foreach ($name in @("Visual Studio 18 2026", "Visual Studio 17 2022")) {
        if ($generators -match [regex]::Escape($name)) { return $name }
    }
    throw "No supported Visual Studio CMake generator found (looked for 18 2026 and 17 2022)."
}

if ((Test-Path (Join-Path $releaseDir "embree4.lib")) -and
    (Test-Path (Join-Path $releaseDir "embree_sse42.lib")) -and
    (Test-Path (Join-Path $srcDir "include\embree4\rtcore.h"))) {
    Write-Host "[i] Embree static libs already present in $releaseDir - nothing to do."
    exit 0
}

$cmake = Find-CMake
$generator = Find-CMakeGenerator -CMake $cmake

if (-not (Test-Path (Join-Path $srcDir "CMakeLists.txt"))) {
    if (Test-Path $srcDir) {
        throw "$srcDir exists but has no CMakeLists.txt. Remove it and re-run, or point the project at a different Embree checkout."
    }
    Write-Host "[*] Cloning Embree $Version into $srcDir ..."
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $srcDir) | Out-Null
    & git clone --depth 1 --branch $Version $RepoUrl $srcDir
    if ($LASTEXITCODE -ne 0) { throw "git clone failed ($LASTEXITCODE)." }
} else {
    Write-Host "[i] Reusing existing Embree source at $srcDir"
}

Write-Host "[*] Configuring static Embree ($generator) ..."
& $cmake -S $srcDir -B $buildDir -G $generator -A x64 `
    -DEMBREE_STATIC_LIB=ON `
    -DEMBREE_ISPC_SUPPORT=OFF `
    -DEMBREE_TUTORIALS=OFF `
    -DEMBREE_TASKING_SYSTEM=INTERNAL `
    -DEMBREE_GEOMETRY_SUBDIVISION=OFF `
    -DEMBREE_GEOMETRY_GRID=OFF `
    -DEMBREE_GEOMETRY_POINT=OFF `
    -DEMBREE_GEOMETRY_HAIR=OFF `
    -DEMBREE_GEOMETRY_INSTANCE=OFF `
    -DEMBREE_MAX_ISA=NONE
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)." }

Write-Host "[*] Building Embree Release x64 (this takes a few minutes, once) ..."
& $cmake --build $buildDir --config Release --parallel 4
if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)." }

$libs = @(Get-ChildItem -LiteralPath $releaseDir -Filter *.lib -ErrorAction SilentlyContinue)
if ($libs.Count -eq 0) { throw "Build reported success but produced no .lib files in $releaseDir." }

Write-Host "[+] Embree static libraries ready in $releaseDir"
foreach ($lib in $libs) { Write-Host ("      {0}  ({1:N0} bytes)" -f $lib.Name, $lib.Length) }
Write-Host "[+] Now run scripts\build.ps1 as usual."
exit 0
