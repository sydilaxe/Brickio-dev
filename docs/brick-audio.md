# Brick to Brick audio (design, not built yet)

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
- **Latency:** a period or two over the cable, plus the ring's small cushion: a few milliseconds.
- **Reliability:** found by the same scans as USB interfaces, reopened when the cable or the receiver
  comes back, and each event in the log.
- **Compatibility:** apps without brickio still see the clock link exactly as now.
- **Off unless wanted:** `<APP>_BRICKAUDIO=0` leaves it out.
