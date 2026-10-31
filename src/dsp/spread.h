/* spread.h — stereo offset for the MPC Multihead Echo prototype (T7).
 *
 * Delays the right channel by up to a few milliseconds (0..3 ms) while the left
 * stays untouched. That is what a tape machine's second channel does when the
 * heads are not perfectly aligned: width without pitch or level tricks.
 *
 * At offset 0 the module is a bit-exact bypass. The delay time is smoothed, so
 * turning the knob does not click.
 */

#ifndef SPREAD_H
#define SPREAD_H

#ifdef __cplusplus
extern "C" {
#endif

#define SPREAD_MAX_MS 6.0f
#define SPREAD_BUF 1024 /* power of two, > 6 ms at 44.1 kHz */

typedef struct {
  float sample_rate;
  float offset_ms;   /* target */
  float current;     /* smoothed delay in samples */
  int write_pos;
  float buf_r[SPREAD_BUF];
} spread_state_t;

void spread_init(spread_state_t *s, float sample_rate);
void spread_reset(spread_state_t *s);
void spread_set_offset_ms(spread_state_t *s, float ms);
float spread_get_offset_ms(const spread_state_t *s);

void spread_process(spread_state_t *s, const float *in_l, const float *in_r,
                    float *out_l, float *out_r, int frames);

#endif /* SPREAD_H */

#ifdef __cplusplus
}
#endif
