# Burnos friction log — evidence review notes

Prepared October 14, 2026. These notes explain the evidence standard and exclusions behind [the friction log](friction-log.md).

## What was inspected

- The local Burnos repository, HEAD `405ef9eec7dd71854d9d6f3606ddfa2196bf6bc0`, with a clean working tree at the original friction-log inspection.
- All 47 reachable commits dated within the submission period through that HEAD were screened by their history and messages. Selected incident commits and their relevant diffs were inspected in detail.
- The accessible Burnos cloning chat confirmed repository identity but supplied no additional incident detail.
- The project-specific durable-memory search supplied no relevant hits; it was not used to establish claims.
- The official rules were rechecked on October 14, 2026: https://amazonappdev2026.devpost.com/rules .

This review did not fetch unseen remote branches, prove that the local checkout was the newest possible remote revision, inspect every external development conversation, or rerun the historical failures. The report is deliberately bounded to the available evidence.

## Evidence strength

All five entries are supported by explicit, dated descriptions in commit messages and corroborating source changes. That establishes documented development incidents and corresponding code changes. A commit author's report of live behavior is historical testimony; it is not equivalent to an independently repeated test or a preserved raw trace.

The [exported evidence file](friction-evidence.json) records full commit IDs, author timestamps, original messages, and changed files. Author dates are the dates displayed in the report and are already expressed in Pacific offsets in these records. They should not be interpreted as precise incident start/end timestamps.

| Entry | Incident description | Corroborating implementation | Limit |
|---|---|---|---|
| 1: model capability mismatch | `13ded7a`, `d2e08ff` | Image-rejection fallback; model catalogue reads image and tool capabilities | No archived HTTP trace independently inspected; model names/capabilities are historical |
| 2: browser session | `13ded7a` | Added `openUrl` action | Same original task as Entry 1; does not establish universal browser-profile behavior |
| 3: background input | `4dd9b14` | Modifier/message synchronization; WinUI detection and real-keyboard fallback | Trial counts come from commit message; `_research` probe files were absent |
| 4: memory persistence | `6b3f32a` | `kept()` checks created/idempotent; `ownWords()` sets classification | Historical suite totals were partial, not all-green; no full suite reports inspected |
| 5: dictation hold | `5689d3f` | Checks complete key combination; recovery paths | Competing dictation product unnamed; 92-second duration is author-reported |

## Claims deliberately excluded

- No claim that an obsolete MCP version caused the HTTP 404. The documented 404 concerned unsupported image input.
- No invented Amazon/Alexa SDK outage, unavailable API, documentation error, or deployment failure.
- No claim that lack of direct Alexa access was an unexpected failure encountered by this project.
- No conversion of ordinary new features into historical bugs without an explicit incident account.
- No claim that the independent October 13 MCP persistence demo reproduced or regression-tested the historical October 1 retention problem.
- No transfer of unrelated Mnemosyne benchmark failures into Burnos's incident history.
- No invented person-hours, lost money, user counts, or failure rates. The 4/4, 6/6, 92-second, and partial-suite figures are included only with historical attribution.
- No claim that the fixed cases are still broken in the current checkout.
- No claim that these non-Amazon integration reports are guaranteed to receive a bonus. The rules ask for detailed friction entries and reserve bonus assessment to the reviewers.

## Editorial judgments

Severity labels, impact summaries, reconstructed steps, and actionable suggestions were composed for this report from the source evidence. Suggestions are phrased as improvements that would help developers; they do not assert that current vendor documentation lacks those improvements unless independently verified.

No user confirmation is needed to establish the quoted commit records. Additional raw logs, original probe scripts, or original development-chat messages would strengthen corroboration, but the report does not imply they were inspected.

## Before submitting

Keep attribution and historical-validation qualifiers in the submitted text. Make the repository available to the judges. If the Devpost form separates feedback by tool, map entries to OpenRouter, Windows/browser integration, and Mnemosyne rather than labeling them as Amazon service bugs. Product feedback remains a separate required submission item.
