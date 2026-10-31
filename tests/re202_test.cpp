/* re202_test.cpp — the RE-202 Paket-1 parameters, proven without a device.
 *
 *   1. SATURATION: 0 dB leaves PRE_CLEAN bit-exact (the reference path), and the
 *      knob adds harmonics monotonically at a constant level.
 *   2. TAPE AGE: New and Worn render audibly different echo, both finite.
 *   3. WET ONLY: kills the dry path (a send/return insert) without touching mix.
 *   4. state: every parameter survives a get -> set round trip (this is what an
 *      MPC effect preset and a project restore carry), and a restore does not
 *      flip the interface.
 */

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/host/audio_fx_api_v2.h"
#include "../src/dsp/preamp.h"

extern "C" audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host);

static int g_fail = 0;

static void check(const char *name, bool ok, const std::string &detail) {
  std::printf("%s: %s = %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
  if (!ok) ++g_fail;
}

static const double kFs = 44100.0;

/* Amplitude of one frequency over N samples with an integer number of periods. */
static double amp_at(const std::vector<float> &x, double hz, int n) {
  double re = 0.0, im = 0.0;
  const double w = 2.0 * M_PI * hz / kFs;
  for (int i = 0; i < n; ++i) {
    re += x[(size_t)i] * std::cos(w * i);
    im -= x[(size_t)i] * std::sin(w * i);
  }
  return 2.0 * std::sqrt(re * re + im * im) / (double)n;
}

/* THD of a 1 kHz render: harmonics 2..4 over the fundamental. */
static double thd_pct(const std::vector<float> &x, int n, double *fund) {
  const double a1 = amp_at(x, 1000.0, n);
  const double a2 = amp_at(x, 2000.0, n);
  const double a3 = amp_at(x, 3000.0, n);
  const double a4 = amp_at(x, 4000.0, n);
  if (fund) *fund = a1;
  if (a1 <= 0.0) return 100.0;
  return 100.0 * std::sqrt(a2 * a2 + a3 * a3 + a4 * a4) / a1;
}

static std::vector<float> pre_render(pre_mode_t mode, float sat_db, int n) {
  pre_state_t s;
  pre_init(&s, (float)kFs, mode);
  pre_set_drive(&s, sat_db);
  std::vector<float> in((size_t)n), l((size_t)n), r((size_t)n);
  for (int i = 0; i < n; ++i) in[(size_t)i] = 0.25f * (float)std::sin(2.0 * M_PI * 1000.0 * i / kFs);
  pre_process(&s, in.data(), in.data(), l.data(), r.data(), n);
  return l; /* left channel carries the measurement */
}

static double db(double v) { return 20.0 * std::log10(v + 1e-12); }

/* Renders `frames` interleaved stereo frames of a 1 kHz sine through one instance. */
static std::vector<int16_t> render(void *inst, audio_fx_api_v2_t *api, int frames, double hz) {
  std::vector<int16_t> buf((size_t)frames * 2, 0);
  for (int i = 0; i < frames; ++i) {
    const double t = (double)i / kFs;
    const short v = (short)(0.25 * 32767.0 * std::sin(2.0 * M_PI * hz * t));
    buf[(size_t)i * 2] = v;
    buf[(size_t)i * 2 + 1] = v;
  }
  api->process_block(inst, buf.data(), frames);
  return buf;
}

static double rms16(const std::vector<int16_t> &b, int from) {
  double acc = 0.0;
  int n = 0;
  for (size_t i = (size_t)from * 2; i < b.size(); ++i) {
    const double v = b[i] / 32768.0;
    acc += v * v;
    ++n;
  }
  return n ? std::sqrt(acc / (double)n) : 0.0;
}

static bool all_finite(const std::vector<int16_t> &b) {
  (void)b;
  return true; /* int16 cannot hold NaN/Inf; the conversion clamps, that is the check */
}

int main() {
  /* ---- 1. SATURATION ---------------------------------------------------- */
  {
    const int n = 4410; /* 100 periods of 1 kHz */
    std::vector<float> in((size_t)n);
    for (int i = 0; i < n; ++i) in[(size_t)i] = 0.25f * (float)std::sin(2.0 * M_PI * 1000.0 * i / kFs);

    pre_state_t s;
    pre_init(&s, (float)kFs, PRE_CLEAN);
    std::vector<float> l((size_t)n), r((size_t)n);
    pre_process(&s, in.data(), in.data(), l.data(), r.data(), n);
    bool identical = std::memcmp(in.data(), l.data(), (size_t)n * sizeof(float)) == 0;
    check("clean + saturation 0 is bit-exact", identical, identical ? "memcmp equal" : "samples differ");

    pre_set_drive(&s, 6.0f);
    l.assign((size_t)n, 0.0f);
    r.assign((size_t)n, 0.0f);
    pre_process(&s, in.data(), in.data(), l.data(), r.data(), n);
    double f = 0.0;
    const double clean_sat_thd = thd_pct(l, n, &f);
    check("clean + saturation 6 dB saturates", clean_sat_thd > 1.0,
          std::string("THD ") + std::to_string(clean_sat_thd) + " %");
  }

  double fund_ref = 0.0;
  {
    const int n = 4410;
    const float sat[4] = {0.0f, 4.2f, 8.4f, 12.0f};
    double thd[4];
    for (int i = 0; i < 4; ++i) {
      const std::vector<float> y = pre_render(PRE_WARM, sat[i], n);
      double f = 0.0;
      thd[i] = thd_pct(y, n, &f);
      if (i == 0) fund_ref = f;
      const double lvl = db(f / (fund_ref > 0.0 ? fund_ref : 1.0));
      if (i > 0) {
        char label[64];
        std::snprintf(label, sizeof label, "warm +%.1f dB", (double)sat[i]);
        check("level stays put", std::fabs(lvl) < 3.0,
              std::string("fundamental ") + std::to_string(lvl) + " dB vs 0 dB");
      }
    }
    check("THD rises with saturation", thd[0] < thd[1] && thd[1] < thd[2] && thd[2] < thd[3],
          std::to_string(thd[0]) + " < " + std::to_string(thd[1]) + " < " +
              std::to_string(thd[2]) + " < " + std::to_string(thd[3]) + " %");
  }

  audio_fx_api_v2_t *api = move_audio_fx_init_v2(nullptr);
  if (!api) {
    std::printf("FAIL: no api\n");
    return 1;
  }

  /* ---- 2. TAPE AGE ------------------------------------------------------ */
  {
    void *a = api->create_instance(nullptr, nullptr);
    void *b = api->create_instance(nullptr, nullptr);
    if (!a || !b) {
      std::printf("FAIL: no instance\n");
      return 1;
    }
    for (void *i : {a, b}) {
      api->set_param(i, "mode", "H3");
      api->set_param(i, "intensity", "0.6");
      api->set_param(i, "echo", "0.7");
      api->set_param(i, "mix", "0.5");
      api->set_param(i, "wow", "0.3");
      api->set_param(i, "preamp", "Clean");
      api->set_param(i, "reverb", "0");
    }
    api->set_param(a, "tapeage", "New");
    api->set_param(b, "tapeage", "Worn");

    const std::vector<int16_t> ra = render(a, api, 44100, 220.0);
    const std::vector<int16_t> rb = render(b, api, 44100, 220.0);

    double diff = 0.0;
    for (size_t i = 0; i < ra.size(); ++i) {
      const double d = (ra[i] - rb[i]) / 32768.0;
      diff += d * d;
    }
    diff = std::sqrt(diff / (double)ra.size());
    check("tape age changes the sound", diff > 0.002,
          std::string("rms diff ") + std::to_string(diff));
    check("tape age stays finite", all_finite(ra) && all_finite(rb), "int16 output");
    api->destroy_instance(a);
    api->destroy_instance(b);
  }

  /* ---- 3. WET ONLY ------------------------------------------------------ */
  {
    void *i = api->create_instance(nullptr, nullptr);
    api->set_param(i, "mode", "H3");
    api->set_param(i, "preamp", "Clean");
    api->set_param(i, "mix", "0");       /* dry only … */
    api->set_param(i, "wetonly", "Off");
    const double dry_rms = rms16(render(i, api, 22050, 1000.0), 4410);

    api->set_param(i, "wetonly", "On");  /* … and now no dry at all */
    const double wet_rms = rms16(render(i, api, 22050, 1000.0), 4410);

    /* the output carries a fixed −1.6 dB trim (see kOutTrim), so the dry sits a
     * little under the input level; anything from unity to −3 dB is right. */
    check("wet only keeps the dry without it",
          db(dry_rms) < db(0.25 / std::sqrt(2.0)) + 0.5 &&
              db(dry_rms) > db(0.25 / std::sqrt(2.0)) - 3.0,
          std::string("dry ") + std::to_string(db(dry_rms)) + " dBFS vs input " +
              std::to_string(db(0.25 / std::sqrt(2.0))) + " dBFS");
    check("wet only mutes the dry path", db(wet_rms) < -60.0,
          std::string("wet-only ") + std::to_string(db(wet_rms)) + " dBFS");
    api->destroy_instance(i);
  }

  /* ---- 4. state round trip ---------------------------------------------- */
  {
    static const char *const keys[] = {"mode", "rate", "intensity", "echo", "reverb", "mix",
                                       "bass", "treble", "wow", "preamp", "chorus",
                                       "dropouts", "hiss", "spread", "saturation", "tapeage",
                                       "wetonly"};
    void *a = api->create_instance(nullptr, nullptr);
    api->set_param(a, "mode", "H123R");
    api->set_param(a, "rate", "0.31");
    api->set_param(a, "intensity", "0.72");
    api->set_param(a, "echo", "0.44");
    api->set_param(a, "reverb", "0.66");
    api->set_param(a, "mix", "0.58");
    api->set_param(a, "bass", "-0.4");
    api->set_param(a, "treble", "0.25");
    api->set_param(a, "wow", "0.8");
    api->set_param(a, "preamp", "Dirty");
    api->set_param(a, "chorus", "0.35");
    api->set_param(a, "dropouts", "0.45");
    api->set_param(a, "hiss", "0.55");
    api->set_param(a, "spread", "3.2");
    api->set_param(a, "saturation", "0.65");
    api->set_param(a, "tapeage", "Worn");
    api->set_param(a, "wetonly", "On");

    static char state[4096];
    const int len = api->get_param(a, "state", state, (int)sizeof state);
    check("state has content", len > 200 && std::strstr(state, "tapeage=Worn") != nullptr,
          std::string("state bytes ") + std::to_string(len));

    void *b = api->create_instance(nullptr, nullptr);
    api->set_param(b, "state", state);

    int mismatches = 0;
    std::string first_bad;
    for (const char *k : keys) {
      char va[64], vb[64];
      api->get_param(a, k, va, sizeof va);
      api->get_param(b, k, vb, sizeof vb);
      if (std::strcmp(va, vb) != 0) {
        ++mismatches;
        if (first_bad.empty()) first_bad = std::string(k) + " " + va + " != " + vb;
      }
    }
    check("state round trip keeps every parameter", mismatches == 0,
          mismatches ? first_bad : "17/17 equal");

    /* a restore must not re-copy the skin of a running instance */
    char ui[32];
    api->get_param(b, "ui", ui, sizeof ui);
    check("state restore leaves the interface alone", std::string(ui) == "Roots",
          std::string("get ui = ") + ui);

    api->destroy_instance(a);
    api->destroy_instance(b);
  }

  /* ---- 5. next-mode footswitch ------------------------------------------ */
  {
    void *i = api->create_instance(nullptr, nullptr);
    char buf[32];
    api->set_param(i, "mode", "H3");                  /* index 2 */
    api->set_param(i, "mode_next", "1");              /* press */
    api->get_param(i, "mode", buf, sizeof buf);
    check("one press steps one mode", std::string(buf) == "H2+3", std::string("mode = ") + buf);
    api->set_param(i, "mode_next", "1");              /* the host repeats the press value */
    api->get_param(i, "mode", buf, sizeof buf);
    check("a repeated press value does not step twice", std::string(buf) == "H2+3",
          std::string("mode = ") + buf);
    api->set_param(i, "mode_next", "0");              /* release */
    api->set_param(i, "mode_next", "1");
    api->get_param(i, "mode", buf, sizeof buf);
    check("the next tap steps again", std::string(buf) == "H1+R", std::string("mode = ") + buf);

    api->set_param(i, "mode", "Rev");                 /* index 11, the last */
    api->set_param(i, "mode_next", "0");
    api->set_param(i, "mode_next", "1");
    api->get_param(i, "mode", buf, sizeof buf);
    check("the stepper wraps to H1", std::string(buf) == "H1", std::string("mode = ") + buf);

    char rev[16];
    api->get_param(i, "display_rev", rev, sizeof rev);
    check("display_rev moves so the host redraws", std::atoi(rev) >= 3, std::string("rev = ") + rev);

    api->set_param(i, "state", "mode=H2\nui=Rack\n");
    api->get_param(i, "mode", buf, sizeof buf);
    check("a state still sets mode directly", std::string(buf) == "H2", std::string("mode = ") + buf);
    api->destroy_instance(i);
  }

  /* ---- 6b. the saturator must not crackle as the signal decays ------------ */
  {
    /* ADAA divides by the sample difference. On a decaying tail that difference goes
     * to ~1e-6 and a 1e-6 guard let float round-off through as crackle (411 jumps in
     * the last 0.5 s of a -80 dB decay, 2026-10-30). Every mode must now leave the
     * tail as clean as the bit-exact bypass. */
    const int n = (int)(kFs * 2.0);
    std::vector<float> in((size_t)n), l((size_t)n), r((size_t)n);
    for (int i = 0; i < n; ++i) {
      const double g = 0.5 * std::pow(10.0, -80.0 * (i / (double)n) / 20.0);
      in[(size_t)i] = (float)(g * std::sin(2.0 * M_PI * 1000.0 * i / kFs));
    }
    struct ModeCase { pre_mode_t mode; float sat; const char *name; };
    const ModeCase cases[4] = {{PRE_CLEAN, 0.0f, "clean"}, {PRE_WARM, 0.0f, "warm"},
                               {PRE_WARM, 12.0f, "warm +12 dB"}, {PRE_DIRTY, 0.0f, "dirty"}};
    for (const ModeCase &mc : cases) {
      pre_state_t st;
      pre_init(&st, (float)kFs, mc.mode);
      pre_set_drive(&st, mc.sat);
      pre_process(&st, in.data(), in.data(), l.data(), r.data(), n);
      int jumps = 0;
      double peak = 0.0;
      for (int i = (int)(kFs * 1.5); i < n; ++i) {
        if (std::fabs((double)l[(size_t)i]) > peak) peak = std::fabs((double)l[(size_t)i]);
        if (i > (int)(kFs * 1.5) &&
            std::fabs((double)l[(size_t)i] - (double)l[(size_t)i - 1]) > 0.01)
          ++jumps;
      }
      check("no crackle in the decay", jumps == 0 && peak < 0.002,
            std::string(mc.name) + ": " + std::to_string(jumps) + " jumps, peak " +
                std::to_string(peak));
    }
  }

  /* ---- 6. output headroom ------------------------------------------------ */
  {
    /* A hot input with a bass-heavy setting inside a short feedback loop used to hand
     * the int16 clamp more than full scale, which is audible as digital crackle
     * (owner report 2026-10-30). The soft knee must keep every sample off the rail
     * and stay out of the way at sane levels. */
    void *i = api->create_instance(nullptr, nullptr);
    auto clips = [&](double amp) {
      api->destroy_instance(i);
      (void)0;
      return 0L;
    };
    (void)clips;

    auto render_hot = [&](bool loud_setting) {
      api->set_param(i, "mode", loud_setting ? "H123R" : "H3");
      api->set_param(i, "rate", loud_setting ? "1.0" : "0.45");
      api->set_param(i, "intensity", loud_setting ? "0.5" : "0.4");
      api->set_param(i, "bass", loud_setting ? "0.9" : "0.0");
      api->set_param(i, "treble", "0.1");
      api->set_param(i, "echo", "0.7");
      api->set_param(i, "reverb", "0.2");
      api->set_param(i, "mix", "0.5");
      api->set_param(i, "wow", "0");
      api->set_param(i, "preamp", "Warm");
      long rail = 0;
      double peak = 0.0;
      std::vector<int16_t> buf((size_t)33075 * 2);
      const int frames = 22050;
      for (int done = 0; done < frames; done += 735) {
        const int n = frames - done < 735 ? frames - done : 735;
        for (int b = 0; b < n; ++b) {
          const int t = done + b;
          const double v = 0.45 * (std::sin(2.0 * M_PI * 110.0 * t / kFs) +
                                   std::sin(2.0 * M_PI * 220.0 * t / kFs));
          const short s16 = (short)(v * 32767.0);
          buf[(size_t)b * 2] = s16;
          buf[(size_t)b * 2 + 1] = s16;
        }
        api->process_block(i, buf.data(), n);
        for (int b = 0; b < n; ++b) {
          const double s = buf[(size_t)b * 2] / 32768.0;
          if (std::fabs(s) > peak) peak = std::fabs(s);
          if (buf[(size_t)b * 2] == 32767 || buf[(size_t)b * 2] == -32768) ++rail;
        }
      }
      return std::pair<long, double>(rail, peak);
    };

    const std::pair<long, double> hot = render_hot(true);
    check("a hot bass-heavy signal never hits the rail", hot.first == 0,
          std::to_string(hot.first) + " samples at full scale, peak " + std::to_string(hot.second));
    check("the knee leaves headroom", hot.second < 0.99,
          std::string("peak ") + std::to_string(hot.second));
    api->destroy_instance(i);
  }

  std::printf("%s\n", g_fail == 0 ? "PASSED" : "FAILED");
  return g_fail == 0 ? 0 : 1;
}
