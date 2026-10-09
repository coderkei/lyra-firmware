param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $SdRoot
)

$ErrorActionPreference = 'Stop'
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$destination = Join-Path $SdRoot '.lyra\ime'
$assets = @('lyra_ime_zh.bin', 'lyra_ime_ja.bin')

New-Item -ItemType Directory -Path $destination -Force | Out-Null
foreach ($asset in $assets) {
    $source = Join-Path $scriptRoot $asset
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Generated IME dictionary is missing: $source"
    }
    Copy-Item -LiteralPath $source -Destination (Join-Path $destination $asset) -Force
}

Write-Output "Installed offline IME dictionaries to $destination"
