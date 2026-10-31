/* preamp.c — Preamp stage implementation (T4a/T4c).
 *
 * Per channel:
 *   clean : nothing (bit-exact bypass)
 *   warm  : drive -> tanh with 3 % asymmetry -> low shelf +1.5 dB @ 100 Hz
 *           -> 1st-order lowpass 12 kHz -> output compensation -> DC blocker
 *   dirty : drive -> tanh with 8 % asymmetry -> 1st-order highpass 60 Hz
 *           -> 1st-order lowpass 8 kHz -> output compensation -> DC blocker
 *
 * The saturator is the only nonlinear part, so it is the only part that runs
 * oversampled: 2x halfband interpolation -> first-order ADAA around tanh ->
 * 2x halfband decimation. Everything else stays at the host rate, which keeps
 * the cost of the anti-aliasing at roughly the cost of the saturator itself.
 *
 * The output compensation normalises the small-signal gain of the saturator
 * (drive * sech^2(bias)), so a -12 dBFS sine stays within the test corridor
 * instead of being level-shifted by the mode.
 */

#include "preamp.h"

#include <math.h>

static const float PRE_PI = 3.14159265358979323846f;

/* ---- 2x halfband, ported from the vendored DuskOversampler (kA taps) ------ */

/* stage B: 15-tap halfband, transition 0.26, stopband -75 dB (scipy remez,
 * verbatim from vendor/ported/shared-dpf/dsp/DuskOversampler.hpp hbtaps::kB). */
static const float PRE_HB_A[PRE_HB_SIDE] = {
    0.3048934958f, -0.0712879483f, 0.0197218961f, -0.0034083969f,
};

static const int PRE_HB_C = 7; /* L/2 for a 15-tap halfband */

static void pre_hb_reset(pre_hb_t *h) {
  int i;
  for (i = 0; i < PRE_HB_RING; ++i) {
    h->buf[i] = 0.0f;
  }
  h->pos = 0;
}

static void pre_hb_push(pre_hb_t *h, float v) {
  h->pos = (h->pos + 1) & (PRE_HB_RING - 1);
  h->buf[h->pos] = v;
}

static float pre_hb_out(const pre_hb_t *h) {
  const int mask = PRE_HB_RING - 1;
  float acc = 0.5f * h->buf[(h->pos - PRE_HB_C) & mask];
  int i;
  for (i = 0; i < PRE_HB_SIDE; ++i) {
    const int k = 2 * i + 1;
    acc += PRE_HB_A[i] * (h->buf[(h->pos - (PRE_HB_C - k)) & mask] +
                          h->buf[(h->pos - (PRE_HB_C + k)) & mask]);
  }
  return acc;
}

/* ---- helpers ------------------------------------------------------------ */

static float pre_pole(float fc, float fs) {
  return (float)exp(-2.0 * (double)PRE_PI * (double)fc / (double)fs);
}

static float pre_db_to_lin(float db) {
  return (float)pow(10.0, (double)db / 20.0);
}

static float pre_denorm(float v) {
  return (fabsf(v) < 1e-20f) ? 0.0f : v;
}

/* Antiderivative of tanh: ln(cosh(x)), computed without overflow. */
static float pre_lncosh(float x) {
  const float ax = fabsf(x);
  return ax + log1pf(expf(-2.0f * ax)) - 0.6931472f;
}

static void pre_apply_drive(pre_state_t *s) {
  const float db = s->mode_db + s->sat_db;
  s->drive = pre_db_to_lin(db);
  if (s->mode == PRE_CLEAN && s->sat_db <= 0.0f) {
    s->drive = 1.0f;
    s->out_gain = 1.0f;
    return; /* keeps the bit-exact reference path */
  }
  {
    const float t = tanhf(s->bias);
    s->out_gain = 1.0f / (s->drive * (1.0f - t * t));
  }
}

static void pre_setup(pre_state_t *s, pre_mode_t mode) {
  const float fs = s->sample_rate;

  s->mode = mode;
  s->mode_db = 0.0f;
  s->drive = 1.0f;
  s->bias = 0.0f;
  s->out_gain = 1.0f;
  s->lp_b = 1.0f; s->lp_a = 0.0f;
  s->hp_b = 1.0f; s->hp_a = 0.0f;
  s->sh_g = 1.0f; s->sh_b = 0.0f; s->sh_a = 0.0f;

  if (mode == PRE_WARM) {
    s->mode_db = 3.0f;
    s->bias = 0.03f;
    s->lp_a = pre_pole(12000.0f, fs);
    s->lp_b = 1.0f - s->lp_a;
    s->sh_g = pre_db_to_lin(1.5f);
    s->sh_a = pre_pole(100.0f, fs);
    s->sh_b = 1.0f - s->sh_a;
  } else if (mode == PRE_DIRTY) {
    s->mode_db = 9.0f;
    s->bias = 0.08f;
    s->lp_a = pre_pole(8000.0f, fs);
    s->lp_b = 1.0f - s->lp_a;
    s->hp_a = pre_pole(60.0f, fs);
    s->hp_b = 1.0f - s->hp_a;
  }

  pre_apply_drive(s);
}

void pre_init(pre_state_t *s, float sample_rate, pre_mode_t mode) {
  s->sample_rate = (sample_rate > 0.0f) ? sample_rate : 44100.0f;
  s->sat_db = 0.0f;
  pre_reset(s);
  pre_setup(s, mode);
}

void pre_set_mode(pre_state_t *s, pre_mode_t mode) {
  pre_setup(s, mode); /* keeps the current SATURATION setting */
}

void pre_set_drive(pre_state_t *s, float db) {
  if (!(db >= 0.0f)) db = 0.0f;   /* also catches NaN */
  if (db > 12.0f) db = 12.0f;
  s->sat_db = db;
  pre_apply_drive(s);
}

void pre_reset(pre_state_t *s) {
  int c;
  for (c = 0; c < 2; ++c) {
    int k;
    s->lp_x1[c] = 0.0f; s->lp_y1[c] = 0.0f;
    s->hp_x1[c] = 0.0f; s->hp_y1[c] = 0.0f;
    s->sh_z[c] = 0.0f;
    s->dc_x1[c] = 0.0f; s->dc_y1[c] = 0.0f;
    s->ad_x1[c] = 0.0f; s->ad_f1[c] = 0.0f;
    (void)k;
    pre_hb_reset(&s->up_hb[c]);
    pre_hb_reset(&s->dn_hb[c]);
  }
}

/* Saturation with first-order ADAA, applied at the oversampled rate. */
static float pre_saturate(pre_state_t *s, int c, float x) {
  const float xd = s->drive * x + s->bias;
  const float f = pre_lncosh(xd);
  const float dx = xd - s->ad_x1[c];
  float y;
  /* The antiderivative difference divided by `dx` is the whole point of ADAA, and it is
   * only stable while `dx` is large compared with the float resolution of f (~1e-7).
   * A 1e-6 threshold was 100x too small: on a decaying tail (dx ~ 1e-6) the division
   * amplified round-off into audible crackle — measured 2026-10-30: 411 jumps >0.01 in
   * the last 0.5 s of a -80 dB decay, 47x the signal itself, and only while the
   * saturator ran (clean bypass and high saturation were fine). 1e-3 keeps the worst
   * case at ~1e-4, where the midpoint fallback below is the more accurate answer. */
  if (fabsf(dx) > 1e-3f) {
    y = (f - s->ad_f1[c]) / dx;
  } else {
    y = tanhf(0.5f * (xd + s->ad_x1[c]));
  }
  s->ad_x1[c] = xd;
  s->ad_f1[c] = pre_denorm(f);
  return y - tanhf(s->bias);
}

static float pre_channel(pre_state_t *s, int c, float x) {
  float y;

  if (s->mode == PRE_CLEAN && s->sat_db <= 0.0f) {
    return x; /* bit-exact reference path */
  }

  /* 1) 2x interpolate, 2) saturate at 2x, 3) decimate back to host rate.
   * Same call order as DuskOversampler::processSample: push x, take a0, push 0,
   * take a1; the x2 restores unity gain after zero stuffing. */
  {
    float v0, v1;
    pre_hb_push(&s->up_hb[c], x);
    v0 = 2.0f * pre_hb_out(&s->up_hb[c]);
    pre_hb_push(&s->up_hb[c], 0.0f);
    v1 = 2.0f * pre_hb_out(&s->up_hb[c]);
    v0 = pre_saturate(s, c, v0);
    v1 = pre_saturate(s, c, v1);

    pre_hb_push(&s->dn_hb[c], v0);
    pre_hb_push(&s->dn_hb[c], v1);
    y = pre_hb_out(&s->dn_hb[c]);
  }

  if (s->sh_g != 1.0f) { /* low shelf: boost only the low-passed part */
    s->sh_z[c] = s->sh_b * y + s->sh_a * s->sh_z[c];
    s->sh_z[c] = pre_denorm(s->sh_z[c]);
    y += (s->sh_g - 1.0f) * s->sh_z[c];
  }

  if (s->hp_a > 0.0f) { /* 1st-order highpass */
    const float hp = (1.0f + s->hp_a) * 0.5f * (y - s->hp_x1[c]) +
                     s->hp_a * s->hp_y1[c];
    s->hp_x1[c] = y;
    s->hp_y1[c] = pre_denorm(hp);
    y = hp;
  }

  if (s->lp_a > 0.0f) { /* 1st-order lowpass */
    const float lp = s->lp_b * y + s->lp_a * s->lp_y1[c];
    s->lp_y1[c] = pre_denorm(lp);
    y = lp;
  }

  y *= s->out_gain;

  /* DC blocker (~15 Hz) after the nonlinearity and the shelves */
  {
    const float a = pre_pole(15.0f, s->sample_rate);
    const float dc = y - s->dc_x1[c] + a * s->dc_y1[c];
    s->dc_x1[c] = y;
    s->dc_y1[c] = pre_denorm(dc);
    y = dc;
  }

  return y;
}

void pre_process(pre_state_t *s, const float *in_l, const float *in_r,
                 float *out_l, float *out_r, int frames) {
  int i;
  for (i = 0; i < frames; ++i) {
    out_l[i] = pre_channel(s, 0, in_l[i]);
    out_r[i] = pre_channel(s, 1, in_r[i]);
  }
}
