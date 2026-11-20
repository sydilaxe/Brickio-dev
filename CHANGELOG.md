# Changes

## Unreleased

- **`mclockio`: the musical clock, wired** (new). `mcio_init` / `mcio_start` connect `mclock` to brickio's
  MIDI ports and to the Bricks on the USB clock link, both ways: clock bytes, song position and the link
  position go to `mclock` on the reader's thread as they arrive; a real-time thread steps the clock out every
  millisecond to every port and every following Brick (found and dropped as they come and go); every change
  to the app is made holding the app's lock. An app that took `mclock` and did its own I/O can keep doing so.
- **`mclock`: relay** (`mclock_set_relay`, off by default). Following a clock or a leading Brick, the clock goes
  on too, stepped from this app's heard sound, with START and STOP, so gear further down lines up with what
  this app plays. `mclockio` leaves out the port the clock comes in on, so it never goes back to its source.
- **`mclock`: ratios and START OUT** (`mclock_set_ratio`, `mclock_set_start_out`; 1:1 and on by default),
  Jaffedrone's IN DIV / OUT DIV and START OUT, so it can move onto `mclock` keeping them: a clock followed at
  a ratio of the app's tempo (0.5: half time) and one sent at another (2: double time), and the clock sent
  without START and STOP. The phase lock and the tempo's threshold work in the clock's own 16ths, so a ratio
  follows as closely as 1:1: a 140 BPM clock followed at half time, 70.00 BPM, within 0.7 ms with 1 ms of
  jitter (`tests/test_mclock.c`). The tempo is set to a thousandth of a BPM (it was a hundredth). The link
  is followed at the leader's own tempo, whatever the ratio.
- **`mclock`: song position** (`mclock_song_position`): the next tick marks that 16th; while playing, the app
  moves there. `midi.c`'s parser now reads F2 (it dropped it, and F1 / F3's data bytes were read as nothing,
  as now, but without the song position).
- **`mclock`: CONTINUE lands on the tick.** CONTINUE placed the app as though the next tick came a tick
  later; with a clock that runs all the time it comes anywhere between two, which left the app up to a tick
  out (10 ms at 120 BPM) for the seconds the phase lock took. Now the first tick after CONTINUE places the
  app exactly: within 0.3 ms from that tick on, wherever CONTINUE fell (`tests/test_mclock.c`).
- **`mclock`: lost ticks counted.** A tick lost on the way (a busy hub) made the count one short, and the
  phase lock then held the app a whole tick out for good, and pulled the tempo down. A gap of 2-4 ticks'
  length (within a third of one) while the tempo is settled is now counted as the ticks it was: one in 40
  lost, the sound stays within 0.9 ms of the leader at the right tempo.
- **`mclock`: led from START.** An app on AUTO that got START before the ticks had given a tempo sent a
  START and a tick of its own back for a moment; an outside START or CONTINUE now makes it a follower at once
  (`mclock_watch` lets go if no clock follows).
- **`midi`, `link`: the readers at real-time priority, opt-in** (`midi_set_priority`, `link_follow_priority`).
  A message's time is taken when its reader wakes; at normal priority the screen's work can delay that by
  milliseconds, and a follower's tempo and phase come from those times. Refused (no permission), the threads
  run as before. `midi_set_clock`: the clock's bytes on the reader thread as they arrive (what `mclockio`
  uses); `midi_send_except`: to every port but one.
- Every addition is opt-in: an app that takes this version and calls nothing new behaves as before, but for
  the three `mclock` fixes above (CONTINUE, lost ticks, START), which only change what was wrong.

- **`usbaudio`: a reopen's own missed periods no longer count toward a period more.** The 10 s count
  starts 3 s after DUPLEX opens. Before, changing the buffer to 256 on the bluebox missed 9 periods in
  the reopen's first second, which switched 256 to 3 queued (and saved it) though it runs clean with 2.
  An `audio.txt` with `periods3@<card>=256` from that keeps it until the line is deleted.

- **`duplex`: a missed period no longer turns into an endless string of restarts.** After a start or
  restart, while the input is slow to come in, the output is topped up with silence whenever it falls
  below half a period. A USB interface's input can take longer than the output's buffer to start (the
  1010music bluebox does), and before, the output ran dry waiting for it, which restarted both, again
  and again: at 64 and 128 frames on the bluebox, 10-13 restarts a second, gaps of 80-800 ms. A start
  whose input comes in time runs exactly as before. (A first version started the output a period
  lower; on the bluebox at 128 frames the audio kept reopening, most likely as that left the driver short, so the
  output starts full again, as it always did.)
- **`usbaudio`: DUPLEX keeps a period more by itself when 2 keep missing.** With `<APP>_PERIODS` unset,
  6 missed periods in 10 s with 2 queued reopen DUPLEX with 3 queued, for that card at that buffer and
  below, and `audio.txt` keeps it (`periods3@<card>=<frames>`) so the next start begins with 3. On the
  bluebox: 64 frames with 3 queued ran with no missed period at all, 19.6 ms round trip (34 ms at 256
  with 2). `<APP>_PERIODS=2` keeps 2.
- **`duplex`, `usbaudio`: the latency as measured.** `dx_held` gives what each direction really holds,
  the kernel driver's own queue included (snd_pcm_delay). Three seconds after DUPLEX starts the log says
  so, in frames, ms and periods, and `UaStats.latency_ms` uses it from then on.

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
