#ifndef KEYWORDSPOTTER_H
#define KEYWORDSPOTTER_H

#include <QByteArray>
#include <QString>

#include <functional>

class QObject;

// THE WAKE WORD, HEARD AS IT IS SAID. A keyword spotter is a very small speech
// model (3.3 million weights, sherpa-onnx's English zipformer) that knows one
// phrase - the wake word - and says so within half a second of hearing it,
// while the sentence it began is still being spoken. In wake word mode that is
// what turns the waiting orb blue before the request is even finished.
//
// It is not what Burnos relies on. The spoken commands - mute, unmute and the
// rest - are not its business at all: on the owner's own voice it missed them
// again and again, where the speech recogniser wrote every one of them down
// correctly, so commands go through the recogniser in every state. And a wake
// word this model misses is still found in what the recogniser wrote.
//
// It runs on this machine, through the sherpa-onnx library the voice already
// uses. The model is loaded when Burnos opens and kept until it closes; only a
// change of wake word makes it anew.
namespace BurnOS::Wake {

// The library and the keyword model are both on disk.
bool available();

// Loads the model now, off the main thread, and runs it once. Called when
// Burnos opens; the model then stays loaded until Burnos closes.
void preload();

// LISTEN FOR THE WAKE WORD from now on: when it is in what is fed, `heard` is
// told on the main thread - unless `guard` is gone by then - until stop(). The
// wake word is read from Settings here, so a changed one takes effect at the
// next start.
void start(QObject *guard, std::function<void()> heard);
// 16 kHz mono 16 bit audio, as the microphone hands it over. Main thread.
void feed(const QByteArray &pcm);
void stop();

// Whether `phrase` can be a wake word: the model has to be able to spell it,
// which takes plain letters, and it has to be long enough to tell from noise.
bool canSpell(const QString &phrase);

// A transcription with the wake word taken off the front - "Hey Burnos, what
// time is it?" is the question "what time is it?". With `anywhere`, for a
// phrase the keyword model has already heard the wake word in, everything up
// to and including the wake word goes, since that phrase may start with the
// end of something said before it, and the recogniser's spelling of the wake
// word is given more room. `found` says whether the wake word was there;
// without it the text comes back as it was.
QString withoutWakeWord(const QString &text, bool anywhere, bool *found);

} // namespace BurnOS::Wake

#endif // KEYWORDSPOTTER_H
