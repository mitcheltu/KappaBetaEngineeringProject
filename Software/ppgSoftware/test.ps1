param([string]$Compiler = 'g++')

# Host-only assertion tests; no firmware upload and no -DNDEBUG.
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$buildRoot = Join-Path $projectRoot 'tmp/ppg-tests'
New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null
$originalPath = $env:PATH
Push-Location $projectRoot
try {
  $compilerCommand = Get-Command $Compiler -ErrorAction SilentlyContinue
  if (-not $compilerCommand) { throw 'C++ compiler unavailable; pass -Compiler with g++.exe.' }
  # MSYS2 executables need their runtime DLL directory when tests execute.
  $env:PATH = (Split-Path -Parent $compilerCommand.Source) + ';' + $env:PATH
  $tests = @('oxygen_concern_test', 'ppg_window_quality_test', 'ppg_pulse_test', 'ppg_hardware_test', 'ppg_analysis_test')
  foreach ($test in $tests) {
    $arguments = @('-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-pedantic',
      '-I', 'Software/ppgSoftware')
    if ($test -in @('ppg_hardware_test', 'ppg_analysis_test')) { $arguments += @('-I', 'Software/tests/ppg_hardware_stubs') }
    $executable = Join-Path $buildRoot ($test + '.exe')
    $arguments += @(('Software/tests/' + $test + '.cpp'), '-o', $executable)
    & $compilerCommand.Source @arguments
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $test" }
    & $executable
    if ($LASTEXITCODE -ne 0) { throw "Assertion test failed: $test" }
  }
} finally {
  $env:PATH = $originalPath
  Pop-Location
}
