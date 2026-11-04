param(
  [string]$ArduinoCli,
  [string]$LibraryDirectory,
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

$sketchRoot = Join-Path $buildRoot 'DrowningWearable'
$binaryRoot = Join-Path $buildRoot 'build'
$configFile = Join-Path $buildRoot 'arduino-cli.yaml'
New-Item -ItemType Directory -Force -Path $sketchRoot, $binaryRoot | Out-Null
@{
  directories = @{
    data = [IO.Path]::GetFullPath($CoreDataDirectory)
    downloads = (Join-Path $buildRoot 'downloads')
    user = $sketchbook
  }
  build_cache = @{ path = (Join-Path $buildRoot 'cache') }
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $configFile -Encoding utf8

# Arduino requires a folder matching the primary sketch name.
Get-ChildItem -LiteralPath $PSScriptRoot -File |
  Where-Object { $_.Extension -in @('.ino', '.h', '.hpp', '.cpp', '.c') } |
  ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $sketchRoot -Force }

$arguments = @(
  'compile', '--config-file', $configFile,
  '--fqbn', 'esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB,PSRAM=opi,CDCOnBoot=cdc',
  '--warnings', 'all', '--jobs', '8', '--build-path', $binaryRoot,
  '--libraries', (Join-Path $PSScriptRoot 'libraries')
)
if ($LibraryDirectory) {
  $arguments += @('--libraries', (Resolve-Path -LiteralPath $LibraryDirectory).Path)
}
if ($VerboseCompiler) { $arguments += '--verbose' }
$arguments += $sketchRoot
& $ArduinoCli @arguments
if ($LASTEXITCODE -ne 0) { throw "Arduino compile failed with exit code $LASTEXITCODE" }
