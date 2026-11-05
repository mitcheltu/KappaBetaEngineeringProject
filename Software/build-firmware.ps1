param(
  [string]$ArduinoCli,
  [string]$LibraryDirectory,
  [string]$BlueRoboticsLibraryDirectory,
  [string]$CoreDataDirectory,
  [string]$BuildDirectory,
  [switch]$VerboseCompiler
)

# Compile only: this script never installs a core/library or uploads firmware.
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $projectRoot 'tmp/revf-build' }
$buildRoot = [IO.Path]::GetFullPath($BuildDirectory)

if (-not $ArduinoCli) {
  $cliCommand = Get-Command arduino-cli -ErrorAction SilentlyContinue
  if ($cliCommand) {
    $ArduinoCli = $cliCommand.Source
  } elseif ($env:LOCALAPPDATA) {
    $ArduinoCli = Join-Path $env:LOCALAPPDATA 'Programs/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe'
  }
}
if (-not $ArduinoCli -or -not (Test-Path -LiteralPath $ArduinoCli)) {
  throw 'Arduino CLI was not found. Install Arduino IDE/CLI or pass -ArduinoCli with its executable path.'
}

# Read the IDE's configured sketchbook, including redirected Documents folders.
# No global configuration is changed. CLI defaults apply if no IDE config exists.
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
    throw 'Pass -CoreDataDirectory with your installed Arduino core directory.'
  }
}
$sketchbook = $existingConfig.directories.user
if (-not $sketchbook) { $sketchbook = Join-Path $buildRoot 'sketchbook' }

# Compile the same sketch folder that opens directly in Arduino IDE.
$sketchRoot = Join-Path $PSScriptRoot 'DrowningWearable'
if (-not (Test-Path -LiteralPath (Join-Path $sketchRoot 'DrowningWearable.ino'))) {
  throw 'The DrowningWearable sketch folder is missing its primary .ino file.'
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
if ($BlueRoboticsLibraryDirectory) {
  # Other libraries also export MS5837.h; explicitly prefer the official one.
  $depthLibrary = (Resolve-Path -LiteralPath $BlueRoboticsLibraryDirectory).Path
  $depthProperties = Join-Path $depthLibrary 'library.properties'
  if (-not (Test-Path -LiteralPath $depthProperties) -or
      -not (Select-String -LiteralPath $depthProperties -Pattern '^name=BlueRobotics MS5837 Library\s*$' -Quiet)) {
    throw '-BlueRoboticsLibraryDirectory must contain the official BlueRobotics MS5837 Library.'
  }
  $arguments += @('--library', $depthLibrary)
}
if ($VerboseCompiler) { $arguments += '--verbose' }
$arguments += $sketchRoot
& $ArduinoCli @arguments
if ($LASTEXITCODE -ne 0) { throw "Arduino compile failed with exit code $LASTEXITCODE" }
