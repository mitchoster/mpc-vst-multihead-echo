/* spread.c — stereo offset (T7).
 *
 * Right channel runs through a delay line of 0..6 ms; the target is smoothed
 * with a ~25 ms one-pole so a knob move glides instead of stepping. Offset 0 is
 * a bit-exact bypass (the delay line is not even read).
 *
 * Mono compatibility: a few milliseconds of delay decorrelates the channels but
 * cannot cancel them (cancellation needs sub-millisecond alignment), so the mono
 * sum stays at or above the level of a single channel. That is what the test
 * asserts, instead of the meaningless "mono sum equals stereo" check.
 */

#include "spread.h"

#include <math.h>

static float spread_tau(float tau_s, float fs) {
  return (float)exp(-1.0 / ((double)tau_s * (double)fs));
}

void spread_init(spread_state_t *s, float sample_rate) {
  s->sample_rate = (sample_rate > 0.0f) ? sample_rate : 44100.0f;
  s->offset_ms = 0.0f;
  spread_reset(s);
}

void spread_reset(spread_state_t *s) {
  int i;
  for (i = 0; i < SPREAD_BUF; ++i) {
    s->buf_r[i] = 0.0f;
  }
  s->write_pos = 0;
  s->current = 0.0f;
}

void spread_set_offset_ms(spread_state_t *s, float ms) {
  if (ms < 0.0f) ms = 0.0f;
  if (ms > SPREAD_MAX_MS) ms = SPREAD_MAX_MS;
  s->offset_ms = ms;
}

float spread_get_offset_ms(const spread_state_t *s) {
  return s->offset_ms;
}

void spread_process(spread_state_t *s, const float *in_l, const float *in_r,
                    float *out_l, float *out_r, int frames) {
  const int mask = SPREAD_BUF - 1;
  const float target_samples = s->offset_ms * s->sample_rate * 0.001f;
  const float smooth = spread_tau(0.025f, s->sample_rate);
  int i;

  if (s->offset_ms <= 0.0f && s->current < 1e-3f) {
    s->current = 0.0f;
    for (i = 0; i < frames; ++i) {
      out_l[i] = in_l[i];
      out_r[i] = in_r[i];
    }
    return;
  }

  for (i = 0; i < frames; ++i) {
    float d, i0, frac, wet;
    int a, b;

    s->buf_r[s->write_pos] = in_r[i];
    s->current = smooth * s->current + (1.0f - smooth) * target_samples;
    d = s->current;
    if (d < 0.0f) d = 0.0f;

    i0 = (float)(int)d;
    frac = d - i0;
    a = (s->write_pos - (int)i0) & mask;
    b = (s->write_pos - (int)i0 - 1) & mask;
    wet = s->buf_r[a] * (1.0f - frac) + s->buf_r[b] * frac;

    out_l[i] = in_l[i];
    out_r[i] = wet;
    s->write_pos = (s->write_pos + 1) & mask;
  }
}
