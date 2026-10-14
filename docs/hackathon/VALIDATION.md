# Repository and Windows release checks

## Windows release 0.22.2 — October 14, 2026

This release bundles all voice models and preserves OpenRouter for conversational AI.

| Check | Result and scope |
| --- | --- |
| Native build | Qt 6.11.2 / MinGW 13.1 release build passed |
| Bundled models | All 378 model/data files matched SHA-256 pins; total 1,085,569,733 bytes |
| Fresh model setup | With empty isolated user and legacy caches and no model-path override, the installed app selected its bundled models and reported ready in 0.19 seconds; no download started or external model cache was created |
| Portable ZIP | All 1,036 packaged file hashes verified directly from the ZIP |
| Installer and reinstall | Both completed successfully in a path containing spaces; all 1,036 installed file hashes matched after each run |
| Installed memory | Capture, search, native context, full process restart, recall, and owned-server cleanup passed with isolated fictional data and no API key |
| Installed speech | Bundled Kokoro synthesized the fictional Aurora budget sentence; installed Parakeet recognized it using the bundled models |
| Uninstall | Removed the tested app and bundled models; retained the external fictional-data marker |

Tests ran on the development PC, not a clean Windows VM. Real-microphone accuracy,
paid model calls, connected-account actions, and a live Alexa device were not tested.
The previously blocked browser smoke test remains unverified. The installer is unsigned.
Internet access and an OpenRouter key remain necessary for conversational AI and online
features, but no separate voice-model download is required.

## Windows release 0.22.1 — October 14, 2026

| Check | Result and scope |
| --- | --- |
| Native application | Qt 6.11.2 / MinGW 13.1 release build passed |
| Complete installer | Inno Setup 6.7.3 produced the per-user x64 installer; clean installation and reinstallation both returned success |
| Installed files | All 654 manifest entries matched their SHA-256 hashes after each installation, including a path containing spaces |
| Bundled memory | Frozen Mnemosyne 1.2.0 passed its self-test; no separately installed Python is needed |
| Native memory integration | Installed Burnos started its packaged memory server with an isolated profile and Windows-only PATH; a fictional fact was captured, found by search, and included in native conversation context before and after restarting both processes |
| Process cleanup | The memory server owned by the tested app exited with Burnos; existing independently hosted servers were not stopped |
| First-run models | The actual downloader completed from an empty isolated model directory; 378 resulting files totaled 1,085,569,733 bytes |
| Local speech | Packaged Kokoro synthesized the fictional Aurora budget sentence; packaged Parakeet recognized it correctly using the newly downloaded models |
| In-process models | Native startup logs confirmed Parakeet, Kokoro, speaker matching, and keyword models loaded and remained available; the release includes the shared Parakeet libraries rather than relying only on CLI fallback |
| Speaker model | Native voice probe extracted a voiceprint and returned similarity 1.000 for the identical synthetic clip |
| First-run UI | Settings opened automatically with an empty API-key field; the installer wizard and its license page were inspected |
| Uninstall | Removed the tested application while retaining the external fictional user-data marker |
| Publication scan | Gitleaks found no secrets in the staged release changes; package construction uses an explicit file allowlist and excludes personal stores, keys, and logs |

These checks ran in isolated profiles on the development PC, not in a clean Windows VM. No paid model calls, real connected-account actions, or real-microphone accuracy evaluation were performed. The separate browser smoke-test command was blocked by the host's automatic approval review and is not claimed as verified. The installer is unsigned. First launch still requires Internet access for model downloads and the user's own OpenRouter configuration.

The package includes third-party notices and offers corresponding source alongside the binaries. See [Windows build and test instructions](../WINDOWS_RELEASE.md) and [distribution terms](../../packaging/LICENSE-WINDOWS.txt).

## Earlier repository preparation

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

The existing v0.22.0 Windows archive did not include the memory server or third-party license notices. Legacy releases with binary assets were retained as drafts. The complete v0.22.1 package supersedes them; this review does not certify those legacy archives for redistribution.

The memory check emitted a Starlette warning about the future replacement of its httpx test-client dependency; assertions passed. The first upstream speech build exposed missing thread-throttling declarations in MinGW's headers; the documented Windows API target flags resolved the build without patching upstream sources.

Those earlier checks were scoped repository preparation, not a full application acceptance run or comprehensive security audit. First-run downloads and native restart/memory testing were subsequently completed for v0.22.1 above. MSVC, a clean Windows machine, real connected accounts, and a live Alexa device remain untested. Refer to [the submission checklist](README.md) for remaining delivery items.
