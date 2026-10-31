/* t14_test.cpp — the RE-202 performance layer (Paket 2), proven offline.
 *
 *   1. SYNC: the echo time follows the host tempo (1/16 at 120 BPM = 125 ms,
 *      1/16 at 180 = 83.3 ms, 1/8 at 180 = 166.7 ms), `sync=Off` follows the knob.
 *   2. DRIFT: a small, bounded wander around the synced time.
 *   3. WARP: reverb+feedback swell while held — louder without clipping.
 *   4. TWIST: modulates the echo, Hard deeper than Normal, no stepped output.
 *
 * The motor has 350 ms of capstan inertia, so everything settles on silence first;
 * measuring straight after a change would read the glide, not the target.
 */

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/host/audio_fx_api_v2.h"
#include "../src/ported/core/TapeEchoDSP.hpp"

extern "C" audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host);

static int g_fail = 0;
static const int kBlock = 128;
static const double kFs = 44100.0;

static void check(const char *name, bool ok, const std::string &detail) {
  std::printf("%s: %s = %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
  if (!ok) ++g_fail;
}

enum Signal { kImpulse, kTone, kClicks };

struct Render {
  std::vector<float> l;
};

static void settle(audio_fx_api_v2_t *api, void *inst, double seconds) {
  std::vector<int16_t> buf((size_t)kBlock * 2, 0);
  const int blocks = (int)(seconds * kFs / kBlock);
  for (int b = 0; b < blocks; ++b) api->process_block(inst, buf.data(), kBlock);
}

static Render run_signal(audio_fx_api_v2_t *api, void *inst, int frames, Signal kind,
                         bool hold_warp, bool hold_twist) {
  Render out;
  out.l.assign((size_t)frames, 0.0f);
  std::vector<int16_t> buf((size_t)kBlock * 2);
  for (int done = 0; done < frames; done += kBlock) {
    const int n = (frames - done < kBlock) ? (frames - done) : kBlock;
    for (int i = 0; i < n; ++i) {
      const int t = done + i;
      double v = 0.0;
      if (kind == kImpulse) v = (t == 0) ? 0.9 : 0.0;
      else if (kind == kTone) v = 0.22 * std::sin(2.0 * M_PI * 220.0 * (double)t / kFs);
      else v = (t % (int)(kFs * 0.30) == 0) ? 0.8 : 0.0;
      const short s16 = (short)(v * 32767.0);
      buf[(size_t)i * 2] = s16;
      buf[(size_t)i * 2 + 1] = s16;
    }
    api->set_param(inst, "warp", hold_warp ? "1" : "0");
    api->set_param(inst, "twist", hold_twist ? "1" : "0");
    api->process_block(inst, buf.data(), n);
    for (int i = 0; i < n; ++i) out.l[(size_t)(done + i)] = buf[(size_t)i * 2] / 32768.0f;
  }
  return out;
}

static int peak_at(const Render &r, int from, int to, double floor_abs);

struct ClickRun {
  Render r;
  int times[6];
  int n;
};

/* Clicks at the given frames; `warp_on/off` and `twist_on/off` are frame windows
 * (0 … frames means "held the whole time"). */
static ClickRun run_clicks(audio_fx_api_v2_t *api, void *inst, int frames, const int *clicks, int n,
                           int warp_from, int warp_to, int twist_from, int twist_to) {
  ClickRun out;
  const int total = (frames + kBlock - 1) / kBlock * kBlock;
  out.r.l.assign((size_t)total, 0.0f);
  out.n = n;
  for (int k = 0; k < n; ++k) out.times[k] = clicks[k];
  std::vector<int16_t> buf((size_t)kBlock * 2);
  for (int done = 0; done < frames; done += kBlock) {
    for (int b = 0; b < kBlock; ++b) {
      const int t = done + b;
      bool fire = false;
      for (int k = 0; k < n; ++k) if (t == clicks[k]) fire = true;
      const short v = fire ? (short)(0.9 * 32767.0) : (short)0;
      buf[(size_t)b * 2] = v;
      buf[(size_t)b * 2 + 1] = v;
    }
    api->set_param(inst, "warp", (done >= warp_from && done < warp_to) ? "1" : "0");
    api->set_param(inst, "twist", (done >= twist_from && done < twist_to) ? "1" : "0");
    api->process_block(inst, buf.data(), kBlock);
    for (int b = 0; b < kBlock; ++b) out.r.l[(size_t)(done + b)] = buf[(size_t)b * 2] / 32768.0f;
  }
  return out;
}

/* Peak-to-peak spread of the arrival times of those clicks, in ms. */
static double arrival_spread(const ClickRun &c) {
  double lo = 1e9, hi = -1e9;
  for (int k = 0; k < c.n; ++k) {
    const int at = peak_at(c.r, c.times[k] + (int)(kFs * 0.05), c.times[k] + (int)(kFs * 0.25), 0.01);
    if (at < 0) return -1.0;
    const double d = 1000.0 * (double)(at - c.times[k]) / kFs;
    if (d < lo) lo = d;
    if (d > hi) hi = d;
  }
  return hi - lo;
}

static int peak_at(const Render &r, int from, int to, double floor_abs) {
  int best = -1;
  double bestv = floor_abs;
  for (int i = from; i < to && i < (int)r.l.size(); ++i) {
    const double v = std::fabs((double)r.l[(size_t)i]);
    if (v > bestv) { bestv = v; best = i; }
  }
  return best;
}

static double rms_window(const Render &r, int from, int to) {
  double acc = 0.0;
  int n = 0;
  for (int i = from; i < to && i < (int)r.l.size(); ++i) {
    const double v = r.l[(size_t)i];
    acc += v * v;
    ++n;
  }
  return n ? std::sqrt(acc / (double)n) : 0.0;
}

static double rms_diff(const Render &a, const Render &b) {
  double acc = 0.0;
  const size_t n = a.l.size() < b.l.size() ? a.l.size() : b.l.size();
  for (size_t i = 0; i < n; ++i) {
    const double d = (double)a.l[i] - (double)b.l[i];
    acc += d * d;
  }
  return n ? std::sqrt(acc / (double)n) : 0.0;
}

static double peak_abs(const Render &r) {
  double m = 0.0;
  for (float v : r.l) {
    const double a = std::fabs((double)v);
    if (a > m) m = a;
  }
  return m;
}

/* A clean single-head, wet-only configuration so the arrival time is unambiguous. */
static void setup(audio_fx_api_v2_t *api, void *inst, const char *bpm) {
  api->set_param(inst, "mode", "H1");
  api->set_param(inst, "rate", "0.45");
  api->set_param(inst, "intensity", "0.45");
  api->set_param(inst, "echo", "0.85");
  api->set_param(inst, "reverb", "0");
  api->set_param(inst, "mix", "1");          /* wet only: no dry to mistake for the echo */
  api->set_param(inst, "bass", "0");
  api->set_param(inst, "treble", "0");
  api->set_param(inst, "wow", "0");          /* wow would move the arrival */
  api->set_param(inst, "preamp", "Clean");
  api->set_param(inst, "saturation", "0");
  api->set_param(inst, "chorus", "0");
  api->set_param(inst, "dropouts", "0");
  api->set_param(inst, "hiss", "0");
  api->set_param(inst, "spread", "0");
  api->set_param(inst, "tapeage", "New");
  api->set_param(inst, "lfo_bpm", bpm);
}

static double delay_ms(audio_fx_api_v2_t *api, void *inst, const char *bpm, const char *sync,
                       const char *drift) {
  setup(api, inst, bpm);
  api->set_param(inst, "sync", sync);
  api->set_param(inst, "drift", drift);
  settle(api, inst, 3.0);
  const Render r = run_signal(api, inst, (int)(kFs * 0.6), kImpulse, false, false);
  const int at = peak_at(r, (int)(kFs * 0.030), (int)(kFs * 0.5), 0.01);
  return at < 0 ? -1.0 : 1000.0 * (double)at / kFs;
}

static bool near(double got, double want, double tol_pct) {
  return got > 0.0 && std::fabs(got - want) / want * 100.0 <= tol_pct;
}

int main() {
  audio_fx_api_v2_t *api = move_audio_fx_init_v2(nullptr);
  if (!api) {
    std::printf("FAIL: no api\n");
    return 1;
  }

  /* ---- 1. SYNC ---------------------------------------------------------- */
  {
    void *i = api->create_instance(nullptr, nullptr);

    const double a = delay_ms(api, i, "120", "1/16", "0");   /* 125.0 ms */
    check("1/16 at 120 BPM = 125 ms", near(a, 125.0, 3.0), std::to_string(a) + " ms");

    const double b = delay_ms(api, i, "180", "1/16", "0");   /* 83.3 ms */
    check("1/16 at 180 BPM = 83.3 ms", near(b, 83.3, 3.0), std::to_string(b) + " ms");

    const double c = delay_ms(api, i, "180", "1/8", "0");    /* 166.7 ms */
    check("1/8 at 180 BPM = 166.7 ms", near(c, 166.7, 3.0), std::to_string(c) + " ms");

    const double want_off = (double)duskaudio::TapeEchoDSP::delayMsForRepeatRate(0.45f);
    const double d = delay_ms(api, i, "120", "Off", "0");
    check("sync off follows the rate knob", near(d, want_off, 3.0),
          std::to_string(d) + " ms, core says " + std::to_string(want_off));

    api->destroy_instance(i);
  }

  /* ---- 2. DRIFT --------------------------------------------------------- */
  {
    /* A single impulse can land on a wobble zero crossing, so measure the spread of
     * three arrivals spread over a second: that is what "the time wanders" means. */
    void *i = api->create_instance(nullptr, nullptr);
    auto spread = [&](const char *drift) {
      setup(api, i, "120");
      api->set_param(i, "sync", "1/16");
      api->set_param(i, "drift", drift);
      settle(api, i, 3.0);
      Render r;
      const int total = ((int)(kFs * 2.4) + kBlock - 1) / kBlock * kBlock;  /* whole blocks */
      r.l.assign((size_t)total, 0.0f);
      std::vector<int16_t> buf((size_t)kBlock * 2);
      const int clicks[3] = {(int)(kFs * 0.30), (int)(kFs * 1.10), (int)(kFs * 1.90)};
      for (int done = 0; done < (int)r.l.size(); done += kBlock) {
        for (int b = 0; b < kBlock; ++b) {
          const int t = done + b;
          const bool fire = (t == clicks[0] || t == clicks[1] || t == clicks[2]);
          const short v = fire ? (short)(0.9 * 32767.0) : (short)0;
          buf[(size_t)b * 2] = v;
          buf[(size_t)b * 2 + 1] = v;
        }
        api->process_block(i, buf.data(), kBlock);
        for (int b = 0; b < kBlock; ++b) r.l[(size_t)(done + b)] = buf[(size_t)b * 2] / 32768.0f;
      }
      double lo = 1e9, hi = -1e9;
      for (int c : clicks) {
        const int at = peak_at(r, c + (int)(kFs * 0.05), c + (int)(kFs * 0.25), 0.01);
        if (at < 0) return -1.0;
        const double d = 1000.0 * (double)(at - c) / kFs;
        if (d < lo) lo = d;
        if (d > hi) hi = d;
      }
      return hi - lo;
    };
    const double still = spread("0");
    const double wandering = spread("1");
    check("drift moves the time", wandering > still + 0.25,
          std::to_string(still) + " -> " + std::to_string(wandering) + " ms spread");
    check("drift stays small (<= 2 % of the range)", wandering < 4.5,
          std::to_string(wandering) + " ms spread");
    api->destroy_instance(i);
  }

  /* ---- 3. WARP ---------------------------------------------------------- */
  {
    /* The meaningful promise is the swelling *tail*, so the input stops and only the
     * reverb/feedback keep going — that is what a held WARP is for. */
    void *i = api->create_instance(nullptr, nullptr);
    setup(api, i, "120");
    api->set_param(i, "sync", "Off");
    api->set_param(i, "reverb", "0.15");
    api->set_param(i, "intensity", "0.35");
    api->set_param(i, "mix", "0.5");

    auto tail = [&](bool held) {
      Render r;
      const int frames = (int)(kFs * 4.0);
      const int total = (frames + kBlock - 1) / kBlock * kBlock;
      r.l.assign((size_t)total, 0.0f);
      std::vector<int16_t> buf((size_t)kBlock * 2);
      for (int done = 0; done < frames; done += kBlock) {
        for (int b = 0; b < kBlock; ++b) {
          const int t = done + b;
          const double v = (t < (int)(kFs * 2.0))
                               ? 0.22 * std::sin(2.0 * M_PI * 220.0 * (double)t / kFs)
                               : 0.0;                       /* input stops at 2 s */
          const short s16 = (short)(v * 32767.0);
          buf[(size_t)b * 2] = s16;
          buf[(size_t)b * 2 + 1] = s16;
        }
        api->set_param(i, "warp", held ? "1" : "0");
        api->process_block(i, buf.data(), kBlock);
        for (int b = 0; b < kBlock; ++b) r.l[(size_t)(done + b)] = buf[(size_t)b * 2] / 32768.0f;
      }
      return r;
    };

    const Render off = tail(false);
    const Render on = tail(true);
    const double tail_off = rms_window(off, (int)(kFs * 2.60), (int)(kFs * 3.20));
    const double tail_on = rms_window(on, (int)(kFs * 2.60), (int)(kFs * 3.20));

    check("warp swells the tail", tail_on > tail_off * 1.3,
          std::to_string(tail_off) + " (off) -> " + std::to_string(tail_on) + " (held)");
    check("warp does not clip", peak_abs(on) < 1.0,
          std::string("peak ") + std::to_string(peak_abs(on)));

    /* Warp types must be three different characters, and every one must swell. */
    double tails[3];
    const char *names[3] = {"Normal", "Deep", "Wild"};
    for (int t = 0; t < 3; ++t) {
      api->set_param(i, "warptype", names[t]);
      const Render r = tail(true);
      tails[t] = rms_window(r, (int)(kFs * 2.60), (int)(kFs * 3.20));
      check("every warp type swells", tails[t] > tail_off * 1.3,
            std::string(names[t]) + ": " + std::to_string(tails[t]));
    }
    const double d01 = std::fabs(tails[0] - tails[1]) / (tails[0] > tails[1] ? tails[0] : tails[1]);
    const double d02 = std::fabs(tails[0] - tails[2]) / (tails[0] > tails[2] ? tails[0] : tails[2]);
    const double d12 = std::fabs(tails[1] - tails[2]) / (tails[1] > tails[2] ? tails[1] : tails[2]);
    check("the three warp types differ", d01 > 0.10 && d02 > 0.10 && d12 > 0.10,
          std::to_string(d01) + " / " + std::to_string(d02) + " / " + std::to_string(d12));
    api->destroy_instance(i);
  }

  /* ---- 4. TWIST --------------------------------------------------------- */
  {
    void *i = api->create_instance(nullptr, nullptr);
    setup(api, i, "120");
    api->set_param(i, "sync", "Off");
    api->set_param(i, "mix", "1");
    settle(api, i, 0.5);

    const int clicks[4] = {(int)(kFs * 0.20), (int)(kFs * 0.70), (int)(kFs * 1.20),
                           (int)(kFs * 1.70)};
    auto spread_with = [&](const char *type, bool hold) {
      api->set_param(i, "twisttype", type);
      const ClickRun c = run_clicks(api, i, (int)(kFs * 2.2), clicks, 4, 0, 0,
                                    hold ? 0 : 0, hold ? (int)(kFs * 2.2) : 0);
      return arrival_spread(c);
    };

    const double plain = spread_with("Normal", false);
    const double normal = spread_with("Normal", true);
    const double hard = spread_with("Hard", true);

    check("twist moves the echo", normal > plain + 1.0,
          std::to_string(plain) + " -> " + std::to_string(normal) + " ms spread");
    check("hard twist is deeper than normal", hard > normal * 1.4,
          std::to_string(normal) + " -> " + std::to_string(hard) + " ms spread");

    const ClickRun c = run_clicks(api, i, (int)(kFs * 2.2), clicks, 4, 0, 0, 0, (int)(kFs * 2.2));
    double step_max = 0.0;
    for (size_t k = 1; k < c.r.l.size(); ++k) {
      const double d = std::fabs((double)c.r.l[k] - (double)c.r.l[k - 1]);
      if (d > step_max) step_max = d;
    }
    check("twist does not step the output", step_max < 1.5,
          std::string("max sample step ") + std::to_string(step_max));
    api->destroy_instance(i);
  }

  /* ---- 5. the reverb must not depend on the head mode --------------------- */
  {
    /* Upstream feeds the spring only from the modes whose table entry carries a reverb
     * send, so H1/H2/H3/H2+3 had a dead REVERB knob (owner report 2026-10-31). */
    void *i = api->create_instance(nullptr, nullptr);
    setup(api, i, "120");
    api->set_param(i, "sync", "Off");
    api->set_param(i, "mix", "0.5");
    api->set_param(i, "intensity", "0.2");

    auto tail = [&](const char *reverb) {
      api->set_param(i, "reverb", reverb);
      settle(api, i, 0.3);
      const Render r = run_signal(api, i, (int)(kFs * 2.0), kImpulse, false, false);
      return rms_window(r, (int)(kFs * 0.90), (int)(kFs * 1.60));
    };

    api->set_param(i, "revmode", "202");
    const double dry_tail = tail("0");
    const double wet_tail = tail("0.6");
    check("reverb is audible in mode H1 (202)", wet_tail > dry_tail * 2.0,
          std::to_string(dry_tail) + " -> " + std::to_string(wet_tail) + " rms");

    /* 201 wiring: in H1 the mode table carries no reverb send, so the knob is dead again */
    api->set_param(i, "revmode", "201");
    const double tail_201 = tail("0.6");
    check("201 keeps the reverb tied to the mode", tail_201 < wet_tail * 0.1,
          std::to_string(wet_tail) + " (202) -> " + std::to_string(tail_201) + " (201) rms");
    api->set_param(i, "revmode", "202");
    api->destroy_instance(i);
  }

  std::printf("%s\n", g_fail == 0 ? "PASSED" : "FAILED");
  return g_fail == 0 ? 0 : 1;
}
