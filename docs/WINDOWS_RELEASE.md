# Reproducing the Windows release

The 0.24.0 package includes Burnos, Qt, CPU speech runtimes, and Mnemosyne
1.4.1 with its Python runtime. It does not include user data or API keys.
All voice models are bundled; OpenRouter still provides conversational AI. The binary distribution terms
and corresponding-source instructions are in [packaging](../packaging/SOURCES.md).

## Build inputs

- Windows x64; Qt 6.11.2 MinGW kit; MinGW 13.1; CMake.
- whisper.cpp v1.9.5 and sherpa-onnx v1.13.8: follow the
  [developer setup](GETTING_STARTED.md#developer-setup-from-source).
- CPython 3.12.14 x64 and the pinned
  [memory build requirements](../packaging/memory-requirements.txt).
- Mnemosyne source at `edddea3ee18b0bb9d83d4a5405afee6b6a2425a6`.
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
.\scripts\package-windows.ps1 -AppDir build/win32-g++/release -MemoryDir build/frozen-memory/mneme-mcp -LicensesDir build/redistribution/licenses -ModelsDir build/voice-models -OutputDir build/release-0.24.0
```

Prepare `build/voice-models` using `BURNOS_MODEL_DIR` and the app's
`--prepare-models` command. See [model provenance](../packaging/VOICE-MODELS.md).
The output directory must be fresh. The script checks the executable
version, copies an explicit list of Qt files, includes the two speech
runtime directories and frozen memory, verifies and bundles every pinned voice
model file, rejects state/log files, writes
a package manifest, and creates the portable ZIP, installer, and checksums.

## Verify before publishing

```powershell
python scripts/verify-windows-package.py build/release-0.24.0/Burnos-v0.24.0-windows-x64 --report build/memory-test.json
python scripts/verify-bundled-models.py build/release-0.24.0/Burnos-v0.24.0-windows-x64 --report build/model-test.json
```

This starts the packaged memory through Burnos with an isolated profile
and port, captures a fictional fact, confirms its presence in native
conversation context, restarts both processes, repeats retrieval, and
checks that the owned child exits. It uses a Windows-only PATH and no
model key or microphone.

The bundled-model check verifies model-file hashes and runs actual model setup
with empty isolated user/legacy caches and no model-directory override. It must
select the shipped models, report ready, and never start a download.

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
