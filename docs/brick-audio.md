# Brick to Brick audio

One **primary** Brick is the USB host: its top port goes to a powered hub, with its audio interface
(the bluebox, say) and any number of **receiving** Bricks, each on its **bottom** port, the port that can
be a USB device. The same wiring as the apps' clock link. The primary plays into a receiver (HI5 into
TrimFX, for instance); each sees the other as an audio device, as reliably as a USB interface: in the
device lists, hot-plugged, reopened if it goes.

```
            primary Brick (HI5)                     receiving Brick (TrimFX)
 top port ── powered hub ──────────── bottom port   top port ── its own interface
                │                                    (TrimFX's in and out)
              bluebox
```

## What the bottom port can do (measured)

`tools/probe-gadget.sh` on a MustardOS Brick (kernel 4.9.191), twice:

- **No USB audio or MIDI gadget** in the kernel (`uac1`, `uac2`, `midi`): only FunctionFS, mass
  storage, MTP and PTP.
- **Bulk endpoints, one each way, bind** (the clock link's kind).
- **One isochronous endpoint binds, in either direction** (64, 196 and 294 bytes a millisecond), but
  **one each way doesn't** (error 524, not supported by the controller).

So a Brick can't be a two-way standard sound card. Full results:
[trimui-brick-resources, docs/hardware.md](https://github.com/sydilaxe/Trimui-Brick-Resources/blob/claude/relaxed-brahmagupta-yhd2my/docs/hardware.md).

## The way: brickio's own channel on bulk (B)

The clock link (FunctionFS) gets a second interface. Interface 0 stays byte-identical, so older apps
see the clock link as before and pass over the rest. Interface 1 has bulk endpoints each way, carrying
framed audio and MIDI with named ports for routing, in both directions. Stereo float at 48 kHz is
384 kB/s, far inside the port's speed.

The primary's brickio lists each receiver as an output and an input ("BRICK: TrimFX"). The receiver's
brickio lists the primary the same way ("BRICK: HI5"). MIDI between them goes in the same frames, so
it never needs a port of its own.

Bulk has no reserved slot each millisecond, but on a hub carrying one audio interface and a few Bricks
the bus is mostly idle, and the receiver's ring absorbs the jitter. Only brickio apps see these
devices; a computer doesn't.

## Built (0.2.0): the channel; managed for apps by `blinkauto` (Jaffedrone sends, Columbia Street samples)

| | |
|---|---|
| `blink.c/h` | The frames: HELLO (the app, its name and its named ports), AUDIO (float, any rate, with the heard position), MIDI, PING / PONG, NOP. A reader that takes bytes as they come (frames split across transfers, garbage passed over). Unknown types are passed over, so a newer app can add some |
| `blinkio.c/h` | A session per Brick at the other end: audio out from the app's audio thread (lock-free), audio in through brickio's ring (`aring`: cushion, drift followed), MIDI both ways by port, HELLO every half second (each side knows within 1.5 s when the other has gone), PING for the link's round trip. Transports: the follower's data channel (`bl_usb_follower`) and the leader's usbfs side (`bl_find`, `bl_usb_leader`: two reads always queued, so nothing that arrives is lost) |
| `link.c` | `link_follow_data(1)` before `link_follow_start` adds the data channel as interface 1. Off by default; interface 0 and its bytes are as before (the golden test still passes); if the port won't take both, the clock link is set up alone |
| `tools/blink-test.c` | The test on two Bricks: `follow` on the receiver (sends back what it gets), `lead` on the primary (a tone and a click a second, MIDI clock at 120 BPM): the click's round trip, losses, the rings running dry, drift, MIDI clock rate |

`tests/test_blink.c` runs two sessions over a socket pair, each with its own audio clock (100 ppm
apart), audio both ways on two ports, MIDI clock and a 3000-byte SysEx, and one side going away: no
frame lost or dropped, a 440 Hz sine received with no clicks, the ring never dry, every MIDI byte, the
peer's going noticed. The click's round trip there, through both rings in a 4-second run, was about 20 ms:
the rings start with a safe cushion and bring it down over the first minutes, so the figure that counts
is the one `blink-test` reports on the Bricks after a while.

### Testing it on two Bricks

No app open on either. Copy `build/aarch64/blink-test` (`make tools`) to each, then over SSH, as root:

1. The receiving Brick, its **bottom** port in the hub: `./blink-test follow`
2. The primary Brick, the hub in its **top** port: `./blink-test lead 60`

Each prints a line a second. What to look for: `linked`, `lost 0`, `dropped 0`, `dry` staying put after
the first seconds, the click's round trip coming down and settling, `MIDI clock 48/s` on the follower.
If the follower says the data channel didn't bind, the port takes the clock link alone, and that is the
answer to the open question of three endpoints on this controller.

### Still to do

- The device test above.
- In `usbaudio`: list a linked Brick as an output and an input beside the USB interfaces ("BRICK:
  TrimFX"), so an app needs no code of its own; then the apps, one at a time, behind a setting.
- A receiver sending back more than one stereo pair, and routing by port in the apps' SETUP.

## Maybe later: a one-way standard sound card (A)

One isochronous direction binds, so a receiver might show as a standard USB speaker (playback only)
that any host, a computer included, can use. There would be no return path on it, and it isn't proven:
the kernel's FunctionFS must also accept USB audio's class descriptors and alternate settings, and
binding isn't streaming. That needs a further probe, and it would add to B, not replace it.

## Either way

- **Clocks:** each Brick runs on its own crystal (or its interface's), so the receiver's input follows
  the drift through brickio's ring (resampling by a few parts per million, inaudibly), as it does for
  two interfaces today. The primary's stream can carry its tempo position like the link does, so the
  receiver's tempo-synced effects line up with the music, not just the sound.
- **Latency:** a period or two over the cable, plus the ring's cushion. To be measured on the Bricks
  (`blink-test lead` reports it).
- **Reliability:** found by the same scans as USB interfaces, reopened when the cable or the receiver
  comes back, and each event in the log.
- **Compatibility:** apps without brickio still see the clock link exactly as now.
- **Off unless wanted:** `<APP>_BRICKAUDIO=0` leaves it out.
