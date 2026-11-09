param(
  [string]$ArduinoCli,
  [string]$ConfigFile,
  [ValidateSet('8M', '16M')][string]$FlashSize = '8M',
  [ValidateRange(1, 8)][int]$Jobs = 2,
  [string]$OutputDirectory
)

# Compile only. No core/library installation, settings changes, or upload.
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
if (-not $ArduinoCli) {
  $cliCommand = Get-Command arduino-cli -ErrorAction SilentlyContinue
  if ($cliCommand) { $ArduinoCli = $cliCommand.Source }
  else { $ArduinoCli = Join-Path $env:LOCALAPPDATA 'Programs/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe' }
}
if (-not (Test-Path -LiteralPath $ArduinoCli)) { throw 'Arduino CLI not found; pass -ArduinoCli or install Arduino IDE.' }
if (-not $ConfigFile) {
  $ideConfig = Join-Path $env:USERPROFILE '.arduinoIDE/arduino-cli.yaml'
  if (Test-Path -LiteralPath $ideConfig) { $ConfigFile = $ideConfig }
}
$configArgs = @()
if ($ConfigFile) { $configArgs = @('--config-file', (Resolve-Path -LiteralPath $ConfigFile).Path) }
$dump = & $ArduinoCli config dump --json @configArgs
if ($LASTEXITCODE -ne 0) { throw 'Unable to read Arduino CLI configuration.' }
$sketchbook = ($dump | ConvertFrom-Json).config.directories.user
$depthLibrary = Join-Path $sketchbook 'libraries/BlueRobotics_MS5837_Library'
if (-not (Test-Path -LiteralPath (Join-Path $depthLibrary 'library.properties'))) {
  throw 'Install BlueRobotics MS5837 Library 1.1.1 using Library Manager first.'
}
if (-not (Select-String -LiteralPath (Join-Path $depthLibrary 'library.properties') -Pattern '^name=BlueRobotics MS5837 Library\s*$' -Quiet)) {
  throw 'The selected depth library is not the original BlueRobotics library.'
}
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $projectRoot 'tmp/demo-revi/bin' }
$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
$partition = if ($FlashSize -eq '16M') { 'fatflash' } else { 'default_8MB' }
$fqbn = "esp32:esp32:esp32s3:FlashSize=$FlashSize,PartitionScheme=$partition,PSRAM=opi,CDCOnBoot=cdc"
# Do not set --build-path or --clean: allow normal Arduino CLI cache reuse.
& $ArduinoCli compile @configArgs --fqbn $fqbn --warnings all --jobs $Jobs `
    --library $depthLibrary --output-dir $outputRoot $PSScriptRoot
if ($LASTEXITCODE -ne 0) { throw "Demo compilation failed with exit code $LASTEXITCODE" }
Write-Output "Demo compiled to $outputRoot (no upload performed)."
