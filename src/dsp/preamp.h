/* preamp.h — Preamp stage of the MPC Multihead Echo prototype (T4a).
 *
 * A self-contained, offline-usable DSP unit with three modes:
 *   PRE_CLEAN  — bit-exact bypass (reference path; the DC blocker lives in the
 *                nonlinear modes, where asymmetry is what creates DC)
 *   PRE_WARM   — +3 dB drive into tanh, 3 % asymmetry, low shelf +1.5 dB @ 100 Hz,
 *                1st-order lowpass at 12 kHz
 *   PRE_DIRTY  — +9 dB drive into tanh, 8 % asymmetry, 1st-order highpass 60 Hz,
 *                1st-order lowpass 8 kHz
 *
 * C99, 44.1 kHz, 128-frame blocks, real-time safe (no malloc/lock in the render
 * path). Only <math.h>/<string.h>/<stdint.h>. The two channels are processed
 * identically with independent state. In-place operation is allowed.
 */

#ifndef PREAMP_H
#define PREAMP_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  PRE_CLEAN = 0,
  PRE_WARM = 1,
  PRE_DIRTY = 2
} pre_mode_t;

/* 2x halfband around the saturator (T4c): 15-tap stage-B set from the vendored
 * DuskOversampler (4 one-sided taps, stopband -75 dB, wide transition) — a
 * third of the MACs of the 47-tap stage-A set for a deeper stopband. */
#define PRE_OS_TAPS 31
#define PRE_HB_RING 128
#define PRE_HB_SIDE 4

typedef struct {
  float buf[PRE_HB_RING];
  int pos;
} pre_hb_t;

typedef struct {
  float sample_rate;
  pre_mode_t mode;

  /* mode coefficients (derived once in pre_init/pre_set_mode) */
  float mode_db;    /* the mode's own drive, dB (0 / +3 / +9) */
  float sat_db;     /* SATURATION knob: extra drive on top, dB (0 = mode alone) */
  float drive;      /* linear input gain   = db_to_lin(mode_db + sat_db) */
  float out_gain;   /* linear output compensation */
  float bias;       /* asymmetry offset inside tanh */
  float lp_b, lp_a; /* 1st-order lowpass  y = b*x + a*y1 */
  float hp_b, hp_a; /* 1st-order highpass y = b*(x - x1) + a*y1 */
  float sh_g;       /* low-shelf gain (linear), 1 = off */
  float sh_b, sh_a; /* shelf detector lowpass */

  /* per-channel state: 0 = left, 1 = right */
  float lp_x1[2], lp_y1[2];
  float hp_x1[2], hp_y1[2];
  float sh_z[2];
  float dc_x1[2], dc_y1[2];
  /* ADAA (antiderivative anti-aliasing) state for the tanh saturator */
  float ad_x1[2];   /* previous driven input (2x rate) */
  float ad_f1[2];   /* previous antiderivative value */
  /* 2x halfband oversampling around the saturator (T4c) */
  pre_hb_t up_hb[2]; /* interpolation halfband, base rate */
  pre_hb_t dn_hb[2]; /* decimation halfband, 2x rate */
} pre_state_t;

void pre_init(pre_state_t *s, float sample_rate, pre_mode_t mode);
void pre_set_mode(pre_state_t *s, pre_mode_t mode);
/* SATURATION: extra drive in dB on top of the mode's own (0…+12 dB).
 * 0 dB leaves every mode bit-identical to before, including the PRE_CLEAN
 * bypass; above 0 dB PRE_CLEAN becomes a plain symmetric tanh saturator.
 * The output compensation is recomputed with the drive, so a small signal keeps
 * its level and the knob adds harmonics instead of loudness. */
void pre_set_drive(pre_state_t *s, float db);
void pre_reset(pre_state_t *s);

/* Processes `frames` stereo samples. out_l/out_r may alias in_l/in_r.
 * frames >= 0. No allocation, no locks, no denormal traps. */
void pre_process(pre_state_t *s, const float *in_l, const float *in_r,
                 float *out_l, float *out_r, int frames);

#endif /* PREAMP_H */

#ifdef __cplusplus
}
#endif
