param(
  [string]$ArduinoCli,
  [string]$LibraryDirectory,
  [string]$SketchbookDirectory,
  [string]$CoreDataDirectory,
  [string]$BuildDirectory,
  [switch]$VerboseCompiler
)

# Compile only. This script never installs dependencies or uploads to a board.
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $projectRoot 'tmp/ppg-build' }
$buildRoot = [IO.Path]::GetFullPath($BuildDirectory)

if (-not $ArduinoCli) {
  $cliCommand = Get-Command arduino-cli -ErrorAction SilentlyContinue
  if ($cliCommand) {
    $ArduinoCli = $cliCommand.Source
  } else {
    # Sandboxed shells may redirect LOCALAPPDATA while the IDE remains in the
    # user's actual AppData directory.
    $localRoots = @($env:LOCALAPPDATA)
    if ($env:USERPROFILE) { $localRoots += Join-Path $env:USERPROFILE 'AppData/Local' }
    foreach ($localRoot in $localRoots) {
      if (-not $localRoot) { continue }
      $candidate = Join-Path $localRoot 'Programs/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe'
      if (Test-Path -LiteralPath $candidate) { $ArduinoCli = $candidate; break }
    }
  }
}
if (-not $ArduinoCli -or -not (Test-Path -LiteralPath $ArduinoCli)) {
  throw 'Arduino CLI was not found. Pass -ArduinoCli with the installed executable path.'
}

# Read existing configuration without changing global IDE/CLI settings.
$configArguments = @('config', 'dump', '--json')
if ($env:USERPROFILE) {
  $ideConfig = Join-Path $env:USERPROFILE '.arduinoIDE/arduino-cli.yaml'
  if (Test-Path -LiteralPath $ideConfig) { $configArguments += @('--config-file', $ideConfig) }
}
$configText = & $ArduinoCli @configArguments
if ($LASTEXITCODE -ne 0) { throw 'Could not read Arduino CLI configuration.' }
$existingConfig = ($configText | ConvertFrom-Json).config
if (-not $CoreDataDirectory) {
  if ($existingConfig.directories.data) {
    $CoreDataDirectory = $existingConfig.directories.data
  } elseif ($env:LOCALAPPDATA) {
    $CoreDataDirectory = Join-Path $env:LOCALAPPDATA 'Arduino15'
  } else {
    throw 'Pass -CoreDataDirectory with the installed Arduino core directory.'
  }
}
$sketchbook = $SketchbookDirectory
if (-not $sketchbook) { $sketchbook = $existingConfig.directories.user }
if (-not $sketchbook) { $sketchbook = Join-Path $buildRoot 'sketchbook' }
if (-not $LibraryDirectory) {
  $cachedLibraries = Join-Path $projectRoot 'tmp/revf-build/libraries'
  if (Test-Path -LiteralPath $cachedLibraries) { $LibraryDirectory = $cachedLibraries }
}

$binaryRoot = Join-Path $buildRoot 'build'
$configFile = Join-Path $buildRoot 'arduino-cli.yaml'
New-Item -ItemType Directory -Force -Path $binaryRoot | Out-Null
@{
  directories = @{
    data = [IO.Path]::GetFullPath($CoreDataDirectory)
    downloads = (Join-Path $buildRoot 'downloads')
    user = $sketchbook
  }
  build_cache = @{ path = (Join-Path $buildRoot 'cache') }
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $configFile -Encoding utf8

$arguments = @(
  'compile', '--config-file', $configFile,
  '--fqbn', 'esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB,PSRAM=opi,CDCOnBoot=cdc',
  '--warnings', 'all', '--jobs', '8', '--build-path', $binaryRoot
)
if ($LibraryDirectory) {
  $arguments += @('--libraries', (Resolve-Path -LiteralPath $LibraryDirectory).Path)
}
if ($VerboseCompiler) { $arguments += '--verbose' }
$arguments += $PSScriptRoot
& $ArduinoCli @arguments
if ($LASTEXITCODE -ne 0) { throw "Arduino compile failed with exit code $LASTEXITCODE" }
