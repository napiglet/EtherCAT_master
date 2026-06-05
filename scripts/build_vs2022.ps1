param(
  [switch]$Clean,
  [switch]$OpenSolution
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $ScriptDir
$Preset = "windows-msvc-vs2022"
$BuildRoot = Join-Path $RepoRoot "build"
$BuildDir = Join-Path $RepoRoot "build\$Preset"

function Find-CMake {
  $cmd = Get-Command cmake -ErrorAction SilentlyContinue
  if ($cmd) {
    return $cmd.Source
  }

  $candidates = @(
    "C:\Program Files\CMake\bin\cmake.exe",
    "C:\Program Files (x86)\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
  )

  foreach ($candidate in $candidates) {
    if (Test-Path -LiteralPath $candidate) {
      return $candidate
    }
  }

  throw "CMake was not found. Install CMake or add cmake.exe to PATH."
}

function Remove-BuildDirectory {
  param([string]$Path)

  $resolvedRoot = (Resolve-Path -LiteralPath $RepoRoot).Path
  $parent = Split-Path -Parent $Path
  if (-not (Test-Path -LiteralPath $parent)) {
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
  }

  if (Test-Path -LiteralPath $Path) {
    $resolvedTarget = (Resolve-Path -LiteralPath $Path).Path
    if (-not $resolvedTarget.StartsWith($resolvedRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
      throw "Refusing to remove a build directory outside the repository: $resolvedTarget"
    }
    Remove-Item -LiteralPath $resolvedTarget -Recurse -Force
  }
}

$cmake = Find-CMake

if ($Clean) {
  Write-Host "Removing generated build directory: $BuildRoot"
  Remove-BuildDirectory -Path $BuildRoot
}

Write-Host "Repository root: $RepoRoot"
Write-Host "Configuring CMake preset: $Preset"
& $cmake --preset $Preset
if ($LASTEXITCODE -ne 0) {
  throw "CMake configure failed with exit code $LASTEXITCODE"
}

Write-Host "Building CMake preset: $Preset"
& $cmake --build --preset $Preset
if ($LASTEXITCODE -ne 0) {
  throw "CMake build failed with exit code $LASTEXITCODE"
}

$solution = Join-Path $BuildDir "SOEM.sln"
Write-Host ""
Write-Host "Build complete."
Write-Host "Solution: $solution"
Write-Host "GUI exe : $(Join-Path $BuildDir 'bin\ethercat_gui.exe')"
Write-Host "CLI exe : $(Join-Path $BuildDir 'bin\lms_master.exe')"
Write-Host "SDK dir : $(Join-Path $BuildDir 'ethercat_sdk')"
Write-Host "DLL API : $(Join-Path $BuildDir 'ethercat_sdk\include\ethercat_master.h')"

if ($OpenSolution) {
  Start-Process -FilePath $solution
}
