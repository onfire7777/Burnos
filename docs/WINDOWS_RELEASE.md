# Reproducing the Windows release

The 0.22.1 package includes Burnos, Qt, CPU speech runtimes, and Mnemosyne
1.2.0 with its Python runtime. It does not include user data or API keys.
Speech models download at first launch. The binary distribution terms
and corresponding-source instructions are in [packaging](../packaging/SOURCES.md).

## Build inputs

- Windows x64; Qt 6.11.2 MinGW kit; MinGW 13.1; CMake.
- whisper.cpp v1.9.5 and sherpa-onnx v1.13.8: follow the
  [developer setup](GETTING_STARTED.md#developer-setup-from-source).
- CPython 3.12.14 x64 and the pinned
  [memory build requirements](../packaging/memory-requirements.txt).
- Mnemosyne source at `be29799f462fab1b558b604ffaa4298b390cd614`.
- Inno Setup 6.7.3 and PowerShell 7 for packaging.

## Build the app and memory runtime

Build the native app with `build.bat`; add speech engines as described in
the developer setup. In a clean Python 3.12 virtual environment:

```powershell
python -m pip install -r packaging/memory-requirements.txt
python -m pip install ../Mnemosyne
python -m PyInstaller --clean --noconfirm --distpath build/frozen-memory --workpath build/freeze-work packaging/memory.spec
```

The result is `build/frozen-memory/mneme-mcp/`, including its executable
and `_internal` directory. Both must ship. Do not copy a developer venv:
its launchers refer to paths that do not exist on a recipient's computer.

## Collect notices and source

From the same build environment, with a checked-out sherpa-onnx v1.13.8:

```powershell
python packaging/collect-sources.py build/redistribution --sherpa-source ../sherpa-onnx
```

This downloads public source archives, checks upstream SHA-256 pins where
available, records all hashes, and gathers license/copyright notices. It
does not inspect application memory or credentials. Include the source
archives, source manifest, this repository's matching source, and these
build instructions in the release's corresponding-source ZIP.

## Package

```powershell
.\scripts\package-windows.ps1 -AppDir build/win32-g++/release -MemoryDir build/frozen-memory/mneme-mcp -LicensesDir build/redistribution/licenses -OutputDir build/release-0.22.1
```

The output directory must be fresh. The script checks the executable
version, copies an explicit list of Qt files, includes the two speech
runtime directories and frozen memory, rejects state/log files, writes
a package manifest, and creates the portable ZIP, installer, and checksums.

## Verify before publishing

```powershell
python scripts/verify-windows-package.py build/release-0.22.1/Burnos-v0.22.1-windows-x64 --report build/memory-test.json
```

This starts the packaged memory through Burnos with an isolated profile
and port, captures a fictional fact, confirms its presence in native
conversation context, restarts both processes, repeats retrieval, and
checks that the owned child exits. It uses a Windows-only PATH and no
model key or microphone.

Install the setup EXE into a fresh test directory, rerun that check on
the installed copy, verify Start-menu entries and Settings, and test
uninstall/reinstall while retaining the isolated data directory. Use
`BURNOS_MODEL_DIR` with a new empty directory and `Burnos.exe --prepare-models`
to exercise real first-run downloads without microphone capture. After
download, test synthesis, transcription, speaker matching, and wake-word
loading with the packaged runtimes. Do not infer memory readiness from
a probe with `@nomemory`.

No code-signing certificate is configured for this release. Do not claim
the installer is signed or bypass Windows security settings during tests.
