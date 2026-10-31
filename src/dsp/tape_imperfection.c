/* tape_imperfection.c — dropouts and hiss (T6).
 *
 * Dropouts: a gap timer schedules the next event (mean gap 0.9 s / amount, so
 * "more tape wear" means more frequent, never a machine gun). Depth and length
 * are drawn from the deterministic LCG: 30..90 % deep, 25..90 ms long. The gain
 * falls with a 5 ms attack and recovers with a 40 ms release, so no clicks.
 *
 * Hiss: white noise -> highpass 80 Hz -> lowpass 6 kHz, scaled to about
 * -34 dBFS at amount = 1 (audible on headphones, invisible in a mix).
 *
 * Both parts bypass bit-exactly at amount 0.
 */

#include "tape_imperfection.h"

#include <math.h>

static const float IMP_PI = 3.14159265358979323846f;

static float imp_pole(float fc, float fs) {
  return (float)exp(-2.0 * (double)IMP_PI * (double)fc / (double)fs);
}

/* One-pole coefficient for a time constant in seconds (gain 63 % after tau). */
static float imp_tau(float tau_s, float fs) {
  return (float)exp(-1.0 / ((double)tau_s * (double)fs));
}

/* Uniform 0..1 from the LCG (deterministic across runs and platforms). */
static float imp_rand(imperfection_state_t *s) {
  s->rng = s->rng * 1664525u + 1013904223u;
  return (float)((s->rng >> 8) & 0xFFFFu) / 65535.0f;
}

static void imp_schedule_gap(imperfection_state_t *s) {
  const float mean_s = 0.9f / (s->dropout_amount > 0.05f ? s->dropout_amount : 0.05f);
  /* 0.4..1.6 x the mean keeps it irregular but not clustered */
  const float s_len = mean_s * (0.4f + 1.2f * imp_rand(s));
  s->gap_left = (long)(s_len * s->sample_rate);
  if (s->gap_left < 1) {
    s->gap_left = 1;
  }
}

void imp_init(imperfection_state_t *s, float sample_rate, unsigned seed) {
  s->sample_rate = (sample_rate > 0.0f) ? sample_rate : 44100.0f;
  s->hp_a = imp_pole(80.0f, s->sample_rate);
  s->lp_a = imp_pole(6000.0f, s->sample_rate);
  s->attack = imp_tau(0.005f, s->sample_rate);  /* 5 ms */
  s->release = imp_tau(0.040f, s->sample_rate); /* 40 ms */
  imp_reset(s);
  s->rng = seed ? seed : 0x12345678u; /* after reset, which seeds the default */
}

void imp_reset(imperfection_state_t *s) {
  s->dropout_amount = 0.0f;
  s->hiss_amount = 0.0f;
  s->gain = 1.0f;
  s->target = 1.0f;
  s->hold_left = 0;
  s->gap_left = 0;
  s->triggered = 0;
  s->hp_x1 = s->hp_y1 = 0.0f;
  s->lp_y1 = 0.0f;
  s->rng = 0x12345678u;
}

void imp_set_amounts(imperfection_state_t *s, float dropout01, float hiss01) {
  if (dropout01 < 0.0f) dropout01 = 0.0f;
  if (dropout01 > 1.0f) dropout01 = 1.0f;
  if (hiss01 < 0.0f) hiss01 = 0.0f;
  if (hiss01 > 1.0f) hiss01 = 1.0f;
  if (dropout01 > 0.0f && s->dropout_amount == 0.0f) {
    s->gap_left = 0; /* first dropout soon after enabling */
  }
  s->dropout_amount = dropout01;
  s->hiss_amount = hiss01;
}

void imp_process(imperfection_state_t *s, const float *in_l, const float *in_r,
                 float *out_l, float *out_r, int frames) {
  int i;

  if (s->dropout_amount <= 0.0f && s->hiss_amount <= 0.0f) {
    for (i = 0; i < frames; ++i) {
      out_l[i] = in_l[i];
      out_r[i] = in_r[i];
    }
    return;
  }

  for (i = 0; i < frames; ++i) {
    float g = 1.0f;
    float l = in_l[i];
    float r = in_r[i];

    /* ---- dropouts ------------------------------------------------------ */
    if (s->dropout_amount > 0.0f) {
      if (s->hold_left > 0) {
        if (--s->hold_left == 0) {
          s->target = 1.0f; /* start recovering */
          imp_schedule_gap(s);
        }
      } else if (--s->gap_left <= 0) {
        const float depth = 0.3f + 0.6f * imp_rand(s);
        const float ms = 25.0f + 65.0f * imp_rand(s);
        s->target = 1.0f - depth;
        s->hold_left = (long)(ms * 0.001f * s->sample_rate);
        if (s->hold_left < 1) {
          s->hold_left = 1;
        }
        ++s->triggered;
      }
      /* one-pole toward the target: fast down, slow up */
      {
        const float a = (s->target < s->gain) ? s->attack : s->release;
        s->gain = a * s->gain + (1.0f - a) * s->target;
        if (s->gain < 1e-6f) {
          s->gain = 0.0f;
        }
      }
      g = s->gain;
    }

    /* ---- hiss ---------------------------------------------------------- */
    if (s->hiss_amount > 0.0f) {
      const float w = imp_rand(s) * 2.0f - 1.0f;
      const float hp = 0.5f * (1.0f + s->hp_a) * (w - s->hp_x1) + s->hp_a * s->hp_y1;
      const float lp = (1.0f - s->lp_a) * hp + s->lp_a * s->lp_y1;
      const float n = lp * 0.02f * s->hiss_amount;
      s->hp_x1 = w;
      s->hp_y1 = hp;
      s->lp_y1 = lp;
      l += n;
      r += n * 0.9f; /* slightly decorrelated, still mono-compatible */
    }

    out_l[i] = l * g;
    out_r[i] = r * g;
  }
}
