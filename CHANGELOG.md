# Changes

## 0.1.0

The first version: the apps' shared code in one place.

- **From TrimFX:** `usbaudio`, `duplex`, `aring`, `midi`, and its `perflog` (with the false-dropout fix:
  buffer times are exact, so 128 frames no longer report a dropout every ~30 s).
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
