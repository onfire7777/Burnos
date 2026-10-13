#ifndef KEYWORDSPOTTER_H
#define KEYWORDSPOTTER_H

#include <QByteArray>
#include <QString>

#include <functional>

class QObject;

// THE ONE THING THAT LISTENS WHILE BURNOS IS MUTED, OR WAITING FOR ITS NAME. A
// keyword spotter is a very small speech model (3.3 million weights,
// sherpa-onnx's English zipformer) that knows only a short list of phrases and
// says when one of them was said. It writes nothing down: what it hears that
// is not on the list leaves no trace - no transcription, no memory, nothing
// sent anywhere. Muted, the microphone's sound goes to this and to nothing
// else, so "unmute" still works; in wake word mode the same is true until the
// wake word is said.
//
// It runs on this machine, through the sherpa-onnx library the voice already
// uses. The model is loaded when Burnos opens and kept until it closes.
namespace BurnOS::Wake {

// What a keyword asks for.
enum class Command {
    Wake,               // the wake word: unmute, and what follows is a request
    Unmute,             // "unmute"
    Mute,               // "mute", "stop listening"
    Settings,           // "open settings"
    StartTranscription, // "start transcription"
    StopTranscription,  // "stop transcription"
    ReadBack,           // "read that back"
};

// The library and the keyword model are both on disk.
bool available();

// Loads the model now, off the main thread, and runs it once. Called when
// Burnos opens; the model then stays loaded until Burnos closes.
void preload();

// LISTEN from now on: every keyword in what is fed is handed to `heard` on the
// main thread - unless `guard` is gone by then - until stop(). The commands
// are always listened for; the wake word only with `wakeWord`, so that a muted
// Burnos is not woken by its name. The wake word is read from Settings here,
// so a changed one takes effect at the next start: a new list on the loaded
// model, nothing is loaded again.
void start(QObject *guard, bool wakeWord,
           std::function<void(Command, const QString &keyword)> heard);
// 16 kHz mono 16 bit audio, as the microphone hands it over. Main thread.
void feed(const QByteArray &pcm);
void stop();

// Whether `phrase` can be a wake word: the model has to be able to spell it,
// which takes plain letters, and it has to be long enough to tell from noise.
bool canSpell(const QString &phrase);

// A transcription with the wake word taken off the front - "Hey Burnos, what
// time is it?" is the question "what time is it?". With `anywhere`, for the
// phrase that woke Burnos, everything up to and including the wake word goes,
// since that phrase may start with the end of something said before it.
// `found` says whether the wake word was there; without it the text comes back
// as it was.
QString withoutWakeWord(const QString &text, bool anywhere, bool *found);

} // namespace BurnOS::Wake

#endif // KEYWORDSPOTTER_H
