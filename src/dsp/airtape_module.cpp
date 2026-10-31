/* airtape_module.cpp — MPC Multihead Echo's own Schwung audio_fx_api_v2 shell.
 *
 * Our shell around the vendored Dusk `TapeEchoDSP` core (src/ported/) plus the four
 * new stages of this project (src/dsp/): preamp, chorus, tape imperfection
 * (dropouts + hiss) and stereo spread. The vendored core stays pristine, so a
 * re-vendor is a real diff.
 *
 * Signal path per block:
 *   in -> preamp -> chorus -> dropouts/hiss -> spread -> [jog on repeat rate] -> core -> out
 *
 * Realtime rules: process_block never allocates, never logs, never touches the
 * filesystem. Setters are atomic stores; the wrapper serialises calls per instance.
 *
 * Parameter convention (wrapper/vst2_wrap.c): an option param is exchanged as its
 * INDEX, but a label is accepted too, and get_param answers with the LABEL.
 */
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "../host/audio_fx_api_v2.h"
#include "../host/plugin_api_v1.h"
#include "../ported/core/TapeEchoDSP.hpp"

#include "chorus.h"
#include "companion.h"
#include "preamp.h"
#include "spread.h"
#include "tape_imperfection.h"

namespace {

const int kMaxBlock = 1024;
const float kFs = 44100.0f;   /* the wrapper's fixed rate */

/* Gain staging into the tape loop. The core's input trim sits at unity by default
 * (0.5), which drives the loop hard enough that its internal limiter does the
 * limiting — audible as grit, and worst at saturation 0, where the preamp passes
 * the hottest peaks (measured 2026-10-30: peak 0.98 at saturation 0 vs 0.42 at 1.0).
 * −4 dB into the loop, +4 dB back at the output: net unity, 4 dB less loop drive. */
const float kLoopTrim = 0.50f;    /* core input gain control, 0.5 = unity */
/* −2 dB at the very end. The saturation knob changes the peak level a lot (a hot
 * signal peaks at 0.98 with saturation 0 and 0.45 with saturation 1), and that is
 * what made the grit "saturation-dependent": at the loud end the dry+wet sum sat on
 * the output knee. A fixed trim keeps the whole insert off the knee; the knee stays
 * as a safety net for really hot material. */
const float kOutTrim  = 0.45f;    /* core output-volume control, 0.5 = unity (−2 dB) */

/* Parameter contract. Order == VST index, so appending is the only allowed edit. */
enum {
  P_MODE = 0,
  P_RATE,
  P_INTENSITY,
  P_ECHO,
  P_REVERB,
  P_MIX,
  P_BASS,
  P_TREBLE,
  P_WOW,
  P_PREAMP,
  P_CHORUS,
  P_DROPOUTS,
  P_HISS,
  P_SPREAD,
  P_JOG,
  P_UI,
  /* --- appended 2026-10-30 (RE-202 map, Paket 1) --- */
  P_SATURATION,
  P_TAPEAGE,
  P_WETONLY,
  P_MODE_NEXT,
  /* --- appended 2026-10-30 (RE-202 map, Paket 2: the performing switches) --- */
  P_WARP,
  P_TWIST,
  P_TWISTTYPE,
  P_SYNC,
  P_DRIFT,
  /* --- appended 2026-10-31: 201/202 reverb wiring, then the warp character --- */
  P_REVMODE,
  P_WARPTYPE,
  /* --- appended 2026-11-01: on-device visualisation (state the host can draw) --- */
  P_VIZ_WET,
  P_VIZ_FB,
  P_TEMPO_BPM,
  P_DELAY_MS,
  P_MOTION,
  P_COUNT
};

const char *const kKeys[P_COUNT] = {
    "mode",   "rate",   "intensity", "echo",   "reverb",      "mix",   "bass",
    "treble", "wow",    "preamp",    "chorus", "dropouts",    "hiss",  "spread",
    "jog",    "ui",     "saturation", "tapeage", "wetonly", "mode_next",
    "warp",   "twist",  "twisttype",  "sync",    "drift",   "revmode", "warptype",
    "viz_wet", "viz_fb", "tempo_bpm", "delay_ms", "motion"};

const char *const kModeOptions[12] = {"H1",  "H2",  "H3",   "H2+3",  "H1+R", "H2+R",
                                      "H3+R", "H12+R", "H23+R", "H13+R", "H123R", "Rev"};
const char *const kPreampOptions[3] = {"Clean", "Warm", "Dirty"};
/* One interface since 2026-11-01: Rack and Classic were removed on the owner's verdict.
 * The `ui` parameter stays in the contract (append-only, index 15) and keeps the swap
 * machinery, so a stale ui.txt or a preset that still carries ui=2 degrades to Roots
 * instead of pointing at a folder that no longer exists. */
const char *const kUiOptions[1] = {"Roots"};
const char *const kUiDirs[1] = {"roots"};
const int kNumUi = 1;
/* The core quantises tape age to these three states (teQuantizeTapeAge:
 * 0 / 0.5 / 1). "Used" is the DSP's own default, so an old project keeps the
 * sound it had before the parameter existed. */
const char *const kTapeAgeOptions[3] = {"New", "Used", "Worn"};
const char *const kWetOnlyOptions[2] = {"Off", "On"};
const char *const kTwistOptions[3] = {"Normal", "Hard", "Natural"};
/* Only divisions that can land inside the core's 69.3…177.4 ms motor range:
 * 1/16 covers 85…217 BPM, 1/8 the fast end, 1/32 the slow end. Anything else
 * would just pin the motor to a rail. */
const char *const kSyncOptions[4] = {"Off", "1/32", "1/16", "1/8"};
/* 201: the spring is fed only by the modes that carry a reverb head (upstream / RE-201).
 * 202: the spring is fed whenever there is reverb to hear (RE-202 behaviour). */
const char *const kRevModeOptions[2] = {"201", "202"};
/* Three swell characters: a short hall bloom, a slow deep one, and one that also
 * drags the motor down (tape-slowdown feel). */
const char *const kWarpOptions[3] = {"Normal", "Deep", "Wild"};
const char *const kFbOptions[3] = {"Safe", "Hot", "Unstable"};

float clamp_f(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* Spring send: -1 lets the core follow its mode table (201), >= 0 overrides it (202). */
float reverb_send(int revmode, float level) {
  if (revmode == 0) return -1.0f;                    /* 201: the core follows its mode table */
  return (level > 0.001f) ? 1.0f : 0.0f;             /* 202: feed whenever there is reverb */
}

/* Output soft knee. The dry+wet sum can pass full scale long before the int16
 * clamp — hot input with a bass-heavy setting inside a short feedback loop did it
 * (measured 2026-10-30: 168 hard-clipped samples in 1.5 s at bass 0.9, rate 1.0,
 * mode H123R). A hard clamp turns that into digital crackle; everything above
 * 0.75 bends instead, with a ceiling of 0.98 so the conversion never lands on
 * the rail. Bit-transparent below the knee, so normal material is untouched (a hot
 * signal had 6.4 % of its samples above a 0.75 knee — constant soft compression is
 * audible as crackle, hence 0.90). */
float out_soft(float x) {
  const float k = 0.85f, room = 0.12f;   /* safety net; the −1.6 dB trim keeps signals below it */
  const float ax = std::fabs(x);
  if (ax <= k) return x;
  const float y = k + room * std::tanh((ax - k) / room);
  return (x < 0.0f) ? -y : y;
}

const char *text_param(int idx) {
  if (idx == P_MODE) return "mode";
  if (idx == P_PREAMP) return "preamp";
  if (idx == P_UI) return "ui";
  if (idx == P_TAPEAGE) return "tapeage";
  if (idx == P_WETONLY) return "wetonly";
  if (idx == P_TWISTTYPE) return "twisttype";
  if (idx == P_SYNC) return "sync";
  if (idx == P_REVMODE) return "revmode";
  if (idx == P_WARPTYPE) return "warptype";
  if (idx == P_VIZ_FB) return "viz_fb";
  if (idx == P_MOTION) return "motion";
  if (idx == P_JOG || idx == P_WARP || idx == P_TWIST) return kKeys[idx];
  return nullptr;
}

int text_count(int idx) {
  if (idx == P_MODE) return 12;
  if (idx == P_WETONLY) return 2;
  if (idx == P_SYNC) return 4;
  if (idx == P_REVMODE) return 2;
  if (idx == P_WARPTYPE) return 3;
  if (idx == P_JOG || idx == P_WARP || idx == P_TWIST) return 2;
  if (idx == P_VIZ_FB) return 3;
  if (idx == P_MOTION) return 2;
  if (idx == P_UI) return kNumUi;   /* Roots */
  return (idx == P_PREAMP || idx == P_TAPEAGE || idx == P_TWISTTYPE) ? 3 : 0;
}

const char *text_at(int idx, int i) {
  if (idx == P_MODE) return kModeOptions[i];
  if (idx == P_UI) return kUiOptions[i];
  if (idx == P_TAPEAGE) return kTapeAgeOptions[i];
  if (idx == P_WETONLY) return kWetOnlyOptions[i];
  if (idx == P_TWISTTYPE) return kTwistOptions[i];
  if (idx == P_SYNC) return kSyncOptions[i];
  if (idx == P_REVMODE) return kRevModeOptions[i];
  if (idx == P_WARPTYPE) return kWarpOptions[i];
  if (idx == P_VIZ_FB) return kFbOptions[i];
  if (idx == P_JOG || idx == P_WARP || idx == P_TWIST || idx == P_MOTION) return kWetOnlyOptions[i];  /* Off / On */
  return kPreampOptions[i];
}

/* ---- interface swap ------------------------------------------------------
 * MPC binds a skin at insert and has no reload call, but it re-reads
 * "Plugin Skins/TUI.json" on every insert. The interface ships as a subfolder of
 * that skin folder; the swap copies its TUI.json + Q-Links.json to the root and
 * records the name in ui.txt. No image is touched, so the instance that is
 * currently drawing keeps every texture it has loaded. The change lands on the
 * next insert. Since 2026-11-01 there is exactly one folder ("roots"): the swap
 * still runs, because that is how a stale root TUI.json gets healed. */

int copy_file(const char *src, const char *dst) {
  FILE *in = std::fopen(src, "rb");
  if (!in) return 0;
  FILE *out = std::fopen(dst, "wb");
  if (!out) { std::fclose(in); return 0; }
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, in)) > 0) {
    if (std::fwrite(buf, 1, n, out) != n) { std::fclose(in); std::fclose(out); return 0; }
  }
  std::fclose(in);
  std::fclose(out);
  return 1;
}

void write_ui_choice(const char *dir, const char *name) {
  char path[640];
  std::snprintf(path, sizeof path, "%s/Plugin Skins/ui.txt", dir);
  FILE *f = std::fopen(path, "w");
  if (!f) return;
  std::fprintf(f, "%s\n", name);
  std::fclose(f);
}

/* -1 = no usable record (missing file, or a name whose interface no longer exists —
 * "rack"/"classic" from before 2026-11-01). The caller repairs the root skin from the
 * one folder that does exist, so a device that still carries an old skin heals itself. */
int read_ui_choice(const char *dir) {
  char path[640];
  char buf[32] = {0};
  std::snprintf(path, sizeof path, "%s/Plugin Skins/ui.txt", dir);
  FILE *f = std::fopen(path, "r");
  if (!f) return -1;
  if (!std::fgets(buf, (int)sizeof buf, f)) { std::fclose(f); return -1; }
  std::fclose(f);
  for (int i = 0; i < kNumUi; ++i) {
    if (!std::strncmp(buf, kUiDirs[i], std::strlen(kUiDirs[i]))) return i;
  }
  return -1;
}

void ui_swap(const char *dir, int choice) {
  static const char *const files[2] = {"TUI.json", "Q-Links.json"};
  char src[768], dst[768];
  const int n_dirs = (int)(sizeof kUiDirs / sizeof kUiDirs[0]);
  if (!dir || !dir[0] || choice < 0 || choice >= n_dirs) return;
  for (int i = 0; i < 2; ++i) {
    std::snprintf(src, sizeof src, "%s/Plugin Skins/%s/%s", dir, kUiDirs[choice], files[i]);
    std::snprintf(dst, sizeof dst, "%s/Plugin Skins/%s", dir, files[i]);
    copy_file(src, dst);
  }
  write_ui_choice(dir, kUiDirs[choice]);
}

struct airtape_t {
  duskaudio::TapeEchoDSP dsp;
  pre_state_t pre;
  chorus_state_t chorus;
  imperfection_state_t imp;
  spread_state_t spread;

  char dir[512];      /* plugin folder, used for the interface swap */
  int ui_adopted;     /* choice read from ui.txt at insert */
  int slot;           /* companion page slot (-1 = server off), see companion.h */
  std::atomic<float> values[P_COUNT];
  std::atomic<float> jog_target;
  std::atomic<int> display_rev{0};   /* HAS_DISPLAY_REV: bumps when the engine changes a param itself */
  int mode_next_held;                /* edge detector for the MODE footswitch */
  float jog_cur;      /* smoothed jog amount, audio thread only */
  /* --- Paket 2 transport state (audio thread only) --- */
  float bpm;          /* host tempo, from the "lfo_bpm" pseudo-key */
  float warp_t;       /* 0..1 swell, ramped up while WARP is held */
  float twist_t;      /* 0..1 engage, ramped */
  float twist_phase;  /* LFO phase 0..1 */
  float drift_phase;  /* slow wander phase 0..1 */
  /* on-device visualisation state */
  float viz_sumsq;    /* output energy since the last update */
  int   viz_count;
  int   viz_acc;
  float viz_wet_s;    /* smoothed bar position */
  float last_rate;    /* the repeat rate actually sent to the core */
  float base_rate;    /* the rate before drift/twist/jog, for the wow readout */

  float scratch_l[kMaxBlock];
  float scratch_r[kMaxBlock];
  float work_l[kMaxBlock];
  float work_r[kMaxBlock];

  explicit airtape_t(const char *) { set_defaults(); }

  void set_defaults() {
    values[P_MODE].store(2.0f);       /* H3 */
    values[P_RATE].store(0.45f);
    values[P_INTENSITY].store(0.40f);
    values[P_ECHO].store(0.60f);
    values[P_REVERB].store(0.20f);
    values[P_MIX].store(0.50f);
    values[P_BASS].store(0.0f);
    values[P_TREBLE].store(0.0f);
    values[P_WOW].store(0.15f);
    values[P_PREAMP].store(1.0f);     /* Warm */
    values[P_CHORUS].store(0.0f);
    values[P_DROPOUTS].store(0.0f);
    values[P_HISS].store(0.0f);
    values[P_SPREAD].store(0.0f);
    values[P_JOG].store(0.0f);
    values[P_UI].store(0.0f);  /* Night */
    ui_adopted = 0;
    jog_target.store(0.0f);
    jog_cur = 0.0f;
    mode_next_held = 0;
    display_rev.store(0);
    /* Appended params need their JSON defaults here too: the wrapper pushes them at insert
     * on the device, but the offline harness (and any host that only creates the instance)
     * never does — `motion` off and `revmode` 201 were silent default mismatches. */
    values[P_SATURATION].store(0.0f);
    values[P_TAPEAGE].store(1.0f);    /* Used */
    values[P_WETONLY].store(0.0f);    /* Off */
    values[P_REVMODE].store(1.0f);    /* 202 */
    values[P_WARPTYPE].store(0.0f);   /* Normal */
    values[P_MOTION].store(1.0f);     /* On: the meter moves unless told otherwise */
    values[P_WARP].store(0.0f);
    values[P_TWIST].store(0.0f);
    values[P_TWISTTYPE].store(0.0f);  /* Normal */
    values[P_SYNC].store(0.0f);       /* Off */
    values[P_DRIFT].store(0.0f);
    bpm = 120.0f;
    warp_t = 0.0f;
    twist_t = 0.0f;
    twist_phase = 0.0f;
    drift_phase = 0.0f;
    viz_sumsq = 0.0f;
    viz_count = 0;
    viz_acc = 0;
    viz_wet_s = 0.0f;
    last_rate = 0.45f;
    base_rate = 0.45f;
    slot = -1;
  }
};

/* Contract ranges. Keep in step with src/airtape_module.json: a value clamped too
 * narrow silently caps a control (ui 0..2 and spread 0..5 ms were the first
 * casualties of a blanket 0..1 clamp). */
/* Derived from src/airtape_module.json (option count - 1, else min/max).
 * Hand-appending these used to wedge a new option param at the wrong index. */
const float kMin[P_COUNT] = {
                            0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, -1.0f,
                            0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                            0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                            0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
const float kMax[P_COUNT] = {
                            11.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                            1.0f, 2.0f, 1.0f, 1.0f, 1.0f, 5.0f, 1.0f, 0.0f,
                            1.0f, 2.0f, 1.0f, 1.0f, 1.0f, 1.0f, 2.0f, 3.0f,
                            1.0f, 1.0f, 2.0f, 1.0f, 2.0f, 300.0f, 2000.0f, 1.0f};

int find_key(const char *key) {
  for (int i = 0; i < P_COUNT; ++i) {
    if (!std::strcmp(kKeys[i], key)) return i;
  }
  return -1;
}

float parse_value(int idx, const char *val) {
  if (text_param(idx)) {
    const int n = text_count(idx);
    for (int i = 0; i < n; ++i) {
      if (!std::strcmp(text_at(idx, i), val)) return (float)i;
    }
  }
  return (float)std::atof(val);
}

void apply_params(airtape_t *inst) {
  auto v = [inst](int i) { return inst->values[i].load(std::memory_order_relaxed); };

  inst->dsp.setMode((int)(v(P_MODE) + 0.5f) + 1);
  inst->dsp.setIntensity(v(P_INTENSITY));
  inst->dsp.setEchoLevel(v(P_ECHO));
  inst->dsp.setReverbLevel(v(P_REVERB));
  inst->dsp.setMix(v(P_MIX));
  inst->dsp.setBass(v(P_BASS));
  inst->dsp.setTreble(v(P_TREBLE));
  inst->dsp.setWowFlutter(v(P_WOW));
  inst->dsp.setBypass(false);
  inst->dsp.setRepeatRate(v(P_RATE));

  /* Tape age quantised by the core: 0 = New, 0.5 = Used, 1 = Worn. */
  /* Reverb independent of the head mode: upstream only feeds the spring in the modes
   * that carry a reverb send, which made the REVERB knob dead in H1/H2/H3/H2+3. */
  inst->dsp.setReverbSend(reverb_send((int)(v(P_REVMODE) + 0.5f), v(P_REVERB)));
  inst->dsp.setInputGain(kLoopTrim);
  inst->dsp.setOutputVolume(kOutTrim);
  inst->dsp.setTapeAge(v(P_TAPEAGE) * 0.5f);
  /* WET ONLY (RE-202 "DIRECT ON/OFF"): kill the dry path for a send/return
   * insert; `mix` keeps its meaning, so the toggle is additive, not a remix. */
  inst->dsp.setDryLevel(v(P_WETONLY) > 0.5f ? 0.0f : 1.0f);

  pre_set_mode(&inst->pre, (pre_mode_t)(int)(v(P_PREAMP) + 0.5f));
  pre_set_drive(&inst->pre, v(P_SATURATION) * 12.0f);
  chorus_set_amount(&inst->chorus, v(P_CHORUS));
  imp_set_amounts(&inst->imp, v(P_DROPOUTS), v(P_HISS));
  spread_set_offset_ms(&inst->spread, v(P_SPREAD));
}

/* ---- API ----------------------------------------------------------------- */

void *create_instance(const char *module_dir, const char *) {
  void *mem = std::calloc(1, sizeof(airtape_t));
  if (!mem) return nullptr;
  airtape_t *inst = new (mem) airtape_t(nullptr);
  inst->slot = mhec::attach();   /* companion page; -1 when MHEC_HTTP=0 or no slot free */
  if (module_dir) {
    std::snprintf(inst->dir, sizeof inst->dir, "%s", module_dir);
    /* ui.txt on disk is the truth: adopt it, never overwrite the skin here —
     * writing the default back on every insert is what made the switch look
     * like it worked only once. */
    inst->ui_adopted = read_ui_choice(inst->dir);
    if (inst->ui_adopted < 0) {   /* stale or missing record: heal the root skin once */
      inst->ui_adopted = 0;
      ui_swap(inst->dir, inst->ui_adopted);
    }
    inst->values[P_UI].store((float)inst->ui_adopted, std::memory_order_relaxed);
  }
  inst->dsp.prepare((double)kFs, kMaxBlock);
  inst->dsp.reset();
  pre_init(&inst->pre, kFs, PRE_WARM);
  chorus_init(&inst->chorus, kFs);
  imp_init(&inst->imp, kFs, 0xC0FFEEu);
  spread_init(&inst->spread, kFs);
  apply_params(inst);
  return inst;
}

void destroy_instance(void *instance) {
  if (!instance) return;
  airtape_t *inst = static_cast<airtape_t *>(instance);
  mhec::detach(inst->slot);      /* before the memory goes: the page must not read a tombstone */
  inst->slot = -1;
  inst->~airtape_t();
  std::free(inst);
}

/* defined below (shared by set_param and the state restore) */
void set_one(airtape_t *inst, const char *key, const char *val);

void process_block(void *instance, int16_t *audio, int frames) {
  airtape_t *inst = static_cast<airtape_t *>(instance);
  if (!inst || frames <= 0) return;

  /* The companion page's knob moves arrive here, not on the HTTP thread: set_one() writes the
   * DSP's own (non-atomic) state, which the render loop owns. Cheap when the ring is empty. */
  if (inst->slot >= 0) {
    mhec::Command cmds[8];
    const int n = mhec::take_commands(inst->slot, cmds, 8);
    for (int i = 0; i < n; ++i) {
      if (cmds[i].idx < 0 || cmds[i].idx >= P_COUNT) continue;
      if (cmds[i].idx == P_UI) continue;   /* a remote control must not rewrite the skin folder */
      char v[32];
      std::snprintf(v, sizeof v, "%.6f", (double)cmds[i].val);
      set_one(inst, kKeys[cmds[i].idx], v);
    }
    if (n > 0) inst->display_rev.fetch_add(1, std::memory_order_relaxed);
  }

  for (int done = 0; done < frames;) {
    const int n = (frames - done < kMaxBlock) ? (frames - done) : kMaxBlock;
    const int16_t *in = audio + done * 2;

    for (int i = 0; i < n; ++i) {
      inst->scratch_l[i] = (float)in[i * 2] / 32768.0f;
      inst->scratch_r[i] = (float)in[i * 2 + 1] / 32768.0f;
    }

    pre_process(&inst->pre, inst->scratch_l, inst->scratch_r, inst->work_l, inst->work_r, n);
    chorus_process(&inst->chorus, inst->work_l, inst->work_r, inst->work_l, inst->work_r, n);
    imp_process(&inst->imp, inst->work_l, inst->work_r, inst->work_l, inst->work_r, n);
    spread_process(&inst->spread, inst->work_l, inst->work_r, inst->work_l, inst->work_r, n);

    /* --- one motor, four hands: sync, drift, twist, jog ---------------------
     * `sync` sets the base from the host tempo, `drift` lets it wander slowly,
     * `twist` oscillates it while the switch is held, `jog` drags it down and
     * wins over twist (both at once must never jump). Everything lands on the
     * core's repeat rate, whose own 350 ms capstan inertia turns any per-block
     * change into a tape-speed ramp instead of a step. Per block: a handful of
     * sinf() — no per-sample cost. */
    {
      const float step = (float)n / (float)kFs;
      auto val = [inst](int i) { return inst->values[i].load(std::memory_order_relaxed); };

      float rate = val(P_RATE);

      const int sync = (int)(val(P_SYNC) + 0.5f);
      if (sync > 0 && inst->bpm > 1.0f) {
        const double quarter = 60000.0 / (double)inst->bpm;
        const double div = (sync == 1) ? 8.0 : (sync == 2 ? 4.0 : 2.0);  /* 1/32, 1/16, 1/8 */
        rate = duskaudio::TapeEchoDSP::repeatRateForDelayMs((float)(quarter / div));
      }

      inst->base_rate = rate;   /* what the wow readout measures against */

      /* drift: two incommensurate slow sines, ±2 % of the motor range */
      const float drift = val(P_DRIFT);
      inst->drift_phase += step * 0.31f;
      if (inst->drift_phase > 1.0f) inst->drift_phase -= 1.0f;
      const float wob = 0.62f * std::sin(6.2831853f * inst->drift_phase) +
                        0.38f * std::sin(6.2831853f * inst->drift_phase * 0.37f + 1.3f);
      rate += wob * drift * 0.02f;

      /* twist: engage ramp (~80 ms) + 0.7 Hz oscillation around the base */
      const float twist = (val(P_TWIST) > 0.5f) ? 1.0f : 0.0f;
      inst->twist_t += (twist - inst->twist_t) * clamp_f(step * 12.0f, 0.0f, 1.0f);
      if (twist > 0.5f) {
        inst->twist_phase += step * 0.9f;
        if (inst->twist_phase > 1.0f) inst->twist_phase -= 1.0f;
      }
      const int tt = (int)(val(P_TWISTTYPE) + 0.5f);
      const float depth = (tt == 0) ? 0.08f : (tt == 1 ? 0.18f : 0.05f + 0.13f * val(P_INTENSITY));
      rate += inst->twist_t * depth * std::sin(6.2831853f * inst->twist_phase);

      /* jog drags the playhead: a momentary control, not a knob to return */
      const float target = inst->jog_target.load(std::memory_order_relaxed);
      inst->jog_cur += 0.25f * (target - inst->jog_cur);
      rate = clamp_f(rate - 0.06f * inst->jog_cur, 0.0f, 1.0f);

      /* WARP: the pedal's "hold for a dreamlike swell" — reverb and feedback climb
       * while held (0.5 s to full, so a normal hold gets there), fall back in ~250 ms. */
      const int wt = (int)(val(P_WARPTYPE) + 0.5f);
      const float w_rev = (wt == 1) ? 0.80f : (wt == 2 ? 0.35f : 0.55f);
      const float w_int = (wt == 1) ? 0.10f : (wt == 2 ? 0.35f : 0.25f);
      const float w_up  = (wt == 1) ? 0.83f : (wt == 2 ? 3.3f : 2.0f);   /* 1/seconds to full */
      const float w_dn  = (wt == 1) ? 2.0f : 4.0f;
      const float held = (val(P_WARP) > 0.5f) ? 1.0f : 0.0f;
      inst->warp_t += (held > 0.5f) ? step * w_up : -step * w_dn;
      inst->warp_t = clamp_f(inst->warp_t, 0.0f, 1.0f);
      if (wt == 2) rate = clamp_f(rate - 0.05f * inst->warp_t, 0.0f, 1.0f);  /* Wild drags the motor */

      inst->last_rate = rate;
      inst->dsp.setRepeatRate(rate);
      const float rev = clamp_f(val(P_REVERB) + w_rev * inst->warp_t, 0.0f, 1.0f);
      inst->dsp.setReverbSend(reverb_send((int)(val(P_REVMODE) + 0.5f), rev));  /* also under WARP */
      inst->dsp.setReverbLevel(rev);
      inst->dsp.setIntensity(clamp_f(val(P_INTENSITY) + w_int * inst->warp_t, 0.0f, 1.0f));
    }

    const float *ins[2] = {inst->work_l, inst->work_r};
    float *outs[2] = {inst->scratch_l, inst->scratch_r};
    inst->dsp.processBlock(ins, outs, 2, n);

    for (int i = 0; i < n; ++i) {
      float l = out_soft(inst->scratch_l[i]) * 32767.0f;
      float r = out_soft(inst->scratch_r[i]) * 32767.0f;
      /* the knee keeps both inside the range; the clamp stays as a last resort */
      if (l > 32767.0f) l = 32767.0f;
      if (l < -32768.0f) l = -32768.0f;
      if (r > 32767.0f) r = 32767.0f;
      if (r < -32768.0f) r = -32768.0f;
      audio[(done + i) * 2] = (int16_t)l;
      audio[(done + i) * 2 + 1] = (int16_t)r;
      inst->viz_sumsq += inst->scratch_l[i] * inst->scratch_l[i];
    }
    done += n;
    inst->viz_count += n;
  }

  /* --- visualisation: five values the host draws, updated ~46 ms apart ---------
   * A meter is the only animation this platform can do without a redraw storm, so
   * the plugin publishes state and MPC draws it. `motion` freezes the bar. */
  inst->viz_acc += frames;
  if (inst->viz_acc >= 2048) {
    inst->viz_acc = 0;
    const float rms = std::sqrt(inst->viz_sumsq /
                                (float)(inst->viz_count > 0 ? inst->viz_count : 1));
    inst->viz_sumsq = 0.0f;
    inst->viz_count = 0;

    const bool motion_on =
        inst->values[P_MOTION].load(std::memory_order_relaxed) > 0.5f;
    const float db = 20.0f * std::log10(rms + 1e-6f);
    const float target = motion_on ? clamp_f((db + 48.0f) / 48.0f, 0.0f, 1.0f) : 0.0f;
    inst->viz_wet_s += 0.35f * (target - inst->viz_wet_s);
    inst->values[P_VIZ_WET].store(inst->viz_wet_s, std::memory_order_relaxed);

    inst->values[P_DELAY_MS].store(
        duskaudio::TapeEchoDSP::delayMsForRepeatRate(inst->last_rate),
        std::memory_order_relaxed);
    inst->values[P_TEMPO_BPM].store(inst->bpm, std::memory_order_relaxed);

    /* Feedback danger: the core self-oscillates above ~0.75, and a low shelf inside
     * the loop pushes it there earlier, so the bass boost counts. */
    const float inten = clamp_f(
        inst->values[P_INTENSITY].load(std::memory_order_relaxed) +
            0.15f * std::max(0.0f, inst->values[P_BASS].load(std::memory_order_relaxed)),
        0.0f, 1.0f);
    const float fb = inten < 0.60f ? 0.0f : (inten < 0.78f ? 1.0f : 2.0f);
    inst->values[P_VIZ_FB].store(fb, std::memory_order_relaxed);

    /* Companion page: the same numbers the host draws, plus what the picture needs
     * (head activity straight from the core's mode table). Stores into static slots —
     * no allocation, no lock, no instance pointer leaves this call. */
    if (inst->slot >= 0) {
      mhec::Snapshot snap;
      std::memset(&snap, 0, sizeof snap);
      snap.wet = inst->viz_wet_s;
      snap.delay_ms = inst->values[P_DELAY_MS].load(std::memory_order_relaxed);
      snap.bpm = inst->bpm;
      snap.rate = inst->last_rate;
      snap.wow_pct = (inst->base_rate > 0.01f)
                         ? 100.0f * (inst->last_rate - inst->base_rate) / inst->base_rate
                         : 0.0f;
      snap.jog = inst->jog_cur;
      snap.warp = inst->warp_t;
      snap.twist = inst->twist_t;
      snap.echo = inst->values[P_ECHO].load(std::memory_order_relaxed);
      snap.wow = inst->values[P_WOW].load(std::memory_order_relaxed);
      snap.chorus = inst->values[P_CHORUS].load(std::memory_order_relaxed);
      snap.saturation = inst->values[P_SATURATION].load(std::memory_order_relaxed);
      snap.dropouts = inst->values[P_DROPOUTS].load(std::memory_order_relaxed);
      snap.hiss = inst->values[P_HISS].load(std::memory_order_relaxed);
      snap.spread = inst->values[P_SPREAD].load(std::memory_order_relaxed);
      snap.mix = inst->values[P_MIX].load(std::memory_order_relaxed);
      snap.intensity = inst->values[P_INTENSITY].load(std::memory_order_relaxed);
      snap.reverb = inst->values[P_REVERB].load(std::memory_order_relaxed);
      snap.mode = (int)(inst->values[P_MODE].load(std::memory_order_relaxed) + 0.5f);
      snap.fb = (int)fb;
      snap.sync = (int)(inst->values[P_SYNC].load(std::memory_order_relaxed) + 0.5f);
      snap.drift = inst->values[P_DRIFT].load(std::memory_order_relaxed) > 0.5f ? 1 : 0;
      snap.tapeage = (int)(inst->values[P_TAPEAGE].load(std::memory_order_relaxed) + 0.5f);
      snap.preamp = (int)(inst->values[P_PREAMP].load(std::memory_order_relaxed) + 0.5f);
      snap.wetonly = inst->values[P_WETONLY].load(std::memory_order_relaxed) > 0.5f ? 1 : 0;
      snap.motion = inst->values[P_MOTION].load(std::memory_order_relaxed) > 0.5f ? 1 : 0;
      snap.twisttype = (int)(inst->values[P_TWISTTYPE].load(std::memory_order_relaxed) + 0.5f);
      snap.warptype = (int)(inst->values[P_WARPTYPE].load(std::memory_order_relaxed) + 0.5f);
      snap.revmode = (int)(inst->values[P_REVMODE].load(std::memory_order_relaxed) + 0.5f);
      float gains[3] = {0.0f, 0.0f, 0.0f};
      bool sends = false;
      duskaudio::TapeEchoDSP::modeTapsForMode(snap.mode + 1, gains, &sends);
      snap.heads[0] = gains[0] > 0.0f ? 1 : 0;
      snap.heads[1] = gains[1] > 0.0f ? 1 : 0;
      snap.heads[2] = gains[2] > 0.0f ? 1 : 0;
      snap.reverb_head = sends ? 1 : 0;
      mhec::publish(inst->slot, snap);
    }

    inst->display_rev.fetch_add(1, std::memory_order_relaxed);
  }
}

void set_param(void *instance, const char *key, const char *val);

/* One key -> one value. Shared by set_param and the "state" restore so a preset
 * and a host automation event cannot drift apart. */
void set_one(airtape_t *inst, const char *key, const char *val) {
  const int idx = find_key(key);
  if (idx < 0) return;

  float v = parse_value(idx, val);
  v = clamp_f(v, kMin[idx], kMax[idx]);

  inst->values[idx].store(v, std::memory_order_relaxed);

  if (idx == P_JOG) {
    inst->jog_target.store(v > 0.5f ? 1.0f : 0.0f, std::memory_order_relaxed);
    return;
  }
  if (idx == P_MODE_NEXT) {
    /* Next-mode footswitch, owned here rather than by the wrapper's step_of: MPC sends the
     * press value twice per tap, and step_of fired once per event (two modes per tap,
     * measured on the device 2026-10-30). Edge detection makes one tap one step, and the
     * core's mode is set in the same call. The host learns about it through display_rev. */
    if (v > 0.5f) {
      if (!inst->mode_next_held) {
        inst->mode_next_held = 1;
        int m = (int)(inst->values[P_MODE].load(std::memory_order_relaxed) + 0.5f) + 1;
        if (m >= 12) m = 0;
        inst->values[P_MODE].store((float)m, std::memory_order_relaxed);
        apply_params(inst);
        inst->display_rev.fetch_add(1, std::memory_order_relaxed);
      }
    } else {
      inst->mode_next_held = 0;
    }
    return;
  }
  if (idx == P_UI) {
    const int choice = (int)(v + 0.5f);
    /* The host re-applies every parameter when it restores a project; that must
     * not undo the interface the user picked (ui.txt on disk wins). Only a real
     * change of the control writes the skin. */
    if (choice == inst->ui_adopted) return;
    inst->ui_adopted = choice;
    ui_swap(inst->dir, choice);
    return; /* skin only: nothing for the DSP to do */
  }
  apply_params(inst);
}

/* A whole state: "key=value" lines, the format get_param("state") writes and the
 * wrapper stores in a VST2 chunk (so an MPC effect preset and a project restore
 * carry every parameter). "ui" is written but NOT applied: a preset must not
 * re-copy the skin folder of a running instance. "jog" is momentary and never
 * on. A line without '=' is ignored, so an unknown extra field is harmless. */
void restore_state(airtape_t *inst, const char *text) {
  const char *p = text;
  while (p && *p) {
    const char *nl = std::strchr(p, '\n');
    const int len = nl ? (int)(nl - p) : (int)std::strlen(p);
    const char *eq = static_cast<const char *>(std::memchr(p, '=', (size_t)len));
    if (eq) {
      const int kl = (int)(eq - p);
      if (kl > 0 && kl < 64) {
        char key[64];
        std::memcpy(key, p, (size_t)kl);
        key[kl] = 0;
        int vl = len - kl - 1;
        if (vl < 0) vl = 0;
        if (vl > 31) vl = 31;
        char val[32];
        std::memcpy(val, eq + 1, (size_t)vl);
        val[vl] = 0;
        /* Momentary keys are never restored: a preset saved with a switch held
         * would otherwise stick it on. */
        if (std::strcmp(key, "ui") != 0 && std::strcmp(key, "jog") != 0 &&
            std::strcmp(key, "mode_next") != 0 && std::strcmp(key, "warp") != 0 &&
            std::strcmp(key, "twist") != 0) set_one(inst, key, val);
      }
    }
    if (!nl) break;
    p = nl + 1;
  }
  apply_params(inst);
}

void set_param(void *instance, const char *key, const char *val) {
  airtape_t *inst = static_cast<airtape_t *>(instance);
  if (!inst || !key || !val) return;

  if (!std::strcmp(key, "state")) {
    restore_state(inst, val);
    return;
  }
  if (!std::strcmp(key, "lfo_bpm")) {   /* HAS_LFO_BPM: host tempo for `sync` */
    const float b = (float)std::atof(val);
    if (b > 1.0f && b < 1000.0f) inst->bpm = b;
    return;
  }
  set_one(inst, key, val);
}

int get_param(void *instance, const char *key, char *buf, int buf_len) {
  airtape_t *inst = static_cast<airtape_t *>(instance);
  if (!inst || !key || !buf || buf_len <= 0) return -1;

  if (!std::strcmp(key, "tempo_bpm") || !std::strcmp(key, "delay_ms")) {
    /* engine-formatted readouts: no decimals, no unit (the layout prints the unit) */
    const int idx2 = find_key(key);
    return std::snprintf(buf, (size_t)buf_len, "%.0f",
                         (double)inst->values[idx2].load(std::memory_order_relaxed));
  }
  if (!std::strcmp(key, "display_rev")) {
    return std::snprintf(buf, (size_t)buf_len, "%d",
                         inst->display_rev.load(std::memory_order_relaxed));
  }
  if (!std::strcmp(key, "state")) {
    int p = 0;
    for (int i = 0; i < P_COUNT; ++i) {
      char one[64];
      const float v = inst->values[i].load(std::memory_order_relaxed);
      int n;
      if (text_param(i)) {
        const int k = (int)(v + 0.5f);
        const int lim = text_count(i);
        n = std::snprintf(one, sizeof one, "%s=%s\n", kKeys[i],
                          text_at(i, k < 0 ? 0 : (k >= lim ? lim - 1 : k)));
      } else {
        n = std::snprintf(one, sizeof one, "%s=%.4f\n", kKeys[i], (double)v);
      }
      if (n < 0 || p + n >= buf_len) break;
      std::memcpy(buf + p, one, (size_t)n);
      p += n;
    }
    if (p < buf_len) buf[p] = 0;
    return p;
  }

  const int idx = find_key(key);
  if (idx < 0) return -1;

  const float v = inst->values[idx].load(std::memory_order_relaxed);
  int n;
  if (text_param(idx)) {
    const int i = (int)(v + 0.5f);
    const int lim = text_count(idx);
    n = std::snprintf(buf, (size_t)buf_len, "%s", text_at(idx, i < 0 ? 0 : (i >= lim ? lim - 1 : i)));
  } else {
    n = std::snprintf(buf, (size_t)buf_len, "%.4f", (double)v);
  }
  return n;
}

void on_midi(void *, const uint8_t *, int, int) {}

}  // namespace

extern "C" audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *) {
  static audio_fx_api_v2_t api;
  mhec::register_param_keys(kKeys, P_COUNT);   /* the companion server validates writes against this */
  api.api_version = AUDIO_FX_API_VERSION_2;
  api.create_instance = create_instance;
  api.destroy_instance = destroy_instance;
  api.process_block = process_block;
  api.set_param = set_param;
  api.get_param = get_param;
  api.on_midi = on_midi;
  return &api;
}
