#ifndef STTPATHS_H
#define STTPATHS_H

#include <QString>

namespace BurnOS {
namespace Stt {

// C:/ProgramData/BurnOS - the machine wide data root for BurnOS.
QString dataDir();

// C:/ProgramData/BurnOS/STT - where the speech to text model lives.
QString dir();

// Full path to the Parakeet model file inside dir().
QString modelPath();

// Creates dir(), and every missing parent, if it is not already there.
// Returns true only when the directory exists and can be written to
// afterwards. On failure *error, when given, receives a readable reason.
bool ensureDir(QString *error = nullptr);

// True when modelPath() names an existing, non empty file.
bool modelPresent();

// The Silero voice detector's weights, as whisper.cpp publishes them
// (ggml-silero-v6.2.0.bin, MIT), next to the recogniser's model. Its size and
// SHA256 are fixed below, so anything else is not treated as present.
QString vadModelPath();
bool vadPresent();
QByteArray vadSha256(); // hex, lower case
qint64 vadBytes();

} // namespace Stt

namespace Tts {

// C:/ProgramData/BurnOS/TTS - where the Kokoro bundle is unpacked.
QString dir();

// The kokoro-en-v0_19 folder inside dir().
QString modelDir();

// Individual files sherpa-onnx-offline-tts.exe needs.
QString modelPath();
QString voicesPath();
QString tokensPath();
QString espeakDataDir();

// Creates dir() if missing, exactly as Stt::ensureDir does.
bool ensureDir(QString *error = nullptr);

// True only when every file the recogniser needs is on disk.
bool ready();

} // namespace Tts
} // namespace BurnOS

#endif // STTPATHS_H
