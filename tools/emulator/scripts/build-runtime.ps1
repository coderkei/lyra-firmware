$ErrorActionPreference = 'Stop'

$emulatorRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$runtimeRoot = Join-Path $emulatorRoot 'runtime'
$tempRoot = [IO.Path]::GetFullPath($env:TEMP)
$buildRoot = Join-Path $tempRoot ("lyra-esp32sim-build-" + [guid]::NewGuid().ToString('N'))
$sourceRoot = Join-Path $buildRoot 'esp32sim'
$patchPath = Join-Path $runtimeRoot 'esp32sim-lyra.patch'
$storagePatchPath = Join-Path $runtimeRoot 'esp32sim-storage.patch'
$controlsPatchPath = Join-Path $runtimeRoot 'esp32sim-controls.patch'
$appWasmRoot = Join-Path $emulatorRoot 'app\wasm'
$revision = '4ab7e900fee998d0137c2c57d59de9d05339d093'
$rustFlags = '-Cllvm-args=-inline-threshold=2000 -Clink-arg=--export-table -Clink-arg=--growable-table'

function Invoke-Native([string]$Command, [string[]]$Arguments) {
  & $Command @Arguments
  if ($LASTEXITCODE -ne 0) { throw "$Command failed with exit code $LASTEXITCODE" }
}

if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw 'Git is required to rebuild the emulator runtime.' }
if (-not (Get-Command cargo -ErrorAction SilentlyContinue)) { throw 'Rust/Cargo is required. Install Rust, then run rustup target add wasm32-unknown-unknown.' }
if (-not (Test-Path -LiteralPath $patchPath -PathType Leaf)) { throw "Runtime patch not found: $patchPath" }
if (-not (Test-Path -LiteralPath $storagePatchPath -PathType Leaf)) { throw "Storage patch not found: $storagePatchPath" }
if (-not (Test-Path -LiteralPath $controlsPatchPath -PathType Leaf)) { throw "Runtime controls patch not found: $controlsPatchPath" }

$previousRustFlags = $env:RUSTFLAGS
try {
  [IO.Directory]::CreateDirectory($buildRoot) | Out-Null
  Invoke-Native 'git' @('init', $sourceRoot)
  Invoke-Native 'git' @('-C', $sourceRoot, 'config', 'core.autocrlf', 'false')
  Invoke-Native 'git' @('-C', $sourceRoot, 'remote', 'add', 'origin', 'https://github.com/joakimeriksson/esp32sim.git')
  Invoke-Native 'git' @('-C', $sourceRoot, 'fetch', '--depth', '1', 'origin', $revision)
  Invoke-Native 'git' @('-C', $sourceRoot, 'checkout', '--detach', 'FETCH_HEAD')
  Invoke-Native 'git' @('-C', $sourceRoot, 'apply', '--check', $patchPath)
  Invoke-Native 'git' @('-C', $sourceRoot, 'apply', $patchPath)
  Invoke-Native 'git' @('-C', $sourceRoot, 'apply', '--check', $storagePatchPath)
  Invoke-Native 'git' @('-C', $sourceRoot, 'apply', $storagePatchPath)
  Invoke-Native 'git' @('-C', $sourceRoot, 'apply', '--check', $controlsPatchPath)
  Invoke-Native 'git' @('-C', $sourceRoot, 'apply', $controlsPatchPath)

  Push-Location $sourceRoot
  try {
    $env:RUSTFLAGS = $rustFlags
    Invoke-Native 'cargo' @('build', '--release', '--target', 'wasm32-unknown-unknown', '-p', 'esp32sim-wasm')
  } finally {
    Pop-Location
    $env:RUSTFLAGS = $previousRustFlags
  }

  foreach ($file in @('worker.js', 'jit.mjs', 'pacing.mjs', 'experiments.mjs')) {
    Copy-Item -LiteralPath (Join-Path $sourceRoot "web\wasm\$file") -Destination (Join-Path $appWasmRoot $file) -Force
  }

  $wasmPath = Join-Path $sourceRoot 'target\wasm32-unknown-unknown\release\esp32sim_wasm.wasm'
  $gzipPath = Join-Path $appWasmRoot 'esp32sim.wasm.gz'
  $inputStream = [IO.File]::OpenRead($wasmPath)
  $outputStream = [IO.File]::Create($gzipPath)
  $gzipStream = [IO.Compression.GZipStream]::new($outputStream, [IO.Compression.CompressionLevel]::Optimal)
  try { $inputStream.CopyTo($gzipStream) }
  finally { $gzipStream.Dispose(); $outputStream.Dispose(); $inputStream.Dispose() }
  Write-Host "Runtime rebuilt: $gzipPath"
} finally {
  $resolvedBuild = [IO.Path]::GetFullPath($buildRoot).TrimEnd([IO.Path]::DirectorySeparatorChar)
  $resolvedTemp = [IO.Path]::GetFullPath($tempRoot).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
  if ($resolvedBuild.StartsWith($resolvedTemp, [StringComparison]::OrdinalIgnoreCase) -and
      [IO.Path]::GetFileName($resolvedBuild).StartsWith('lyra-esp32sim-build-', [StringComparison]::OrdinalIgnoreCase) -and
      (Test-Path -LiteralPath $resolvedBuild)) {
    Remove-Item -LiteralPath $resolvedBuild -Recurse -Force
  }
}
