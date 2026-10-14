# Third-party components and media

The root MIT license covers Burnos-authored material. It does not relicense third-party software, model weights, fonts, or hosted services.

| Component | Relationship to this repository | Source and terms |
| --- | --- | --- |
| PDF.js 3.11.174 | Two unmodified JavaScript files embedded via Qt resources | [Source](https://github.com/mozilla/pdf.js); [Apache-2.0 license included](third_party/pdfjs/LICENSE) |
| Qt 6 | External build/runtime dependency | [Qt licensing](https://www.qt.io/development/open-source-lgpl-obligations); use a suitable commercial or open-source license for your distribution |
| whisper.cpp / ggml Parakeet | External speech-recognition engine; built separately | [Source](https://github.com/ggml-org/whisper.cpp); preserve upstream MIT notices and dependency licenses |
| sherpa-onnx | External speech runtime | [Source](https://github.com/k2-fsa/sherpa-onnx); Apache-2.0 and its dependency notices |
| ONNX Runtime | Dependency of the speech runtime | [Source and license](https://github.com/microsoft/onnxruntime) |
| Mnemosyne | Separate memory server invoked over MCP | [Source](https://github.com/onfire7777/Mnemosyne); Apache-2.0 |
| Speech models | Downloaded on first run; not committed here | Exact URLs are in [installer.cpp](installer.cpp); model licenses remain separate from engine licenses |
| OpenRouter / Composio | Hosted integrations | Subject to provider terms and the user's authorized accounts |
| Remotion, React, development dependencies | Installed for the optional film project | Exact versions in [package-lock.json](media/remotion/package-lock.json); each package retains its license. [Remotion licensing](https://www.remotion.dev/license) is separate from this repository's MIT license |

## Speech models

Burnos downloads Parakeet weights from [ggml-org/parakeet-GGUF](https://huggingface.co/ggml-org/parakeet-GGUF), Silero VAD weights from [ggml-org/whisper-vad](https://huggingface.co/ggml-org/whisper-vad), and Kokoro, speaker-recognition, and keyword models from [sherpa-onnx releases](https://github.com/k2-fsa/sherpa-onnx/releases). Preserve their model cards, licenses, and any bundled notices when redistributing them. The Kokoro bundle includes phonemizer data with separate notices; do not assume the model's license covers those files.

## Distribution

This repository distributes source and original presentation media, not a complete signed Windows installer. Model weights and external runtime binaries are excluded. If you distribute a compiled app, include the appropriate Qt, speech-runtime, model, phonemizer, and dependency notices and comply with source/relinking obligations that apply to your chosen licenses. In particular, publishing Burnos under MIT does not remove Qt's LGPL obligations.

## Film and visual assets

The Burnos orb, React/SVG/CSS film artwork, and synthesized instrumental were created for this project. Narration was generated using Kokoro. Fonts are resolved from the rendering system and are not distributed. Screens in the film are illustrative. The Burnos name identifies this project; the code license does not imply endorsement of modified versions by its maintainers.
