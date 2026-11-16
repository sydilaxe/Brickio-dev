# brickio

USB audio, MIDI and sync for the sydilaxe TrimUI apps (TrimFX, Conduit, Columbia Street, HI5,
Jaffedrone, RadioScan) on MustardOS: one shared library, so each app gets the same low latency, the
same device handling, the same MIDI and the same sync, and a fix made once reaches every app.

**Version 0.2.0** · GPL-3.0-or-later · C on SDL2 and the kernel's ALSA files; libasound is loaded at run
time (as SDL does), so there is nothing to ship.

**The promise: nothing breaks.** Every protocol the apps already speak to one another (the USB clock
link, its position message, HI5S Wi-Fi sync) is kept byte for byte, and the tests check it against the
apps' own code. Anything new is opt-in: an app that takes brickio and sets nothing behaves as before.

## The modules

| Module | What | From |
|---|---|---|
| `usbaudio.c/h` | The audio devices: USB first (by name or the kernel's USB-Audio cards), hot-plug, the choice kept (`audio.txt`), DIRECT `plughw` (skips PipeWire / dmix), native channels and pairs, the buffer (AUTO or per interface), real-time threads on cores of their own, the watchdog, a round-trip latency test, the log | TrimFX |
| `duplex.c/h` | One USB interface straight through ALSA from one real-time thread: **full duplex** (a period in, the app's process, the same period out: no cushion, ~4 ms round trip at 64 frames) or **playback only** (a synth: ~2.7 ms out at 64 frames). 44.1 / 48 / 88.2 / 96 kHz; float to the interface's own 24- or 32-bit format | TrimFX |
| `aring.c/h` | The input's ring between two callbacks (two interfaces): a small cushion, clock drift followed by resampling, bit-exact when one interface does both | TrimFX |
| `midi.c/h` | USB MIDI: every raw MIDI port, a reader thread, messages stamped on arrival, clock out on an absolute timetable, a **raw tap** for an app that keeps its own parser, a SysEx hook | TrimFX |
| `link.c/h` | The **clock link** between Bricks over USB (the follower's bottom port as a USB device; any number follow one) | Conduit, Columbia Street, HI5, Jaffedrone, RadioScan (one copy, same bytes) |
| `blink.c/h`, `blinkio.c/h` | **The Brick link's data channel:** audio and MIDI both ways between Bricks, over the receiver's bottom port beside the clock link, with named ports ([docs/brick-audio.md](docs/brick-audio.md)). Built and tested on the host; not yet in an app | new in 0.2 |
| `linkpos.c/h` | The link's position message, `F0 7D 'T' 'L' …` (the apps' `midi_link_pos`) | the same |
| `mclock.c/h` | **The musical clock:** MIDI clock and the USB link, in and out, on one timeline (the sound as heard). Clock in: tempo from a line through four beats of arrival times, phase held by trimming the app's speed; START / CONTINUE / STOP; a leading Brick's link position wins. Clock out and the link position stepped from the heard sound. No I/O: driven by the MIDI thread, talking to the app through a `ClockHost` of callbacks | Columbia Street, HI5, Jaffedrone, RadioScan's `midi.c` (decoupled), plus the MIDI 1.0 downbeat and one-beat tempo changes |
| `sync.c/h` | **HI5S**, the Wi-Fi sync: tempo, bar phase and transport over UDP, one unit leading | RadioScan's (the others' copies, decoupled) |
| `aclock.c/h` | The audio clock: where the music is at the speaker, at any moment | HI5, Jaffedrone, Columbia Street, RadioScan (identical) |
| `perflog.c/h` | The performance log: SYS, STAT, PROC, JUMP, HINT, END, with the false-dropout fix | the apps, fixed in TrimFX |
| `brickio.h` | One include for all of it, and `BRICKIO_VERSION` | |

## Why an app would take it

| | Today (SDL's default, 16-bit, through PipeWire) | With brickio's direct path |
|---|---|---|
| Output delay, synth at 512 frames | ~29 ms (512 + PipeWire's 256 × 3 + 128) | 512 × 2 = 21 ms; **2.7 ms at 64 frames** |
| Round trip, an effect on one interface | ~60-90 ms | **~4 ms at 64 frames** (+ the interface) |
| Resolution | 16-bit | float, then the interface's 24 / 32 bits |
| Sync between apps | latency estimated as one buffer: off by PipeWire's ~18 ms | the real figure (`ua_status`), or measured (`ua_latency_test`) |
| Audio threads | anywhere, with the screen | real-time, on cores of their own |
| Shared code | five drifting copies | one, tested |

How low an interface goes depends on the interface and the load: test each at 128, then 64.

## Taking it into an app

1. Copy it in: from the app's repository, `path/to/brickio/tools/brickio-sync.sh v0.1.0 lib/brickio`
   (or `main`). It copies `src/` into `lib/brickio` and writes `lib/brickio/BRICKIO_VERSION`.
2. Compile the modules the app uses, with `-Ilib/brickio`, and link `-lpthread -ldl -lm`.
3. Keep the app's old path as the default, behind a setting (`<APP>_AUDIO_PATH=brickio`), until a
   hardware test has passed; then flip the default.

[docs/integrating.md](docs/integrating.md) goes through it module by module, for each kind of app (a
synth that only plays, an effect, a looper, a stream player) and for an app with MIDI code of its own.

### The audio, in short

```c
#include "usbaudio.h"

static void process(const float *in, float *out, int frames, int have_input, void *ud)
{
    /* the audio thread: stereo interleaved float at UaConfig.rate; in is silence with no input */
}

int main(void)
{
    ua_pin("JD");                         /* first thing: before any thread starts */
    SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO);
    UaConfig ac = {.app = "JD", .process = process, .settings = "audio.txt",
                   .no_input = 1,         /* a synth: playback only */
                   .rate = 48000};
    ua_start(&ac);
    for (;;) { /* each SDL event: ua_event(&e); each frame: ua_tick(); */ }
}
```

## Settings (in `launch.sh`, with the app's prefix)

| | |
|---|---|
| `<APP>_BUFFER` | 64-2048 frames, for every interface (otherwise chosen in the app, per interface) |
| `<APP>_AUDIO`, `<APP>_INPUT` | part of a device's name, or `internal` |
| `<APP>_DIRECT=0` | the shared path (PipeWire) instead of `plughw` |
| `<APP>_DUPLEX=0` | two SDL streams instead of the direct thread |
| `<APP>_PERIODS=3` | a period more queued on the direct path, for an interface that drops out at 2 |
| `<APP>_PIN=0`, `<APP>_CORES=out,in` | the audio threads' cores: off, or chosen (3,2 by default on four cores) |
| `<APP>_LINK=0` | leave the bottom USB port alone (no clock link) |

## Developing it

`make test` runs the host tests under ASan / UBSan: the ring against jitter, bursts, drift and stalls;
the musical clock against a simulated app (clock in with jitter at 120 and 250 BPM, a tempo change
mid-song, a clock running before START, a leading Brick's link, the clock out, the modes); the
direct path against ALSA's null device (full duplex and playback only, 48 and 96 kHz); the link's
USB descriptors and position message against the apps' own bytes; MIDI through a FIFO (running status,
real-time bytes mid-message, the raw tap, the SysEx hook); HI5S's packet layout, older units and the
election; and the Brick link's data channel between two sessions (two audio clocks 100 ppm apart, audio
both ways, MIDI, a side going away). `make check` compiles every module for the host and for the Brick (aarch64, glibc 2.28) with
warnings as errors, and the device tools (`make tools`: `blink-test`, `probe-iso`). CI runs both.

A change to a shared protocol needs a new golden file and a note in [CHANGELOG.md](CHANGELOG.md) on how
old and new apps get along. Versions are tags (`v0.1.0`); `VERSION`, `src/brickio.h` and the changelog
move together.
