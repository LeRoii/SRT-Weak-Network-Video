param(
    [string]$Config = "Release",
    [string]$BuildDir = "build",
    [string]$OutputDir = "dist",
    [string]$Triplet = "x64-windows",
    [switch]$NoZip
)

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildRoot = Join-Path $RepoRoot $BuildDir
$ExePath = Join-Path $BuildRoot "$Config\srt_weak_video.exe"
$ConfigPath = Join-Path $RepoRoot "runtime-config.yaml"
$PackageDir = Join-Path $RepoRoot $OutputDir
$ZipPath = Join-Path $RepoRoot "$OutputDir.zip"

function Copy-IfExists {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    if (Test-Path $Path) {
        Copy-Item -Path $Path -Destination $Destination -Force
    }
}

function Copy-DllsFromDir {
    param(
        [Parameter(Mandatory = $true)][string]$Dir,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    if (Test-Path $Dir) {
        Get-ChildItem -Path $Dir -Filter "*.dll" -File |
            Copy-Item -Destination $Destination -Force
    }
}

if (-not (Test-Path $ExePath)) {
    throw "Executable not found: $ExePath. Build first with: cmake --build build --config $Config"
}

if (-not (Test-Path $ConfigPath)) {
    throw "runtime-config.yaml not found: $ConfigPath"
}

if (Test-Path $PackageDir) {
    Remove-Item -Path $PackageDir -Recurse -Force
}
New-Item -Path $PackageDir -ItemType Directory | Out-Null

Copy-Item -Path $ExePath -Destination $PackageDir -Force
Copy-Item -Path $ConfigPath -Destination $PackageDir -Force

# Copy DLLs that CMake/vcpkg may have already placed beside the executable.
Copy-DllsFromDir -Dir (Split-Path $ExePath -Parent) -Destination $PackageDir

# Copy vcpkg dynamic runtime dependencies.
$VcpkgRoot = $env:VCPKG_ROOT
if ($VcpkgRoot) {
    Copy-DllsFromDir -Dir (Join-Path $VcpkgRoot "installed\$Triplet\bin") -Destination $PackageDir
}

# Copy the bundled FFmpeg shared build, when present.
Copy-DllsFromDir -Dir (Join-Path $RepoRoot "ffmpeg-8.1.1-full_build-shared\bin") -Destination $PackageDir

# Copy the common MSVC runtime DLLs for machines without the VC++ redistributable.
$System32 = Join-Path $env:WINDIR "System32"
foreach ($Dll in @(
    "vcruntime140.dll",
    "vcruntime140_1.dll",
    "msvcp140.dll",
    "concrt140.dll"
)) {
    Copy-IfExists -Path (Join-Path $System32 $Dll) -Destination $PackageDir
}

if (-not $NoZip) {
    if (Test-Path $ZipPath) {
        Remove-Item -Path $ZipPath -Force
    }
    $Compressed = $false
    for ($Attempt = 1; $Attempt -le 5; $Attempt++) {
        try {
            Compress-Archive -Path (Join-Path $PackageDir "*") -DestinationPath $ZipPath -Force
            $Compressed = $true
            break
        } catch {
            if ($Attempt -eq 5) {
                throw
            }
            Start-Sleep -Seconds 2
        }
    }
}

$DllCount = (Get-ChildItem -Path $PackageDir -Filter "*.dll" -File).Count
Write-Host "Packaged: $PackageDir"
Write-Host "DLL count: $DllCount"
if (-not $NoZip) {
    Write-Host "Archive: $ZipPath"
}
Write-Host "Test the package on a clean Windows machine or VM before sharing."
