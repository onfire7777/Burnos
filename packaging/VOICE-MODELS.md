# Bundled voice models

Burnos includes these public pretrained models under `models/` in both the
Windows installer and portable ZIP. No separate voice-model download is required.
The conversational AI continues to use your configured OpenRouter account.
No personal recordings, voiceprints, memory records, or API keys are included.

| Purpose | Model and attribution | Upstream terms |
| --- | --- | --- |
| Speech recognition | NVIDIA Parakeet TDT 0.6B v3, converted and quantized to Q8_0 by ggml-org | [NVIDIA model, CC BY 4.0](https://huggingface.co/nvidia/parakeet-tdt-0.6b-v3); [ggml-org conversion](https://huggingface.co/ggml-org/parakeet-GGUF), marked MIT |
| Spoken responses | hexgrad Kokoro English v0.19, ONNX distribution from sherpa-onnx | Apache-2.0; license included beside the model |
| Voice activity detection | Silero VAD 6.2.0, ggml-org conversion | [MIT](https://huggingface.co/ggml-org/whisper-vad) |
| Speaker matching | ModelScope 3D-Speaker ERes2Net, English VoxCeleb model, sherpa-onnx ONNX distribution | [Apache-2.0](https://github.com/modelscope/3D-Speaker) |
| Wake words | pkufool/sherpa-onnx Zipformer GigaSpeech 3.3M, 2024-01-01 | Apache-2.0; upstream model card included beside the model |

Parakeet is a converted, quantized adaptation of NVIDIA's original model, not
an unmodified original. Burnos makes no further changes to the distributed
weights. Attribution does not imply endorsement. The original model's
[CC BY 4.0 license](https://creativecommons.org/licenses/by/4.0/) also applies.
Other upstream conversions and model files are redistributed unchanged.
Kokoro's bundled eSpeak NG data retains its GPL terms; corresponding eSpeak
source and license notices accompany the Windows distribution.

`packaging/voice-models.json` pins every shipped model/data file by SHA-256.
The upstream download URLs are in `installer.cpp`. To reproduce the model
directory, run `Burnos.exe --prepare-models` with `BURNOS_MODEL_DIR` set to an
empty build directory, then package with `-ModelsDir` pointing to that directory.
Packaging fails on missing files or checksum mismatches and ignores extra files.

Installed models are read from the application's `models` directory. An explicit
`BURNOS_MODEL_DIR` takes precedence for developer testing. Source builds without
bundled models can still use the legacy cache or the built-in downloader.
Uninstall removes bundled model files with the app and preserves personal
memory, settings, and separately downloaded model caches.
