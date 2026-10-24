<#
.SYNOPSIS
    Reproducible release packager for KlyrDB on Windows x64.
.DESCRIPTION
    Configures, builds, tests, installs, packages, and generates SHA-256 checksums
    for the official KlyrDB release distribution archive.
#>

param(
    [string]$Version = "0.12.0",
    [string]$BuildType = "Release"
)

$ErrorActionPreference = "Stop"

$RootDir = Split-Path -Parent $PSScriptRoot
Set-Location $RootDir

Write-Host "=== Packaging KlyrDB Release v$Version ($BuildType) ===" -ForegroundColor Cyan

# Locate cmake and ctest executables
$CMakeExe = "cmake"
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    $VsCMake = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (Test-Path $VsCMake) {
        $CMakeExe = $VsCMake
    }
}

$CTestExe = "ctest"
if (-not (Get-Command ctest -ErrorAction SilentlyContinue)) {
    $VsCTest = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
    if (Test-Path $VsCTest) {
        $CTestExe = $VsCTest
    }
}

$BuildDir = Join-Path $RootDir "build_$($BuildType.ToLower())"
$DistDir = Join-Path $RootDir "dist"
$PackageName = "klyrdb-$Version-windows-x64"
$StagingDir = Join-Path $DistDir $PackageName

# 1. Clean previous staging
if (Test-Path $StagingDir) {
    Remove-Item -Recurse -Force $StagingDir
}
New-Item -ItemType Directory -Force -Path $StagingDir | Out-Null
New-Item -ItemType Directory -Force -Path $DistDir | Out-Null

# 2. Configure Release Build
Write-Host "[1/6] Configuring CMake for $BuildType..." -ForegroundColor Yellow
& $CMakeExe -B $BuildDir -S $RootDir -DCMAKE_BUILD_TYPE=$BuildType -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

# 3. Build Binaries
Write-Host "[2/6] Building $BuildType targets..." -ForegroundColor Yellow
& $CMakeExe --build $BuildDir --config $BuildType
if ($LASTEXITCODE -ne 0) { throw "CMake build failed" }

# 4. Verify CTest Suite
Write-Host "[3/6] Running full test suite..." -ForegroundColor Yellow
& $CTestExe --test-dir $BuildDir -C $BuildType --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Test suite failed - aborting release packaging" }

# 5. Install to Staging Prefix
Write-Host "[4/6] Installing artifacts to staging directory..." -ForegroundColor Yellow
& $CMakeExe --install $BuildDir --config $BuildType --prefix $StagingDir
if ($LASTEXITCODE -ne 0) { throw "CMake install failed" }

# Copy Examples, Docs and Licenses into package
Write-Host "[5/6] Copying developer documentation and consumer examples..." -ForegroundColor Yellow
Copy-Item (Join-Path $RootDir "README.md") -Destination $StagingDir
if (Test-Path (Join-Path $RootDir "LICENSE")) {
    Copy-Item (Join-Path $RootDir "LICENSE") -Destination $StagingDir
}
Copy-Item -Recurse (Join-Path $RootDir "examples") -Destination (Join-Path $StagingDir "examples")

# Purge any build directories, compiler intermediates, CMake caches, and temporary databases
$ExcludeDirs = @("build", "build_*", ".vs", "x64", "Debug", "Release", "out", "bin", "lib", ".cache")
Get-ChildItem -Path (Join-Path $StagingDir "examples") -Recurse -Directory | Where-Object {
    $dirName = $_.Name
    foreach ($pattern in $ExcludeDirs) {
        if ($dirName -like $pattern) { return $true }
    }
    return $false
} | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue

$ExcludeExts = @("*.obj", "*.ilk", "*.pdb", "*.tlog", "*.idb", "*.lastbuildstate", "*.vcxproj*", "*.sln", "*.slnx", "*.user", "*.suo", "CMakeCache.txt", "cmake_install.cmake", "CTestTestfile.cmake", "*.kdb", "*.wal", "*.lock", "*.tmp", "*.bak")
Get-ChildItem -Path (Join-Path $StagingDir "examples") -Recurse -File | Where-Object {
    $fileName = $_.Name
    foreach ($pattern in $ExcludeExts) {
        if ($fileName -like $pattern) { return $true }
    }
    return $false
} | Remove-Item -Force -ErrorAction SilentlyContinue

Copy-Item -Recurse (Join-Path $RootDir "docs") -Destination (Join-Path $StagingDir "docs")

# 6. Create Zip Archive and SHA-256 Checksum
Write-Host "[6/6] Generating archive and SHA-256 checksum..." -ForegroundColor Yellow
$ZipPath = Join-Path $DistDir "$PackageName.zip"
if (Test-Path $ZipPath) {
    Remove-Item -Force $ZipPath
}

Compress-Archive -Path "$StagingDir\*" -DestinationPath $ZipPath -Force
$Hash = (Get-FileHash -Path $ZipPath -Algorithm SHA256).Hash
$ChecksumFile = "$ZipPath.sha256"
"$Hash  $PackageName.zip" | Out-File -FilePath $ChecksumFile -Encoding utf8

Write-Host "`n[+] Release Package Created Successfully!" -ForegroundColor Green
Write-Host "    Archive:  $ZipPath"
Write-Host "    SHA-256:  $Hash"
Write-Host "    Checksum: $ChecksumFile`n"
