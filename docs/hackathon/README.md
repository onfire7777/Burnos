# Burnos — judge's guide

**A voice assistant that carries context forward and turns conversation into useful work.**

Burnos combines a native desktop voice interface, integrated persistent memory, background tasks, research, and connected tools. The central user benefit is continuity: less re-explaining, less switching between windows, and a clearer path from an idea to a useful result.

## Start here

1. [Download and watch the 2:59 reveal](https://github.com/onfire7777/Burnos/raw/refs/heads/main/media/burnos-overview.mp4).
2. [Read what is demonstrated and what is illustrated](DEMO.md).
3. [Build and run Burnos](../GETTING_STARTED.md).
4. Review [the changes made during the hackathon](DEVELOPMENT.md), [product feedback](PRODUCT_FEEDBACK.md), and [the friction log](friction-log.md).

## Track and scope

The proposed primary track is **Alexa+**, using the **simulated-experience alternative**. Burnos demonstrates a voice-assistant interaction on Windows; it is not deployed on an Echo device. The native app source is in this repository, and the animated presentation source is under [`media/remotion`](../../media/remotion).

The [official rules](https://amazonappdev2026.devpost.com/rules) allow this alternative without a prescribed SDK. A self-hosted MCP submission is a different route requiring protocol 2025-11-25 or later over Streamable HTTP. Burnos's native client currently requests an older version; a separate Mnemosyne test is not proof of that route's full compliance.

## Submission checklist

| Item | Status |
| --- | --- |
| Source, license, setup instructions | Included in this repository |
| Feature description and development history | Included |
| 2:59 animated reveal, captions, editable source | Included |
| Public YouTube or Vimeo video URL | Still needs upload and entry in Devpost |
| Clear recording of the functioning submitted experience | Recommended final recording using [this demo plan](DEMO.md); the current film is substantially illustrated |
| Product feedback | Evidence-based draft included; personal onboarding and reuse responses need the entrant's confirmation |
| Optional friction log | Five documented incidents with original commit evidence |
| Devpost form and final submission | Must be completed separately |

The deadline shown by the rules is **October 23, 2026, noon Pacific**. Judges may evaluate the written material and video without trying the code. The repository package does not by itself submit the project or establish eligibility.

## What makes this entry distinctive

- **Memory is integrated into the conversation.** Mnemosyne is used by the assistant, not merely linked in a README.
- **Talking and doing can coexist.** Separate conversation and task models let work continue while the user speaks.
- **Research has its own workspace.** Burnos's browser uses a separate profile and supports web and PDF reading.
- **The interface stays small.** Voice, dictation, and visible orb states keep interaction close to the work.

## Evidence

- [Friction log](friction-log.md), [original commit records](friction-evidence.json), and [methodology](friction-evidence-notes.md).
- [Memory test trace](memory-evidence.json): fictional data, SQLite persistence, a new server application instance, and retrieval of the same source ID.
- [Source map](../ARCHITECTURE.md): implementation files for every major feature.
- [Preparation checks](VALIDATION.md): builds, speech round trip, memory test, and presentation validation.

The Open Source mini-challenge is not claimed solely because this repository is public. Any such entry should identify the specific qualifying contribution and supply its own required details.
