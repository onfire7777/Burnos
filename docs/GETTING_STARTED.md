# Get Burnos running

Burnos targets Windows 10/11 x64. Use a normal, non-administrator Windows account.
The packaged CPU speech runtime requires AVX2, FMA, and F16C support; no dedicated GPU is needed.

## Recommended: install the complete Windows download

**[Download Burnos Setup](https://github.com/onfire7777/Burnos/releases/download/v0.24.0/Burnos-0.24.0-Setup-x64.exe)** or [get the portable ZIP](https://github.com/onfire7777/Burnos/releases/download/v0.24.0/Burnos-v0.24.0-windows-x64.zip).

Run the installer, then launch Burnos from Start. It includes Qt, speech engines, and the Mnemosyne memory server. Python and development tools are not required. The portable version has the same components: extract the **entire** ZIP before running `Burnos.exe`.

All voice models are included in the installer and portable ZIP. Settings opens for your own OpenRouter key and conversational model selection. OpenRouter conversations need Internet access; provider usage may incur charges. Right-click the orb to reopen Settings later. Optional Composio connections require their own setup.

Memory starts automatically and persists in `%LOCALAPPDATA%\BurnOS\memory`. New models use `%LOCALAPPDATA%\BurnOS\models`; a complete existing `%ProgramData%\BurnOS` model cache is reused. The installer uses `%LOCALAPPDATA%\Programs\Burnos` and requires no administrator privileges. Uninstalling keeps your memory, models, and settings.

The release is unsigned; Windows may show an unknown-publisher or SmartScreen warning. Verify the download against `SHA256SUMS.txt` on the [release page](https://github.com/onfire7777/Burnos/releases/tag/v0.24.0). Browser research requires Edge, Chrome, or Brave.

If you previously configured `%APPDATA%\BurnOS\mcp.json`, that configuration takes precedence over the bundled server. Back up and review it if memory fails to connect. Port 8765 must be available or hosting the matching memory server. Do not post your memory files, keys, or unredacted logs in support issues.

## Developer setup from source

The remaining steps are for developers building their own copy; installer users can skip them. See [Windows release packaging](WINDOWS_RELEASE.md) to reproduce the complete bundle.

## 1. Build the native app

Install Git and a Qt 6 Windows desktop kit with Qt Multimedia and its matching compiler. The documented build uses **Qt 6.11.2 + MinGW 13.1**. An MSVC kit can also be built from an x64 Visual Studio developer shell, but that path was not revalidated for this documentation update.

```powershell
git clone https://github.com/onfire7777/Burnos.git
cd Burnos
.\build.bat -QtRoot C:\Qt\6.11.2\mingw_64 -CompilerBin C:\Qt\Tools\mingw1310_64\bin
```

The script checks tools, builds under `build/win32-g++`, and runs `windeployqt`. Adjust paths for your installation. Keep this PowerShell window at the repository root for the following commands.

```powershell
$app = Join-Path (Get-Location) 'build/win32-g++/release'
```

For MSVC, use `-QtRoot` pointing at the MSVC kit and omit `-CompilerBin`; the developer shell must provide `nmake`. Its output is `build/win32-msvc/release`.

## 2. Add speech recognition

Use the **Parakeet implementation in ggml-org/whisper.cpp**, not a different project with a similarly named executable. Burnos expects `parakeet-cli -m … -ng -np -f …` and the whisper.cpp GGML model format.

Install CMake, add the matching MinGW compiler to PATH, and build the pinned upstream release:

```powershell
$env:PATH = 'C:\Qt\Tools\mingw1310_64\bin;C:\Qt\Tools\CMake_64\bin;' + $env:PATH
New-Item -ItemType Directory -Force dependencies | Out-Null
git clone --depth 1 --branch v1.9.5 https://github.com/ggml-org/whisper.cpp.git dependencies/whisper.cpp
cmake -S dependencies/whisper.cpp -B dependencies/whisper-build -G 'MinGW Makefiles' -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF -DWHISPER_BUILD_TESTS=OFF '-DCMAKE_C_FLAGS=-D_WIN32_WINNT=0x0601' '-DCMAKE_CXX_FLAGS=-D_WIN32_WINNT=0x0601'
cmake --build dependencies/whisper-build --target parakeet-cli -j 4
New-Item -ItemType Directory -Force "$app/parakeet-streaming" | Out-Null
Copy-Item dependencies/whisper-build/bin/parakeet-cli.exe "$app/parakeet-streaming/"
Copy-Item dependencies/whisper-build/bin/*.dll "$app/parakeet-streaming/"
Copy-Item dependencies/whisper.cpp/LICENSE "$app/parakeet-streaming/"
Copy-Item 'C:/Qt/Tools/mingw1310_64/bin/libgcc_s_seh-1.dll','C:/Qt/Tools/mingw1310_64/bin/libstdc++-6.dll','C:/Qt/Tools/mingw1310_64/bin/libwinpthread-1.dll' "$app/parakeet-streaming/"
```

Run each command only after the preceding command succeeds. Retain the compiler runtime DLLs deployed in step 1. The upstream tag resolves to `d1be6fde11ac6e0407606b4e42fe72d34add8037` in this setup review. [Upstream CLI documentation](https://github.com/ggml-org/whisper.cpp/tree/v1.9.5/examples/parakeet-cli).

The Windows API target flags avoid an upstream thread-throttling API that is absent from the MinGW 13.1 headers. They disable that optional scheduling optimization, not speech recognition. Burnos itself still targets current Windows desktop systems.

## 3. Add speech synthesis and voice tools

Download the **shared, with-TTS** Windows x64 build of sherpa-onnx 1.13.8. The C API layout used in Burnos is version-specific; do not substitute a `no-tts` archive.

```powershell
$archive = 'dependencies/sherpa-onnx.tar.bz2'
Invoke-WebRequest 'https://github.com/k2-fsa/sherpa-onnx/releases/download/v1.13.8/sherpa-onnx-v1.13.8-win-x64-shared-MT-Release.tar.bz2' -OutFile $archive
if ((Get-FileHash $archive -Algorithm SHA256).Hash -ne '6DFFDC715A4465B989446A6105265D2CB345E7101591A17D35534B6758F6E8DF') { throw 'Archive checksum mismatch' }
tar -xf $archive -C dependencies
$sherpa = 'dependencies/sherpa-onnx-v1.13.8-win-x64-shared-MT-Release'
New-Item -ItemType Directory -Force "$app/kokoro-tts" | Out-Null
Copy-Item "$sherpa/bin/sherpa-onnx-offline-tts.exe" "$app/kokoro-tts/"
Copy-Item "$sherpa/lib/*.dll" "$app/kokoro-tts/"
```

The checksum above was calculated from the upstream archive downloaded on October 14, 2026; it is an integrity pin, not a vendor signature. [Upstream Windows downloads](https://k2-fsa.github.io/sherpa/onnx/install/windows/generated/download/windows_x64.html). Install Microsoft's current x64 Visual C++ Redistributable if a runtime reports a missing Microsoft DLL. Preserve upstream licenses if redistributing these binaries.

## 4. Add persistent memory

Install Python 3.12 or newer. Clone the public [Mnemosyne repository](https://github.com/onfire7777/Mnemosyne) beside the executable and install its MCP and graph extras into an isolated environment:

```powershell
git clone https://github.com/onfire7777/Mnemosyne.git "$app/mnemosyne"
git -C "$app/mnemosyne" checkout edddea3ee18b0bb9d83d4a5405afee6b6a2425a6
py -3.12 -m venv "$app/mnemosyne/.venv"
& "$app/mnemosyne/.venv/Scripts/python.exe" -m pip install "$app/mnemosyne[mcp,graph]"
```

Burnos detects `mnemosyne/.venv/Scripts/mneme-mcp.exe`, starts it on loopback, and supplies its local session secret. The default store is `%LOCALAPPDATA%\BurnOS\memory`; keep it private. Port 8765 must be free or already hosting the correctly configured server. A pre-existing MCP configuration can override automatic discovery.

To use your own MCP server configuration, copy [the example](../examples/mcp.example.json) to `%APPDATA%\BurnOS\mcp.json` and replace the placeholder URL. That example demonstrates the schema; it is not a substitute for a running memory server. `BURNOS_MCP_CONFIG` can point at a separate configuration for testing.

## 5. Launch and configure

```powershell
& "$app/Burnos.exe"
```

Source builds without bundled models download speech models into `%LOCALAPPDATA%\BurnOS\models`, or reuse a complete legacy `%ProgramData%\BurnOS` cache. Official Windows downloads include the models beside the app. Allow several gigabytes of disk headroom. `BURNOS_MODEL_DIR` can select an isolated cache for developer testing.

Open Settings from the orb, enter your OpenRouter key, choose the talking and task models, and check microphone/output settings. Use a tool-capable model; desktop screenshots also require image input. Start with a harmless request and an empty test document. Composio is optional and requires its own configuration and authorized account connections.

Check the app's live log for a ready memory server before relying on persistence. Do not record or share the log without removing private data. The [demo guide](hackathon/DEMO.md) contains a fictional end-to-end scenario.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| Qt or compiler command missing | Qt kit and compiler architecture must match; pass both paths explicitly for MinGW |
| Missing platform plugin or Qt DLL | Run the build script's deployment step; keep the generated DLL/plugin folders beside the EXE |
| `parakeet-cli.exe` missing | Step 2 must produce `parakeet-streaming/parakeet-cli.exe` beside Burnos |
| Speech executable exists but fails | Check dependent DLLs, model download completion, and the exact runtime project/version |
| Kokoro fails to load | Use sherpa-onnx 1.13.8 with TTS, including DLLs from `lib` |
| Memory disappears after restart | Confirm Mnemosyne is ready and both runs use the same store; check port 8765 and custom MCP config |
| Research cannot read a site | The separate browser is not signed in; authentication, CAPTCHA, dynamic pages, and scanned PDFs may prevent extraction |
| Image request is rejected | Select a model that supports image input and tools; do not assume every model can inspect screenshots |

For a backend-only persistence check, install `httpx` into the Mnemosyne environment and run `scripts/verify-memory.py` with that environment's Python. It uses a temporary store and does not read your personal memory. This does not replace the live-app test.
