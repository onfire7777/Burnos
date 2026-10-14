# Burnos — developer friction log

Prepared October 14, 2026 for the Alexa+ track of Build, Ship, Shape: Amazon Developer Hackathon.

## Scope and evidence

These entries describe actual issues documented in Burnos's development history between September 28 and October 10, 2026. They concern the tools and integrations used to build Burnos. They are **not claims of defects in Amazon services or of failures observed on an Alexa device**.

Each entry includes the task, steps, expected and actual behavior, severity, workaround or fix, and an actionable suggestion, following the [official friction-log requirements](https://amazonappdev2026.devpost.com/rules). Severity is assessed retrospectively for this report: **High** means a core workflow failed, stalled, or lost expected persistence; **Medium** means the task took an incorrect route or required additional user intervention. These are our labels, not an official competition severity scale.

The steps below reconstruct the documented historical scenario. The cited fixes and relevant source changes were inspected for this report; the old failures were not rerun. Historical test results are explicitly attributed to commit records. Suggested improvements are new recommendations derived from those incidents, not claims that vendors have accepted or implemented them.

## 1. A text-only model was offered for a workflow that sent screenshots

**Date:** September 28, 2026.

**Affected integration:** Burnos's OpenRouter model selection and screenshot-assisted desktop workflow.

**Severity:** High — a completed desktop action could end in an error instead of a useful response.

**Task attempted:** Ask Burnos to operate the desktop and visually verify the result.

**Steps taken:**
1. Use the model offered by the then-current settings shortlist, `minimax/minimax-m2.7`.
2. Ask Burnos to open GitHub.
3. Allow the desktop workflow to attach a screenshot to the follow-up model request.

**Expected result:** The selected model accepts the inputs needed by the workflow and reports the result of the desktop action.

**Actual result:** The browser opened, but OpenRouter rejected the image-bearing request with HTTP 404 and the message `No endpoints found that support image input`. Burnos displayed Error. The development record identifies the selected model as text-only at the time. It also records a separate shortlist entry, `jev-router`, that advertised no tool support; no separate runtime failure is claimed for that entry.

**Workaround and fix used:** The first patch recognized the image-input rejection, removed the image, told the model that it could not see, retried, and avoided further screenshots for that model during the session. A subsequent patch replaced assumptions in the hand-maintained shortlist with OpenRouter `/models` capability data: image input and tool support. It validated settings and checked previously saved choices at startup. Text-only fallback was a degraded recovery path, not visual verification.

**Actionable suggestion:** Provide a concise, end-to-end capability-validation example for agent builders: validate required input modalities and tool support before accepting a model; show how to handle unavailable capability metadata and endpoint-level rejection. In Burnos, retain those checks as regression coverage whenever model-selection behavior changes.

**Attribution:** This was a mismatch between Burnos's selection logic and the chosen model's capabilities, not evidence of an OpenRouter outage.

**Evidence:** [Incident and fallback, `13ded7a`](https://github.com/onfire7777/Burnos/commit/13ded7ae01c2d40930e74200f768923766981128), especially `openrouter.cpp`; [capability validation, `d2e08ff`](https://github.com/onfire7777/Burnos/commit/d2e08ffa58cfc83c70270dd3c90f3bdf7bd73e3d), especially `modelcatalogue.cpp` and `settingsdialog.cpp`. The inspected patch reads `architecture.input_modalities` and `supported_parameters`.

## 2. Opening a website launched a browser profile without the user's existing session

**Date:** September 28, 2026.

**Affected integration:** Windows application launch and browser-profile selection.

**Severity:** Medium — the requested website opened, but in the wrong session context.

**Task attempted:** Open GitHub in the user's normal browsing environment.

**Steps taken:**
1. Ask Burnos to open GitHub.
2. Have the agent launch the browser application by name and type the address.
3. Inspect the resulting browser window and sign-in state.

**Expected result:** The site opens using the user's intended browser session.

**Actual result:** The development record reports Edge opening on `Profile 1, Please sign in`; GitHub appeared signed out. Launching the browser executable did not select the session the user expected. This was the same original task that exposed Entry 1, but a distinct failure mechanism.

**Workaround and fix used:** Add a dedicated `open_url` action that delegates HTTP and HTTPS URLs to the Windows shell, instead of making the agent start a browser by name and type into it. This delegates browser selection to the user's OS configuration. The historical patch restricts accepted schemes to HTTP and HTTPS.

**Actionable suggestion:** Agent examples should distinguish opening a user-facing URL from launching an isolated browser for research. Prefer OS URL handling for the former, and document that the actual profile/session remains dependent on the user's browser configuration. Do not promise that every OS or multi-profile setup will choose an already authenticated session.

**Attribution:** This was a Burnos integration-design issue involving browser launch behavior. It was not a GitHub authentication outage. Burnos's later isolated research browser serves a different purpose and should not be confused with this user-facing action.

**Evidence:** [Incident and fix, `13ded7a`](https://github.com/onfire7777/Burnos/commit/13ded7ae01c2d40930e74200f768923766981128), `computeruse.cpp` / `computeruse.h`, including the added `openUrl` path.

## 3. Background keyboard shortcuts lost modifiers, and WinUI shortcuts needed a different input path

**Date:** September 29, 2026; Burnos v0.8 development.

**Affected integration:** Windows message-based input, Win32 controls, and WinUI 3 applications.

**Severity:** High — intended editing shortcuts could insert text or fail to act.

**Task attempted:** Edit and save text in a target application while another window remained in front.

**Steps taken:**
1. Send background Ctrl+A to a Win32 text field or a Save dialog's filename field.
2. Send a posted Ctrl+S shortcut to the contemporary Windows Notepad application.
3. Try entering text into Notepad while it is behind File Explorer.

**Expected result:** Ctrl+A selects the existing text, Ctrl+S invokes Save, and text reaches the intended editable control.

**Actual result:** The commit reports background Ctrl+A arriving as a bare `a` in four out of four Win32-field trials, and an unwanted `a` appearing before a path in a Save dialog. It attributes this to modifier state being released before queued keys were consumed. The same record reports that WinUI 3 Notepad ignored posted Ctrl+S, and that posting text to its top-level frame failed when no focused child was reported.

**Workaround and fix used:** Keep modifier state active while synchronizing with the target's message processing; the patch uses `SendMessageTimeoutW` with `WM_NULL`. Detect WinUI-hosted shortcuts and route them through the real keyboard, reporting that choice. Route background text to the document or field window instead of the top-level frame.

**Recorded validation:** The commit reports six out of six successful select-all trials after the change, and text reaching Notepad behind File Explorer without moving focus. These are historical results from the commit, not measurements repeated for this report. The referenced probe scripts were not present in the inspected checkout.

**Actionable suggestion:** Provide a clear input-method compatibility matrix for Win32, WinUI 3, and Chromium applications. Agent tooling should expose when a background operation requires real keyboard input, and maintain small select-all, save, and background-text regression probes for each supported application family.

**Attribution:** The modifier lifetime problem was in Burnos's implementation. The WinUI behavior required a different integration path; this report does not claim that all applications support background keyboard automation.

**Evidence:** [Incident, reported trials, and fix, `4dd9b14`](https://github.com/onfire7777/Burnos/commit/4dd9b1490063d65e221466d0924742ac3179fff9), `computeruse.cpp`.

## 4. An identifier in a memory response did not mean a conversation line had been stored

**Date:** October 1, 2026; Burnos v0.10 development.

**Affected integration:** Burnos's then-current Mnemosyne capture response handling and assistant-authored memory classification.

**Severity:** High — expected conversation persistence was silently incomplete.

**Task attempted:** Persist both sides of a conversation so that later sessions can recover the relevant history.

**Steps taken:**
1. Capture assistant-authored conversation lines into the then-current memory store.
2. Continue beyond its allowance of sixteen self-generated records.
3. Inspect persisted history, including after restarting rather than relying on Burnos's in-process queue.

**Expected result:** A caller can reliably distinguish a saved line from a policy-limited or otherwise non-persisted capture; conversation history does not silently disappear behind an apparent successful response.

**Actual result:** The commit reports that the seventeenth assistant-authored capture returned an identifier but stored no record under that classification's lifetime limit. Burnos's own in-process queue masked missing persistence until restart. The failure was in treating the response as sufficient proof of storage and in how these conversational suggestions were classified.

**Workaround and fix used:** Mark Burnos-authored conversational suggestions with `metadata.reality_class = externally_suggested` while retaining lower trust than the user's own words. Add response checks using the `created` and `idempotent` fields and log a line that was not kept. The code comments explicitly state that these assistant suggestions must not become evidence for facts about the user.

**Recorded validation:** The commit says a check that each spoken line reached memory failed on the preceding commit. It reports suite totals of 41/42 and 40/42 for the two described groups, so this report does not characterize the historical suites as completely passing. Their full raw reports were not available in the inspected checkout.

**Actionable suggestion:** Make memory-write outcomes explicit in API examples: created, already present, policy-limited, or rejected, with a machine-readable reason. Show callers how to test persistence rather than relying on an identifier. Include trust-preserving classification examples so that fixing retention does not promote generated text to user-authored fact.

**Attribution:** This is feedback on the project's own memory integration and historical response semantics, not on Amazon. It describes the version used at the time; it does not assert that the same behavior persists in current Mnemosyne releases.

**Evidence:** [Incident and fix, `6b3f32a`](https://github.com/onfire7777/Burnos/commit/6b3f32ad54038615adbca2f6855ef14c6c1e878d), `agent.cpp`, including `kept()` and `ownWords()`.

## 5. A competing keyboard hook left push-to-dictate stuck after key release

**Date:** October 10, 2026; fixed in Burnos v0.18.1.

**Affected integration:** Windows keyboard state, another dictation tool's hook, and Burnos's hold-to-transcribe state machine.

**Severity:** High — dictation remained active and ordinary agent input was blocked.

**Task attempted:** Hold a configured key combination to dictate text, then release it to finish and return to normal conversation.

**Steps taken:**
1. Run the historical configuration with another dictation tool hooked to the same key.
2. Start a hold-to-transcribe interaction using a combination containing Ctrl.
3. Release Ctrl before releasing the main key.

**Expected result:** Releasing the combination ends the hold promptly and returns input to its normal mode.

**Actual result:** The development record attributes the missing main-key release to the competing hook after Ctrl was released. Windows continued reporting the main key as down. The record describes 92 seconds of speech being held for typing while the agent received none of it. The other dictation product is not identified in the inspected evidence and is not named here.

**Workaround and fix used:** Treat the hold as active only while every required key in the combination is down; release of any member ends it. Add recovery paths: another press or an orb click ends the hold, and a double-click ends the hold and voice-started transcription mode. The patch also separates speech already addressed to the agent from a newly started dictation hold.

**Recorded validation:** The commit reports tests on a test copy showing that Ctrl-first release ends the hold, the next press works, and clicking ends a hold still reported as active. These are historical tests, not fresh microphone or keyboard-hook measurements made for this report.

**Actionable suggestion:** Global-hotkey examples for voice applications should include competing-hook and missing-key-up cases. Model a chord as all required keys being down, provide an obvious recovery action, and add regression cases for every release order.

**Attribution:** This was an integration conflict plus an insufficient recovery condition in Burnos. The evidence does not establish a general defect in Windows or identify a particular third-party product as faulty.

**Evidence:** [Incident and fix, `5689d3f`](https://github.com/onfire7777/Burnos/commit/5689d3f52bf8796850d8e12ab3754d52d8bfaf3c), especially `mainwindow.cpp` and `sttengine.cpp`.

## Submission note

This log supplements the required product-feedback answers; it does not replace them. The rules provide for a reviewed bonus of up to 10%, not an automatic award for submitting a particular number of entries. These are honestly attributed developer-experience reports for the tools used in Burnos; whether and how much they contribute to that bonus is for the organizers to assess.

Repository evidence was reviewed at Burnos commit `405ef9eec7dd71854d9d6f3606ddfa2196bf6bc0`. Ensure the judging team has access to the repository and referenced commits. The accompanying evidence export preserves the original commit messages, dates, and changed-file lists for review.
