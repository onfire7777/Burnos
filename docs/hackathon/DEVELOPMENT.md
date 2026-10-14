# Development during the hackathon

This summary traces the native app's September–October 2026 development. It is anchored to source commits, not inferred from the presentation. The reviewed application snapshot is [`405ef9e`](https://github.com/onfire7777/Burnos/commit/405ef9eec7dd71854d9d6f3606ddfa2196bf6bc0), v0.22.0.

| Date | Change | Why it matters |
| --- | --- | --- |
| September 28 | [Recover from incompatible image requests](https://github.com/onfire7777/Burnos/commit/13ded7ae01c2d40930e74200f768923766981128) | A desktop task should still communicate its outcome when a selected model cannot see screenshots |
| September 29 | [Improve background keyboard input](https://github.com/onfire7777/Burnos/commit/4dd9b1490063d65e221466d0924742ac3179fff9) | Shortcuts and text input need to reach the intended application reliably |
| October 1 | [Detect lost memory writes](https://github.com/onfire7777/Burnos/commit/6b3f32ad54038615adbca2f6855ef14c6c1e878d) | Conversation continuity depends on confirmed persistence |
| October 10 | [Mnemosyne integration update](https://github.com/onfire7777/Burnos/commit/217e66e) and [semantic retrieval](https://github.com/onfire7777/Burnos/commit/a530d4e) | Find past context by meaning |
| October 10 | [Transcription mode](https://github.com/onfire7777/Burnos/commit/1f0ee90) and [stuck-hold fix](https://github.com/onfire7777/Burnos/commit/5689d3f52bf8796850d8e12ab3754d52d8bfaf3c) | Turn speech into text where the user is working |
| October 12 | [Loaded speech models, wake word, speaker matching](https://github.com/onfire7777/Burnos/commit/eb16b27) and [wake mode](https://github.com/onfire7777/Burnos/commit/8da6c4d) | More ways to speak to and control the assistant |
| October 13 | [Spoken commands across states](https://github.com/onfire7777/Burnos/commit/db290ce) | Consistent voice control |
| October 13 | [Separate research browser and PDF reading](https://github.com/onfire7777/Burnos/commit/405ef9eec7dd71854d9d6f3606ddfa2196bf6bc0) | Research without taking over the user's normal browser |

These are dated implementation changes. Historical performance or test claims should be read with their original conditions; see the [friction evidence methodology](friction-evidence-notes.md).
