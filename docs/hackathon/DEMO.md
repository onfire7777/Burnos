# Demo: one idea, carried all the way through

[Download the reveal](https://github.com/onfire7777/Burnos/raw/refs/heads/main/media/burnos-overview.mp4) · [English captions](../../media/burnos-overview.srt) · [Editable source](../../media/remotion/README.md)

## The existing film

The 2:59 film follows Aurora, a fictional beginner design workshop. Its animated desktop panels explain the product; they are not screen captures from the native app. The film includes a real memory-backend test, but no real invitation was sent, booking made, or calendar changed.

| Time | Story |
| --- | --- |
| 0:00–0:25 | Meet Burnos and introduce the workshop |
| 0:25–0:49 | Carry preferences and project context forward |
| 0:49–1:03 | Show the isolated persistence test |
| 1:03–1:22 | Keep speaking while tasks work |
| 1:22–1:42 | Research web and PDF material |
| 1:42–2:16 | Create a brief and explain connected tools |
| 2:16–2:50 | Personal controls and architecture |
| 2:50–2:59 | Close the story |

## A reproducible live demonstration

Use fictional data and an isolated Windows test account. Complete [setup](../GETTING_STARTED.md), confirm the memory server is connected, and select a model that supports tools and image input for desktop tasks. Rehearse before recording; the following is a test plan, not a claim that this exact sequence has already passed.

1. **Remember:** “Remember: Aurora is a beginner design workshop, 90 minutes long, with a $600 budget.” Ask Burnos to repeat the constraints.
2. **Research:** Ask for beginner-friendly activity ideas from a specific public page. Show the returned source and inspect it.
3. **Work while talking:** Ask for an agenda in a local demo folder. While the task runs, say “Keep the language simple.” Show the resulting file and verify the timing and budget.
4. **Resume:** Close and reopen Burnos using the same memory store. Ask “What constraints did we set for Aurora?” Show the answer without restating those constraints.
5. **Stay in control:** Demonstrate dictation in an empty document, then mute and stop controls. If showing a connected service, use an authorized test account and show a draft rather than sending anything.

For a final under-three-minute cut, spend most of the time on actual input and output. Keep the opening and closing from the reveal, replace illustrated workflow sections with the recorded app, and label time jumps. A video uploaded only to GitHub does not fulfill the required public YouTube/Vimeo link in the submission form.

## Memory evidence: precise scope

[`memory-evidence.json`](memory-evidence.json) records an isolated test on October 13, 2026 against Mnemosyne commit `be29799f462fab1b558b604ffaa4298b390cd614`. It used the SDK Streamable HTTP application through Starlette's test client, initialized protocol `2025-11-25`, discovered 60 tools, captured a fictional workshop record, reopened the SQLite store in a new application instance, and retrieved the same source ID.

This tests backend persistence across application instances. It does **not** test a network deployment, an Echo device, a full Burnos restart, or every memory feature. The film abbreviates the trace for readability. [Reproduction script](../../scripts/verify-memory.py).
