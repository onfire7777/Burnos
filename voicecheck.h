#ifndef VOICECHECK_H
#define VOICECHECK_H

#include <QByteArray>
#include <QList>
#include <QString>

#include <functional>

class QObject;

// WHOSE VOICE IT IS. Silero says whether something is speech; a television is
// speech too. This says whether a phrase sounds like the person who taught
// Burnos their voice: a small speaker model (3D-Speaker ERes2Net, 25 MB) turns
// a stretch of audio into a VOICEPRINT - 192 numbers for how a voice sounds,
// nothing about what it said - and two voiceprints of the same person lie
// close together. It runs on this machine, through sherpa-onnx's C library
// beside the onnxruntime the voice already uses, and nothing leaves the PC.
namespace BurnOS::Voice {

using Print = QList<float>;

// The library and the model are both on disk.
bool available();

// Loads the model now, off the main thread, and runs it once, so the first
// phrase of the day is checked as quickly as every later one. Called when
// Burnos opens; the model then stays loaded until Burnos closes.
void preload();

// The voiceprint of 16 kHz mono 16 bit audio, made off the main thread, one at
// a time and in the order asked. `done` hears it on the main thread - empty
// when none could be made - unless `guard` is gone by then.
void print(QObject *guard, const QByteArray &pcm, std::function<void(const Print &)> done);

// The same, here and now. Blocking; for the probe.
Print printNow(const QByteArray &pcm, QString *error = nullptr);

// How alike two voiceprints are: about 0.6 and up for one person, about 0.3
// and down for two. -1 when either is missing.
float alike(const Print &a, const Print &b);

// The spoken part of a recording - the pauses taken out by loudness alone - and
// how loud that speech is. For teaching Burnos a voice, where the person reads
// for half a minute and the detector's opinion is not needed.
struct Spoken
{
    QByteArray pcm;
    float level = 0.0f;
};
Spoken spoken(const QByteArray &pcm);

// THE PART OF A PHRASE THAT WAS SAID AT THE MICROPHONE. With a television on,
// the detector cannot tell its speech from the owner's, so a phrase is often
// the television, then the owner, then the television again - and a voiceprint
// of all of that sounds like neither. The owner is the loud one: this is every
// stretch of the phrase within reach of its loudest moment, with the instant
// either side so that no consonant is cut off, and nothing of what was only
// ever quiet. In a quiet room that is simply the speech, pauses out.
struct Near
{
    QByteArray pcm;     // that part, for the voiceprint
    float level = 0.0f; // how loud its loud moments are
    int ms = 0;         // how much of it there is
    // Where the near voice begins and ends in the phrase, in bytes: what lies
    // outside was somebody further away, or nobody. Drawn generously, so that
    // a soft first or last word is inside - unless the phrase is `crowded`,
    // with a quieter voice heard in it as well, when it is drawn tightly
    // enough to leave that voice out.
    qsizetype from = 0;
    qsizetype to = 0;
    // Only for a crowded phrase: all of it, as long as it was, with whatever
    // lies between the near voice's own sounds silenced - the other voice in
    // the room, which the recogniser otherwise writes down as words in front
    // of a lone "unmute". Turned down to a fifth it still wrote them: quiet
    // speech in silence is as easy to read as loud. Empty for a phrase that
    // is not crowded.
    QByteArray gated;
};
Near nearPart(const QByteArray &pcm, bool crowded = false);

} // namespace BurnOS::Voice

#endif // VOICECHECK_H
