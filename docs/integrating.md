# Taking brickio into an app

The rule: **nothing changes for a user until a hardware test says it should.** Bring brickio in, keep
the app's old path as the default behind a setting, test on a Brick, then flip the default. Each module
can be taken on its own; the order below goes from no risk to the most gain.

## TrimFX: already on brickio's audio and MIDI

TrimFX's `lib/brickio` holds the first versions of `usbaudio`, `duplex`, `aring` and `midi`. brickio's
copies are a superset: with TrimFX's settings as they are (no `rate`, no `no_input`), they open the same
devices at 48 kHz, pin the same cores, and run the same paths. Checked by building TrimFX with them
(Brick and desktop builds, no new warnings), its engine tests, and its own older ring and duplex tests
run unchanged against brickio's code (98 and 12 checks, all pass).

1. `tools/brickio-sync.sh <tag>` from TrimFX's repository. It also copies the modules TrimFX doesn't
   compile yet (`link`, `mclock`, `perflog`, ...): harmless, as the Makefile names the four it builds.
2. **Keep `src/perflog.c/h`.** TrimFX's dropout check differs from brickio's (it doesn't count the first
   0.5 s after opening, and early callbacks never hide a later stall); one will be chosen from a Brick's
   logs. Until then TrimFX's own `src/perflog.h` is the one its files include.
3. Take brickio's `tests/test_aring.c` and `tests/test_duplex.c` into `lib/brickio/tests/`, with
   `#include "../src/` changed to `#include "../`. TrimFX's older ones still pass, but warn under
   `-Wextra` about the new fields they don't set.
4. `midi_close()` stays (another name for `midi_stop`), and `DxConfig`'s new field is last, so older
   initialisers still line up.
5. Later, behind a setting: `mclock` for MIDI clock in (phase held, not just the tempo) in place of
   TrimFX's own four-beat average, and the Brick link.

## 1. Drop-in replacements (the same behaviour, one copy)

These replace files the apps already have. Copy brickio in (`tools/brickio-sync.sh`), delete the app's
copy, and adjust the few calls noted.

| App's file | brickio | What changes in the app |
|---|---|---|
| `aclock.c/h` | `aclock` | nothing (identical) |
| `perflog.c/h` | `perflog` | nothing to call differently; the false dropouts stop. `perf_set_level` is new |
| `link.c/h` | `link` | `link_follow_start()` → `link_follow_start("JD", on_bytes, NULL, notice)`: the bytes that arrived go to `on_bytes` (where the app called `midi_feed` / `midi_feed_at`), the notice line to `notice` (where it called `midi_notice`). The leader's side can use `link_open` / `link_send` instead of its own usbfs calls. The wire is byte-identical |
| `midi_link_pos` and its SysEx decoding in `midi.c` | `linkpos` | `linkpos_encode(out, max, heard, bpm, playing)`; `linkpos_decode(sx, n, &heard, &bpm, &playing)` (takes the SysEx with or without F0 / F7). Same bytes |
| `sync.c/h` (HI5S) | `sync` | RadioScan's version, which talks to the app through a `SyncClock` of callbacks (mode, bpm, position, rate, transport) rather than the app's globals. HI5, Jaffedrone and Columbia Street fill one in from their `song` / `live` / engine calls (about ten lines), and read `SyncStatus.following` where they read `have_leader`. Packets are unchanged |

## 2. The audio path

Today HI5, Jaffedrone, Columbia Street and RadioScan open SDL's default device in 16-bit: on MustardOS
that goes through PipeWire (256-frame periods × 3, plus 128 of headroom), then the app's own buffer.
brickio opens the USB interface directly instead, in float.

**A synth that only plays (Jaffedrone, HI5, Columbia Street):**

```c
ua_pin("JD");                                  /* first in main(), before any thread */
UaConfig ac = {.app = "JD", .process = render, .settings = "audio.txt", .log = perf_log,
               .no_input = 1, .rate = 48000, .opened = audio_opened};
ua_start(&ac);
```

- `render(in, out, frames, ...)` makes `frames` stereo float frames into `out` (ignore `in`). The app's
  16-bit `int16_t` render becomes float, or converts at the end (`out[i] = s16[i] / 32768.f`).
- A USB output then runs straight through ALSA from one real-time thread (`PLAYBACK` in the log): about
  2 periods out (2.7 ms at 64 frames). With no USB output, or on any failure, SDL's path as before.
- **Render-ahead (Jaffedrone's `ahead.c`):** it adds a buffer of delay for steadier timing on PipeWire's
  uneven callbacks. The direct thread is steady, so try without it; keep it as a setting. If its two
  render threads stay, place them off the audio cores: `ua_cores(&out, &in)` says which (3 and 2 on
  four cores; `<APP>_CORES` moves them).

**An effect or a looper (TrimFX, Conduit):** leave `no_input` at 0. One interface both ways runs full
duplex (`DUPLEX` in the log, ~4 ms round trip at 64 frames); two interfaces run two SDL streams with
the ring between them (drift followed).

**A stream player (RadioScan):** the decoder needs a deeper buffer; keep 1024 (`<APP>_BUFFER`). The
gains are float, the log and the latency figure for sync.

**Whichever:** pass every SDL event to `ua_event`, call `ua_tick` once a frame (it returns notes to show),
and show `ua_status` in the app's settings (device, channels, buffer, latency, the hardware's format).
SETUP-style rows: `ua_cycle_device`, `ua_cycle_pair`, `ua_cycle_buffer`, `ua_latency_test`.

## 3. Sync is only as good as the latency it's told

The apps tell the audio clock and the MIDI clock how long the sound takes to be heard:
`aclock_init(rate, bufsz)` and `midi_set_latency(bufsz / rate)`, one buffer. Through PipeWire it is
about 18 ms more than that, so linked apps on different paths would sit that far apart. With brickio:

```c
UaStatus st;
ua_status(&st);
aclock_init(st.rate, (int)(st.latency_ms * st.rate / 1000.f)); /* the output's real queue */
midi_set_latency(st.latency_ms / 1000.f);
```

Update them when `ua_tick` reports a reopen (the buffer or the device changed). For the true figure,
interface included, measure once with a cable: `ua_latency_test()`.

## 4. MIDI, when the app has MIDI code of its own

A raw MIDI port (`/dev/snd/midiC*D*`) is read by **one** opener: two readers race for the bytes. So an
app uses one MIDI reader, never both:

- **Keep the app's own `midi.c`** (the usual case: the apps' clock follow, clock out timed to the heard
  sound and link handling are richer than brickio's so far): don't call `midi_start`. Take `link`,
  `linkpos`, `sync`, `aclock` and the audio from brickio; nothing else changes.
- **Use brickio's reader, keep the app's parser:** `midi_start(NULL, wake)` then
  `midi_set_raw(feed, NULL)`, where `feed` hands the bytes to the app's parser as its own reader did
  (`midi_feed(b, n, t)`). brickio finds, opens and drops the ports; the app's logic is untouched.
- **Move to brickio's events:** `midi_poll` gives notes, CCs, program, bend, aftertouch and clock with
  their arrival times; `midi_clock_out(bpm)` sends clock from a thread of its own.

The plan is for brickio's MIDI to take in the apps' shared parts (clock follow with phase lock, clock
out to the heard sound, MIDI learn), so the first option can become the third over time.

## 5. Checking it on a Brick

With `<APP>_LOG=2` (a status line a second), play for a few minutes at 128 frames, then 64:

- `AUDIO`: `DUPLEX` or `PLAYBACK` on the direct path, the cores, real-time on;
- `HW`: the interface's real format and period;
- `STAT`: `dropouts 0`, `late calls` near 0, `gap max` near the buffer;
- no `ALSA lib ... overrun` / `underrun` lines.

Then flip the app's default to brickio and say so in its changelog.
