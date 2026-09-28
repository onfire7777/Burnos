# S.E.N.T.R.I memory

What has been done in this folder, newest last.

## 2026-09-26 17:06
- [x] Read current sources
- [x] Write glowing orb widget
- [x] Wire up context menu and exit
- [x] Rebuild and launch

## 2026-09-26 17:10
- [x] Rewrite orb and window for frameless transparency
- [x] Copy in and rebuild
- [x] Relaunch and verify

## 2026-09-26 20:30
- [x] Right-click OpenRouter key dialog
- [x] Persist and load the key
- [x] Orb click event
- [x] Wire click to parakeet STT

## 2026-09-26 20:56
- [x] Voice-reactive orb warp
- [x] OpenRouter call to minimax-m2.7
- [x] Kokoro local TTS playback
- [x] End-to-end test and validation

## 2026-09-26 21:27
- [x] Always on top
- [x] Auto-download missing models
- [x] Initializing animation on the orb
- [x] Test and validate

## 2026-09-27 03:13 - live mode
- [x] Voice activity detection in SttEngine: mic stays open, speech opens the
      recording by itself, 650 ms of quiet closes it (kHangoverMs), 320 ms of
      pre-roll is kept so no syllable is clipped, under 350 ms of voice is
      discarded as noise, 30 s length cap
- [x] Thresholds are adaptive: RMS over max(0.020, floor * 3.5) opens, under
      max(0.011, floor * 2.0) counts as silence; the floor is an EMA learned
      only while nobody is talking
- [x] Click arms live mode, orb is BLUE and brighter while a phrase is kept;
      click again disarms, orb is pale GREY (kOrbGrey / kOrbGreyDeep). The old
      amber active colour is gone
- [x] Live mode survives a turn: the mic is paused while we answer, so Kokoro's
      own voice is never taken as the next question, and reopened when the TTS
      finishes or anything fails. Only a mic failure disarms
- [x] livelog.h: every state change is appended to burnos-live.log next to the
      exe. That file is the only way to prove what the orb did, since a
      frameless orb has no text and a scheduled run cannot look at screenshots
- [x] SttEngine::selfTest(), armed only by BURNOS_VAD_SELFTEST in the
      environment: feeds 900 ms of synthetic tone then silence through the
      detector in 20 ms buffers, proving the open threshold and the 650 ms
      hangover from the log alone
- [x] Tested and validated: log showed ARMED blue, speech STARTED at rms 0.1700
      over 0.0200, speech ENDED after 650 ms of quiet with 900 ms of voice,
      1560 ms handed to parakeet, mic reopened, then DISARMED grey on the second
      click. Copy kept as burnos-live-selftest-2026-09-27.log

### Notes for next time
- build.bat in the project root builds it: vcvars64 from
  "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools", qmake from
  C:\Qt\6.10.2\msvc2022_64, then jom from C:\Qt\Tools\QtCreator\bin\jom.
  Kill BurnOS.exe first or the link fails. Output tail goes to build.log
- This box is an RDP session: the default input is "Remote Audio" and it
  delivers no buffers at all, so onAudio never runs and real speech cannot be
  tested here. QAudioSource::stateChanged is now watched and reports a mic that
  dies, but a silent device still looks armed. Use the self test on this host

## 2026-09-27 09:03
- [x] Auto-arm on startup with listening and muted hints
- [x] Diagnose why replies stopped coming back
- [x] Fix the reply path
- [x] Build, run and verify both

## 2026-09-27 13:25
- [x] Silence timeout as a setting, default one second
- [x] Keep the microphone open while it thinks and fold in new speech
- [x] Add the control to the settings dialog
- [x] Build and verify

## 2026-09-27 14:19
- [x] Computer use primitives: launch, windows, controls, click, fill, key
- [x] Jev decisions client against the alpha decisions endpoint
- [x] Route a spoken command through Jev and speak the result
- [x] Build and verify

## 2026-09-28 19:09
- [x] Typing into UWP windows like Microsoft Store
- [x] Launching apps that have no exe command
- [x] Force stop everything on mute
- [x] Answer text inside the orb, retire the bubble
- [x] Orb size setting

## 2026-09-28 21:38
- [x] Research Composio API
- [x] Read BurnOS settings code
- [x] Build Composio client
- [x] Manage Integrations dialog
- [x] Wire in and build
