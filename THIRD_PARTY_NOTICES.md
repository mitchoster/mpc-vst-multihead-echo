# Third-party notices

MPC Multihead Echo is licensed **GPL-3.0-only** (see `LICENSE`). That follows from the engine it
builds on; it is not a choice we could soften.

## Vendored engine

| | |
|---|---|
| Project | [`athousanddetails/schwung-tape-echo2`](https://github.com/athousanddetails/schwung-tape-echo2) |
| Version | v1.4.5, commit `10671072a6d02e9008c64136d840d4d2dda2d21b` (2026-09-03) |
| Licence | GPL-3.0-only |
| Origin | Tape Echo 2 by Dusk Audio ([`dusk-audio/dusk-audio-plugins`](https://github.com/dusk-audio/dusk-audio-plugins), GPL-3.0) |
| Where | `src/ported/` (see `src/ported/VENDORED.md` for the exact two local changes) |

The engine source is vendored unchanged apart from two documented additions — `setReverbSend()` and
`modeTapsForMode()` — so a re-vendor is a real diff. Everything else in `src/` (preamp, chorus, tape
imperfection, stereo spread, the module shell, the companion server and page) is this project's code.

## Build framework

The plugin is built with [`sd88me/mpc-vst-plugins`](https://github.com/sd88me/mpc-vst-plugins) (MIT). It
is not vendored here; `build.sh` and `test.sh` need a checkout, pointed at by `MPC_VST_PLUGINS`.

## Not included

No AIR Music Tech or Akai material: no binaries, impulse responses, presets, artwork or skin files.
This is a clean-room rebuild of the behaviour from public product documentation and listening, with its
own identity. "MPC Multihead Echo" is an independent name; it is not affiliated with or endorsed by
AIR Music Tech, inMusic or Akai Professional.
