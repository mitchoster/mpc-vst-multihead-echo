/* tape_imperfection.h — "living tape" stage for the MPC Multihead Echo prototype (T6).
 *
 * Two independent parts, each with its own amount (0 = bit-exact bypass):
 *   dropouts : short, musically spaced level losses (25..90 ms, 30..90 % deep),
 *              evenly random rather than bursty, with slow attack/release so they
 *              never click
 *   hiss     : band-limited noise bed (80 Hz .. 6 kHz), deterministic LCG
 *
 * Everything is deterministic for a given seed, so tests can count dropouts and
 * compare runs bit for bit. Real-time safe: no malloc, no locks.
 */

#ifndef TAPE_IMPERFECTION_H
#define TAPE_IMPERFECTION_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  float sample_rate;

  /* dropouts */
  float dropout_amount;   /* 0..1 */
  float gain;             /* current gain multiplier */
  float target;           /* 1.0 = open, < 1 = dropping out */
  long hold_left;         /* samples left in the current dropout */
  long gap_left;          /* samples until the next dropout */
  float attack;           /* one-pole coefficient toward a lower gain */
  float release;          /* one-pole coefficient back to 1.0 */
  unsigned triggered;     /* dropouts started since reset (test counter) */

  /* hiss */
  float hiss_amount;      /* 0..1 */
  float hp_x1, hp_y1;     /* 1st-order highpass 80 Hz */
  float lp_y1;            /* 1st-order lowpass 6 kHz */
  float hp_a, lp_a;

  unsigned rng;
} imperfection_state_t;

void imp_init(imperfection_state_t *s, float sample_rate, unsigned seed);
void imp_reset(imperfection_state_t *s);
void imp_set_amounts(imperfection_state_t *s, float dropout01, float hiss01);

/* Processes `frames` stereo samples; out may alias in. */
void imp_process(imperfection_state_t *s, const float *in_l, const float *in_r,
                 float *out_l, float *out_r, int frames);

#endif /* TAPE_IMPERFECTION_H */

#ifdef __cplusplus
}
#endif
