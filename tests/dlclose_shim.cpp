/* dlclose_shim.cpp — the two calls dlclose_test.cpp needs, exported unmangled so the host can
 * dlsym() them. It is the whole plugin lifecycle that matters here: attach (start the listener),
 * one publish, detach (which must stop and join the thread), then the host dlclose()s this file.
 * If anything still runs in here after dlclose, the process dies — which is exactly what MPC did
 * on the device on 2026-11-01. */
#include "../src/dsp/companion.h"

extern "C" int tc_attach() { return mhec::attach(); }

extern "C" void tc_publish(int slot) {
  mhec::Snapshot s;
  s.wet = 0.5f;
  s.delay_ms = 140.0f;
  s.bpm = 120.0f;
  s.rate = 0.45f;
  s.wow_pct = 0.0f;
  s.jog = 0.0f;
  s.warp = 0.0f;
  s.twist = 0.0f;
  s.echo = 0.6f;
  s.mix = 0.5f;
  s.intensity = 0.4f;
  s.reverb = 0.2f;
  s.mode = 2;
  s.fb = 0;
  s.sync = 0;
  s.drift = 0;
  s.heads[0] = 0;
  s.heads[1] = 0;
  s.heads[2] = 1;
  s.reverb_head = 0;
  mhec::publish(slot, s);
}

extern "C" void tc_detach(int slot) { mhec::detach(slot); }
