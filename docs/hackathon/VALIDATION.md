# Repository preparation checks

Checked October 14, 2026 against the v0.22.0 application source and the repository-preparation changes.

| Check | Result and scope |
| --- | --- |
| Native build | Fresh build with Qt 6.11.2, MinGW 13.1, and the portable `build.bat`/PowerShell script completed; Qt deployment completed |
| Executable smoke check | The new executable exported its icon and exited successfully without opening the microphone |
| Build error handling | An invalid Qt directory returned a nonzero exit code |
| Speech runtime setup | whisper.cpp v1.9.5 Parakeet CLI built from source; sherpa-onnx 1.13.8 archive checksum verified |
| Speech round trip | Fresh sherpa runtime synthesized a fictional budget sentence; fresh Parakeet CLI recognized “The Aurora workshop has a budget of $600.” using existing downloaded models |
| Memory | `scripts/verify-memory.py` passed against the existing Mnemosyne environment using a temporary store and MCP 2025-11-25 |
| Video source | `npm ci --ignore-scripts`, ESLint, and TypeScript completed; a reveal still and a 30-frame full-composition clip rendered successfully |
| Committed film | H.264/AAC, 1920×1080, 179.008 seconds |
| Documentation | Local Markdown links and JSON files checked |
| Publication scan | Gitleaks 8.30.1 found no leaks in scanned Git history or the staged publication snapshot |

The existing v0.22.0 Windows archive did not include the memory server or third-party license notices. Releases with binary assets were retained as drafts rather than exposed as ready-to-run public downloads. This review does not certify those legacy archives for redistribution.

The memory check emitted a Starlette warning about the future replacement of its httpx test-client dependency; assertions passed. The first upstream speech build exposed missing thread-throttling declarations in MinGW's headers; the documented Windows API target flags resolved the build without patching upstream sources.

These are scoped checks, not a full end-to-end application acceptance run or a comprehensive security audit. The speech check used models already downloaded on this machine; first-run model downloads were not repeated. MSVC, a clean Windows machine, real connected accounts, a live Alexa device, and a full native-app restart/memory workflow were not tested in this preparation. Refer to [the submission checklist](README.md) for remaining delivery items.
