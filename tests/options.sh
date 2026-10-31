#!/usr/bin/env bash
# Every option parameter round-trips through the real wrapper. No device.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MV="${MPC_VST_PLUGINS:?set MPC_VST_PLUGINS to the mpc-vst-plugins checkout}"
host() { printf '%s' "$1"; }   # Docker sees the same paths on a normal host
python3 "$MV/tools/gen_vst.py" "$HERE/vst.json" --params-h >/dev/null
docker run --rm -v "$HERE":/w -v "$(host <framework>)":/mv:ro -w "/w" gcc:12 bash -lc '
set -e
gcc -std=gnu11 -O1 -Ibuild -I/mv/wrapper -c tests/options_test.c -o /tmp/ot.o
gcc -std=gnu11 -O1 -DPLUG_EFFECT -Ibuild -I/mv/wrapper -c /mv/wrapper/vst2_wrap.c -o /tmp/w.o
gcc -std=gnu11 -O1 -DPLUG_EFFECT -Ibuild -Isrc -c /mv/adapters/schwung_fx/schwung_fx_engine.c -o /tmp/a.o
g++ -std=gnu++17 -O1 -DPLUG_EFFECT -Isrc -Isrc/ported/shared-dpf/dsp -c src/dsp/airtape_module.cpp -o /tmp/m.o
g++ -std=gnu++17 -O1 -DPLUG_EFFECT -Isrc -Isrc/ported/shared-dpf/dsp -c src/dsp/companion.cpp -o /tmp/cp.o
gcc -O1 -Isrc -c src/dsp/preamp.c -o /tmp/p.o
gcc -O1 -Isrc -c src/dsp/chorus.c -o /tmp/c.o
gcc -O1 -Isrc -c src/dsp/tape_imperfection.c -o /tmp/t.o
gcc -O1 -Isrc -c src/dsp/spread.c -o /tmp/s.o
g++ -std=gnu++17 -O1 -Isrc/ported/shared-dpf/dsp -c src/ported/core/TapeEchoDSP.cpp -o /tmp/core.o
g++ /tmp/ot.o /tmp/w.o /tmp/a.o /tmp/m.o /tmp/cp.o /tmp/p.o /tmp/c.o /tmp/t.o /tmp/s.o /tmp/core.o -lm -lpthread -o /tmp/ot
/tmp/ot'
