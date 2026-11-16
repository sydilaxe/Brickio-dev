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

## Two ways, by what the Brick's kernel offers

`tools/probe-gadget.sh` (run once on a Brick) says which.

**A. The receiver is a standard USB audio device (`uac2`, else `uac1`).** brickio sets the receiver's
bottom port up as a USB Audio Class device, alongside the clock link in the same gadget (one composite
device). The primary sees it as an ordinary sound card, "Brick TrimFX", so brickio's usual path takes
it as it takes the bluebox: direct, full duplex if wanted, hot-plug, the watchdog. On the receiver, the
gadget is a sound card too: its capture is what the primary plays. Standard, so even a computer can use
a Brick as an interface.

**B. brickio's own audio channel, if the kernel has neither.** The clock link (FunctionFS) gets a
second interface: interface 0 stays byte-identical (older apps see the clock link as before and pass
over the rest), interface 1 carries audio in bulk transfers. Stereo float at 48 kHz is 384 kB/s, far
inside the port's speed. The primary's brickio lists each receiver as an output ("BRICK: TrimFX") and
sends its output to it as it plays; the receiver's lists the primary as an input ("BRICK: HI5").

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
