> **RESOLVED (2026-09-02, branch `experiment/loci-coproc-acia-reliable`).** The
> per-frame headless audio generation block (`src/main.c`) now also arms itself
> when `emu->has_cast_server` and pushes the frame's stereo buffer to the cast via
> `cast_server_push_audio()` — the exact headless counterpart of the SDL callback. **No flag
> added**: gated on the cast server (already opt-in via `--cast-server`); the
> `ay_generate`/frame cost is negligible. Verified e2e: `-n --realtime --cast-server`
> + `10 SOUND 1,1000,15:20 GOTO 20` → `/audio` streams PCM (silent baseline →
> ~100 k samples, peak 5461). oriced can plug in its `/emu/audio` proxy.

# A word to the Phosphoric team — the cast audio stream (`/audio`) is silent in headless mode (`-n`)

**Date**: 2026-09-02
**From**: bmarty <bmarty@mailo.com>
**Subject**: oriced embeds Phosphoric in **headless** mode and does receive the video (`/stream`) but
**no sound** via `/audio`. Root cause identified in the code: the audio push to the cast is
driven by the **SDL callback**, which is inactive in headless mode. Precise fix request below.

---

## In a nutshell

oriced (the BASIC editor/IDE) launches the emulator with `-n --realtime --cast-server=… --http-api=…` and
shows the **live** output in the page (MJPEG). We would also like the **sound**, exposed by the cast
server on `/audio` (mono WAV 44.1 kHz 16-bit). In headless mode, this stream carries **no samples** —
even with a program that plays sound (`10 SOUND 1,1000,15 : 20 GOTO 20`).

## Root cause (verified in the code)

- The **only caller** of `cast_server_push_audio()` is **`audio_callback()`**
  (`src/audio/audio_output.c`, ~l.116).
- But `audio_callback` is the **SDL audio device callback**:
  `want.callback = audio_callback;` then `SDL_OpenAudioDevice(...)` (`src/audio/audio_output.c`, ~l.168-170).
- In **headless** mode, the SDL audio device is not opened → `audio_callback` **is never invoked**
  → the cast audio ring buffer stays empty → `/audio` streams nothing.
- **Per-frame** PSG generation does already exist in headless mode (`src/main.c`, ~l.1298-1305), via
  `ay_generate` (the comment says *"the same routine the SDL callback uses"*), **but only**
  when recording to WAV/AVI (`emu->audio_wav_fp || avi_audio`), and it **does not push** to the cast.

## Proof

In headless mode with `--cast-server`, a connection to `/audio` during an active `SOUND` delivers no
PCM. Conversely, in GUI mode (SDL open) the cast sound works — consistent with the fact that the
push depends on the SDL callback.

## Concrete request

In the per-frame headless audio generation block (`src/main.c`, ~l.1304):

1. **also trigger** this block when `emu->has_cast_server` (today: only `audio_wav_fp ||
   avi_audio`);
2. **push** the frame's samples to the cast:
   `cast_server_push_audio(&emu->cast_server, frame_pcm, WAV_FRAME_SAMPLES)`.

This is the exact headless counterpart of what the SDL callback already does in GUI mode (same `ay_generate` routine,
same push). Reversible, with no impact on GUI mode or on the existing WAV/AVI recording.

Ideally: generate **only once** per frame and feed *both* the WAV/AVI tap **and** the
cast (the existing comment already insists: `ay_generate` consumes the PSG events, do not
generate twice).

## What oriced will do next

Once the `/audio` stream is fed in headless mode: an `/emu/audio` proxy (same origin, like
`/emu/stream`) + an `<audio>` tag in the page, started on a user gesture (browser autoplay
policy). Nothing more is required on the emulator side.

## Open question

Is there a deliberate reason **not** to generate/push audio in headless mode outside recording
(per-frame CPU cost, reserved for artefacts)? If so, an **opt-in flag** (e.g. `--cast-audio`)
would suit oriced perfectly.
