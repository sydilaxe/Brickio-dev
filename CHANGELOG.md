# Changes

## Unreleased

- **`duplex`: a missed period no longer turns into an endless string of restarts.** After a start or
  restart the output is kept fed until the input's first period comes in. A USB interface's input can
  take longer than a period to start (the 1010music bluebox does), and before, the output ran dry
  waiting for it, which restarted both, again and again: at 64 and 128 frames on the bluebox, 10-13
  restarts a second, gaps of 80-800 ms. The output now starts with the depth it should have, so the
  latency is the same after every start (before, it could differ by a period from one start to the next).
- **`duplex`, `usbaudio`: the latency as measured.** `dx_held` gives what each direction really holds,
  the kernel driver's own queue included (snd_pcm_delay). Three seconds after DUPLEX starts the log says
  so, in frames, ms and periods (3 by design), and `UaStats.latency_ms` uses it from then on.

## 0.2.0

- **The Brick link's data channel** (`blink`, `blinkio`; [docs/brick-audio.md](docs/brick-audio.md)):
  audio and MIDI both ways between Bricks over the receiver's bottom port, beside the clock link, with
  named ports. Not used by any app yet; `tools/blink-test` tests it on two Bricks.
- **`link`:** `link_follow_data(1)` adds the data channel as a second interface. Off by default. The
  clock link's interface 0 and its bytes are unchanged (`tests/golden_link.h`), so every app links to
  every other as before: an older leader finds the clock link and passes over the data interface, and a
  newer leader sees no data channel on an older follower. If the port won't take both, the clock link
  is set up alone.
- **`link`:** paths too long for its buffers are passed over rather than cut short (no change on a Brick).
- **Nothing breaks for TrimFX:** `midi_close` is back (another name for `midi_stop`, which TrimFX
  calls), and `DxConfig.rate` (new in 0.1) moved to the end, so initialisers written before it still
  line up (0: 48 kHz). `tests/test_duplex` runs one round set up the old way. Checked by building TrimFX
  with brickio's files ([docs/integrating.md](docs/integrating.md), "TrimFX").
- `make tools` builds `blink-test` and `probe-iso` for the Brick; `make check` builds them too.

## 0.1.0

The first version: the apps' shared code in one place.

- **From TrimFX:** `usbaudio`, `duplex`, `aring`, `midi`, and its `perflog`, whose dropout count is the
  apps' fixed one (the same code as HI5's, RadioScan's and brick-resources' copies after their fix):
  exact buffer times, a device clock up to 0.1 % slow followed, bursts of callbacks (PipeWire) banking
  at most two buffers of credit, a real stall still counted (`tests/test_perflog.c`).
- **New:** playback only (`UaConfig.no_input`: a synth's USB output straight through ALSA from one
  real-time thread); the rate (`UaConfig.rate`: 44.1, 48, 88.2 or 96 kHz); `ua_cores` and
  `<APP>_CORES` for apps with threads of their own to place; `midi_set_raw` (the bytes for an app's
  own parser) and `midi_set_sysex`.
- **From the apps, one copy:** `link` (wire-identical to all five apps' copies; `link_follow_start`
  now takes the app's prefix and callbacks, and `link_open` / `link_send` do the leader's side),
  `linkpos` (the apps' `midi_link_pos` and its decoding), `sync` (RadioScan's decoupled HI5S, the same
  packets as HI5, Jaffedrone and Columbia Street), `aclock` (identical).
- **The musical clock** (`mclock`): the apps' clock in / out and link following, decoupled (a
  `ClockHost`). Two differences, for gear other than the apps: the first tick after START is the
  downbeat (MIDI 1.0; a clock running before START left the apps a tick ahead), and a tempo change is
  taken in one beat rather than four. Measured against a simulated app with 0-1 ms of jitter: locked
  within 0.6 ms (the jitter itself), within 0.9 ms after a 10 % tempo change, the link exact, clock
  out within 0.9 ms of the heard sound.
- **Compatibility:** the link's USB descriptors and strings and its position message are checked
  against bytes generated from HI5's code (`tests/golden_*.h`); HI5S packets against the documented
  layout, older 32-byte packets included.
