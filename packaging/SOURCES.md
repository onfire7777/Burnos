# Windows distribution source and licenses

Burnos source: https://github.com/onfire7777/Burnos/tree/v0.22.2

The release at https://github.com/onfire7777/Burnos/releases/tag/v0.22.2
provides `Burnos-0.22.2-corresponding-sources.zip` alongside the installer
and portable application. It contains the upstream source archives and a
SHA-256 manifest. These sources are available without charge or registration.

## Components

- Burnos 0.22.2: original source MIT; this combined Windows distribution is
  GPL-3.0-or-later because the sherpa-onnx 1.x runtime incorporates eSpeak NG.
- Qt 6.11.2 Core, GUI, Widgets, Network and Multimedia: LGPL-3.0 or GPL-3.0,
  with additional third-party notices in the Qt directories.
- whisper.cpp / ggml v1.9.5 (Parakeet): MIT.
- sherpa-onnx v1.13.8: Apache-2.0 source plus its dependencies, including
  eSpeak NG (GPL-3.0-or-later), piper-phonemize (MIT), ONNX Runtime (MIT),
  kaldi-native-fbank / kaldi-decoder / simple-sentencepiece, and others.
- Mnemosyne 1.2.0 (`be29799f`): Apache-2.0, packaged with CPython 3.12.14
  and the dependencies listed in `memory-requirements.txt`.
- MinGW GCC 13.1 runtimes: GPL with GCC Runtime Library Exception.
- PyInstaller bootloader: GPL with the PyInstaller bootloader exception.

All voice model weights and supporting data are included in the installer
and portable ZIP. See `VOICE-MODELS.md` for attribution and upstream terms.
The model manifest pins all files by SHA-256. No API keys or personal memory
records are distributed.

## Rebuild and replacement

Build Burnos using `scripts/build.ps1` with Qt 6.11.2's MinGW x64 kit and
MinGW 13.1. The application is dynamically linked. Qt's source archives
contain `configure.bat` and build instructions; use CMake/Ninja and a
matching x64 MinGW compiler. No Qt patches were applied. The installer
ships only the native Windows multimedia plugin, not FFmpeg or Mesa.

For whisper.cpp v1.9.5 use CMake's `MinGW Makefiles` generator, Release,
`BUILD_SHARED_LIBS=ON`, `GGML_NATIVE=OFF`, `GGML_OPENMP=OFF`, `WHISPER_BUILD_TESTS=OFF`, and
`CMAKE_C_FLAGS=-D_WIN32_WINNT=0x0601`,
`CMAKE_CXX_FLAGS=-D_WIN32_WINNT=0x0601`; build target `parakeet-cli`.
No source patches were applied. Include its DLLs and MinGW runtime DLLs.

The sherpa-onnx binaries are the upstream
`sherpa-onnx-v1.13.8-win-x64-shared-MT-Release.tar.bz2` release
(SHA256 `6dffdc715a4465b989446a6105265d2cb345e7101591a17d35534b6758f6e8df`).
Its source includes the upstream CMake and Windows CI build scripts.
Use Visual Studio x64, Release, shared libraries and static MSVC runtime,
with TTS enabled. Dependency sources and their upstream CMake pins are in
the source bundle; no sherpa-onnx patches were applied.

For the memory executable, install the pinned requirements into Python
3.12 x64, install Mnemosyne from the included source, then run PyInstaller
on `packaging/memory.spec`. `scripts/package-windows.ps1` builds the
portable ZIP and Inno Setup installer from the app and frozen memory
directories. See `docs/WINDOWS_RELEASE.md` for the exact procedure.

Exit Burnos before replacing any of its DLLs, executables, or the
`mnemosyne` directory with compatible rebuilt versions. The program and
installer do not verify vendor signatures or prevent modified versions
from running. Keep the same interfaces and architecture. Personal data
is outside the install directory and is not needed to rebuild.
