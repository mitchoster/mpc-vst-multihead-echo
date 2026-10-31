/* chorus.h — modulated-delay chorus for the MPC Multihead Echo prototype (T5).
 *
 * Sits after the preamp and before the tape core. One knob (0..1). At 0 the
 * module is a bit-exact bypass, so the "chorus off" path costs nothing and
 * measures as identical.
 *
 * C99, real-time safe (no malloc/lock in the render path), in-place allowed.
 */

#ifndef CHORUS_H
#define CHORUS_H

#ifdef __cplusplus
extern "C" {
#endif

#define CHORUS_MAX_MS 40.0f
#define CHORUS_BUF 4096 /* power of two, > 40 ms at 44.1 kHz */

typedef struct {
  float sample_rate;
  float amount;      /* 0..1 */
  float lfo_phase;   /* radians, left channel */
  float lfo_inc;     /* radians per sample */
  float base_ms;     /* centre delay */
  float depth_ms;    /* peak deviation at amount = 1 */
  int write_pos;
  float buf_l[CHORUS_BUF];
  float buf_r[CHORUS_BUF];
} chorus_state_t;

void chorus_init(chorus_state_t *c, float sample_rate);
void chorus_reset(chorus_state_t *c);
void chorus_set_amount(chorus_state_t *c, float amount01);
float chorus_get_amount(const chorus_state_t *c);

/* Processes `frames` stereo samples; out may alias in. */
void chorus_process(chorus_state_t *c, const float *in_l, const float *in_r,
                    float *out_l, float *out_r, int frames);

#endif /* CHORUS_H */

#ifdef __cplusplus
}
#endif
