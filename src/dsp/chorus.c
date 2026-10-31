/* chorus.c — one-knob chorus (T5).
 *
 * y = dry + amount * delayed(dry), with the delay time modulated by a sine LFO:
 *   left  : base_ms + depth * sin(phase)
 *   right : base_ms + depth * sin(phase + pi/2)   -> 90 degrees apart (width)
 *
 * amount = 0 is a bit-exact bypass (no modulation is computed at all), which is
 * what the "chorus off" regression test measures.
 *
 * Depth and rate are fixed for now; they become parameters in T8 when the
 * contract is frozen. Fractional delay is linear-interpolated.
 */

#include "chorus.h"

#include <math.h>

static const float CH_PI = 3.14159265358979323846f;

void chorus_init(chorus_state_t *c, float sample_rate) {
  c->sample_rate = (sample_rate > 0.0f) ? sample_rate : 44100.0f;
  c->amount = 0.0f;
  c->lfo_phase = 0.0f;
  c->lfo_inc = 2.0f * CH_PI * 0.35f / c->sample_rate; /* 0.35 Hz */
  c->base_ms = 14.0f;
  c->depth_ms = 6.0f;
  chorus_reset(c);
}

void chorus_reset(chorus_state_t *c) {
  int i;
  for (i = 0; i < CHORUS_BUF; ++i) {
    c->buf_l[i] = 0.0f;
    c->buf_r[i] = 0.0f;
  }
  c->write_pos = 0;
  c->lfo_phase = 0.0f;
}

void chorus_set_amount(chorus_state_t *c, float amount01) {
  if (amount01 < 0.0f) amount01 = 0.0f;
  if (amount01 > 1.0f) amount01 = 1.0f;
  c->amount = amount01;
}

float chorus_get_amount(const chorus_state_t *c) {
  return c->amount;
}

/* Reads the buffer at a fractional delay in samples (linear interpolation). */
static float chorus_tap(const float *buf, int write_pos, float delay) {
  const int mask = CHORUS_BUF - 1;
  const int i0 = (int)delay;
  const float frac = delay - (float)i0;
  const int a = (write_pos - i0) & mask;
  const int b = (write_pos - i0 - 1) & mask;
  return buf[a] * (1.0f - frac) + buf[b] * frac;
}

void chorus_process(chorus_state_t *c, const float *in_l, const float *in_r,
                    float *out_l, float *out_r, int frames) {
  const float ms_to_samples = c->sample_rate * 0.001f;
  const float base = c->base_ms * ms_to_samples;
  int i;

  if (c->amount <= 0.0f) { /* bit-exact bypass */
    for (i = 0; i < frames; ++i) {
      out_l[i] = in_l[i];
      out_r[i] = in_r[i];
    }
    return;
  }

  for (i = 0; i < frames; ++i) {
    const float depth = c->depth_ms * c->amount * ms_to_samples;
    const float dl = base + depth * sinf(c->lfo_phase);
    const float dr = base + depth * sinf(c->lfo_phase + 0.5f * CH_PI);
    float wet_l, wet_r;

    c->buf_l[c->write_pos] = in_l[i];
    c->buf_r[c->write_pos] = in_r[i];

    wet_l = chorus_tap(c->buf_l, c->write_pos, dl);
    wet_r = chorus_tap(c->buf_r, c->write_pos, dr);

    out_l[i] = in_l[i] + c->amount * wet_l;
    out_r[i] = in_r[i] + c->amount * wet_r;

    c->write_pos = (c->write_pos + 1) & (CHORUS_BUF - 1);
    c->lfo_phase += c->lfo_inc;
    if (c->lfo_phase > 2.0f * CH_PI) {
      c->lfo_phase -= 2.0f * CH_PI;
    }
  }
}
