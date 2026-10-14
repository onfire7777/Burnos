[CmdletBinding()]
param(
    [string]$QtRoot = $env:QTDIR,
    [string]$CompilerBin,
    [ValidateRange(1, 64)][int]$Jobs = 4
)
$ErrorActionPreference = 'Stop'
try {
    if ($CompilerBin) {
        if (-not (Test-Path -LiteralPath $CompilerBin -PathType Container)) { throw "Compiler directory does not exist: $CompilerBin" }
        $env:PATH = "$CompilerBin;$env:PATH"
    }
    if ($QtRoot) {
        $qmake = Join-Path $QtRoot 'bin/qmake.exe'
        if (-not (Test-Path -LiteralPath $qmake -PathType Leaf)) { throw "qmake.exe not found under $QtRoot" }
        $env:PATH = "$(Join-Path $QtRoot 'bin');$env:PATH"
    } else {
        $qmake = (Get-Command qmake.exe -ErrorAction Stop).Source
        $QtRoot = Split-Path (Split-Path $qmake)
    }
    $spec = (& $qmake -query QMAKE_XSPEC).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Unable to query Qt.' }
    switch ($spec) {
        'win32-g++' { $make = (Get-Command mingw32-make.exe -ErrorAction Stop).Source; $makeArgs = @("-j$Jobs", 'release') }
        'win32-msvc' { $make = (Get-Command nmake.exe -ErrorAction Stop).Source; $makeArgs = @('/NOLOGO', 'release') }
        default { throw "Unsupported Qt kit: $spec. Use Windows x64 MinGW or an x64 MSVC developer shell." }
    }
    $repo = Split-Path $PSScriptRoot
    $output = Join-Path $repo "build/$spec"
    New-Item -ItemType Directory -Force -Path $output | Out-Null
    Push-Location $output
    try {
        & $qmake (Join-Path $repo 'BurnOS.pro') -spec $spec 'CONFIG+=release'
        if ($LASTEXITCODE -ne 0) { throw 'qmake failed.' }
        & $make @makeArgs
        if ($LASTEXITCODE -ne 0) { throw 'Compilation failed.' }
        $exe = Join-Path $output 'release/Burnos.exe'
        & (Join-Path $QtRoot 'bin/windeployqt.exe') --release --compiler-runtime $exe
        if ($LASTEXITCODE -ne 0) { throw 'Qt deployment failed.' }
        Write-Output "Built and deployed Qt: $exe"
        Write-Output 'Next: follow docs/GETTING_STARTED.md to install the speech and memory runtimes.'
    } finally { Pop-Location }
} catch {
    Write-Error $_
    exit 1
}
