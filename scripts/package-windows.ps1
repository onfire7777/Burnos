[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$AppDir,
    [Parameter(Mandatory)][string]$MemoryDir,
    [Parameter(Mandatory)][string]$LicensesDir,
    [Parameter(Mandatory)][string]$OutputDir,
    [string]$Iscc = "$env:LOCALAPPDATA/Programs/Inno Setup 6/ISCC.exe"
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$version = [regex]::Match((Get-Content "$repo/BurnOS.pro" -Raw), '(?m)^VERSION\s*=\s*(\S+)').Groups[1].Value
if (-not $version) { throw 'No version in BurnOS.pro' }
$AppDir = (Resolve-Path -LiteralPath $AppDir).Path
$MemoryDir = (Resolve-Path -LiteralPath $MemoryDir).Path
$LicensesDir = (Resolve-Path -LiteralPath $LicensesDir).Path
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$OutputDir = (Resolve-Path -LiteralPath $OutputDir).Path
$stage = Join-Path $OutputDir "Burnos-v$version-windows-x64"
if (Test-Path -LiteralPath $stage) { throw "Use a fresh output directory: $stage already exists." }
if ((Get-Item "$AppDir/Burnos.exe").VersionInfo.ProductVersion -ne "$version.0") { throw 'Executable version does not match source.' }
foreach ($required in @('parakeet-streaming/libparakeet.dll', 'parakeet-streaming/ggml.dll',
    'parakeet-streaming/ggml-base.dll', 'parakeet-streaming/ggml-cpu.dll',
    'kokoro-tts/sherpa-onnx-c-api.dll', 'kokoro-tts/onnxruntime.dll')) {
    if (-not (Test-Path -LiteralPath "$AppDir/$required")) { throw "Required speech runtime missing: $required" }
}
New-Item -ItemType Directory -Path $stage | Out-Null
# An allowlist prevents build objects, personal logs, stores, or settings from shipping.
$files = @('Burnos.exe', 'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll',
    'Qt6Network.dll', 'Qt6Multimedia.dll', 'libgcc_s_seh-1.dll',
    'libstdc++-6.dll', 'libwinpthread-1.dll', 'platforms/qwindows.dll',
    'styles/qmodernwindowsstyle.dll', 'tls/qschannelbackend.dll',
    'tls/qcertonlybackend.dll', 'multimedia/windowsmediaplugin.dll',
    'networkinformation/qnetworklistmanager.dll', 'imageformats/qjpeg.dll',
    'imageformats/qico.dll', 'imageformats/qgif.dll')
foreach ($file in $files) {
    $to = Join-Path $stage $file
    New-Item -ItemType Directory -Force -Path (Split-Path $to) | Out-Null
    Copy-Item -LiteralPath (Join-Path $AppDir $file) -Destination $to
}
foreach ($dir in @('parakeet-streaming', 'kokoro-tts')) {
    New-Item -ItemType Directory -Path "$stage/$dir" | Out-Null
    Get-ChildItem -LiteralPath "$AppDir/$dir" -File |
        Where-Object { $_.Extension -in @('.exe', '.dll') -or $_.Name -match '^LICENSE' } |
        Copy-Item -Destination "$stage/$dir"
}
if (-not (Test-Path "$MemoryDir/mneme-mcp.exe")) { throw 'Frozen memory runtime missing.' }
Copy-Item -LiteralPath $MemoryDir -Destination "$stage/mnemosyne" -Recurse
New-Item -ItemType Directory -Path "$stage/licenses" | Out-Null
# Keep extracted license paths short enough for ordinary Windows ZIP tools.
$licenseIndex = Get-ChildItem -LiteralPath $LicensesDir -File -Recurse | ForEach-Object {
    $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    $leaf = ($_.Name -replace '[^a-zA-Z0-9._-]', '_')
    if ($leaf.Length -gt 60) { $leaf = $leaf.Substring(0, 60) }
    $name = $hash.Substring(0, 16) + '-' + $leaf + '.txt'
    Copy-Item -LiteralPath $_.FullName -Destination "$stage/licenses/$name"
    [ordered]@{ source = $_.FullName.Substring($LicensesDir.Length + 1).Replace('\','/'); file = $name; sha256 = $hash }
}
$licenseIndex | ConvertTo-Json -Depth 3 | Set-Content "$stage/licenses/index.json" -Encoding utf8
Copy-Item "$repo/LICENSE" "$stage/LICENSE-Burnos-MIT.txt"
Copy-Item "$repo/packaging/WELCOME.txt" "$stage/WELCOME.txt"
Copy-Item "$repo/packaging/LICENSE-WINDOWS.txt" "$stage/LICENSE-WINDOWS.txt"
Copy-Item "$repo/packaging/SOURCES.md" "$stage/licenses/SOURCES.md"
Copy-Item "$repo/third_party/pdfjs/LICENSE" "$stage/licenses/PDFjs-Apache-2.0.txt"
Copy-Item "$repo/packaging/memory-requirements.txt" "$stage/licenses/memory-requirements.txt"
# Wheel installation metadata can contain the builder's absolute local path.
Get-ChildItem "$stage/mnemosyne" -Filter direct_url.json -Recurse -File | Remove-Item
$forbidden = @(Get-ChildItem $stage -Recurse -File | Where-Object {
    $_.Name -match '^\.env|^mcp\.json$|^memory\.secret$|\.sqlite[\w.-]*$|\.log$|\.pyc$'
})
if ($forbidden.Count) { throw 'Unexpected state or build files in package.' }
$manifest = Get-ChildItem $stage -Recurse -File | ForEach-Object {
    [ordered]@{ path = $_.FullName.Substring($stage.Length + 1).Replace('\','/');
        size = $_.Length; sha256 = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
}
$manifest | ConvertTo-Json -Depth 3 | Set-Content "$stage/package-manifest.json" -Encoding utf8
Compress-Archive -LiteralPath $stage -DestinationPath "$OutputDir/Burnos-v$version-windows-x64.zip" -CompressionLevel Optimal
& $Iscc "/DAppVersion=$version" "/DStageDir=$stage" "/DOutputDir=$OutputDir" "$repo/packaging/Burnos.iss"
if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed.' }
Get-ChildItem $OutputDir -File | Where-Object { $_.Extension -in @('.exe', '.zip') } | ForEach-Object {
    "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $_.Name
} | Set-Content "$OutputDir/SHA256SUMS.txt" -Encoding ascii
Write-Output "Packaged $version in $OutputDir"
