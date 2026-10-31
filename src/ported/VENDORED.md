# Vendored engine source (`vst/src/ported/`)

Same core as `../../proto/vendor` and `../../TapeEcho/vst/src`, from the same upstream
commit — **plus two local additions** (below). Nothing else is modified, so a re-vendor is a
real `diff` and every deviation is listed here.

| | |
|---|---|
| Upstream | `https://github.com/athousanddetails/schwung-tape-echo2` |
| Tag | **v1.4.5** |
| Commit | `10671072a6d02e9008c64136d840d4d2dda2d21b` (2026-09-03) |
| Licence | **GPL-3.0-only** |
| Core origin | Tape Echo 2 by Dusk Audio (`github.com/dusk-audio/dusk-audio-plugins`, GPL-3.0) |
| Vendored | 2026-10-28 |

## Local additions (each one marked in the source)

### 2. `modeTapsForMode()` — 2026-11-01

`TapeEchoDSP.hpp` / `TapeEchoDSP.cpp`. The mode table (`kModeTable`: which heads a mode reads,
whether it feeds the spring) is private to the core, and the companion page has to draw exactly
what the audio path does. Rather than keeping a second copy of the table in the module, a static
getter returns the three head gains and the reverb flag of a mode.

Read-only, no audio-path change: it reads the same `constexpr` table the render loop uses, so a
re-vendor cannot let the drawing and the sound drift apart.

### 1. `setReverbSend()` — 2026-10-31

`TapeEchoDSP.hpp` / `TapeEchoDSP.cpp`. Upstream feeds the spring tank from the *mode table*
(`ModeConfig::reverb`), so the reverb knob is dead in the modes without a reverb head
(H1, H2, H3, H2+3). That is correct for an RE-201; this port follows the RE-202, whose
REVERB VOL is independent of the head mode.

Added: an atomic `pReverbSend` (default `-1`), a setter, and one line in the per-block
update — `sendOverride >= 0 ? sendOverride : m.reverb`. **With the default the behaviour is
byte-for-byte upstream**; only a caller that sets it (our module, from the `reverb` knob)
changes anything.

## Already present upstream-of-us (from the Schwung fork, not ours)

* `setPingPong()` and the per-head ping-pong stage (`TapeEchoDSP.hpp`, marked
  "Schwung addition").
* The `host/` headers and the Schwung shell are not vendored here; the module drives the
  core class directly.
