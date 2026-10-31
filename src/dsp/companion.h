/* companion.h — read-only status feed for the companion page (phone/tablet).
 *
 * Why this exists: MPC OS can only animate what it can poll (a meter strip), and every
 * moving widget costs the device a redraw. The echo timeline needs smooth motion, so the
 * plugin publishes a small snapshot and the *browser* draws it. The device stays quiet.
 *
 * Contract:
 *   - attach() claims one of kSlots static slots and starts the HTTP thread on the first
 *     call; detach() releases the slot but leaves the listener running (a re-insert then
 *     costs nothing, and /state answers `active:0` while no instance exists).
 *   - publish() is called from the audio thread: stores only, no allocation, no locks, no
 *     syscalls beyond clock_gettime.
 *   - The reader (HTTP thread) never dereferences an instance: all state lives in the
 *     static slots, so a destroy_instance() can never race the server into freed memory.
 */
#ifndef MHEC_COMPANION_H
#define MHEC_COMPANION_H

namespace mhec {

const int kSlots = 4;

struct Snapshot {
  /* what the page draws */
  float wet;        /* output level 0..1, the same number as the on-device meter */
  float delay_ms;   /* echo time the motor is actually at */
  float bpm;        /* host tempo */
  float rate;       /* repeat rate actually sent to the core, 0..1 */
  float wow_pct;    /* deviation of that rate from its base, in percent of the range */
  float jog;        /* performing switches, 0..1 (jog is smoothed) */
  float warp;
  float twist;
  /* the settings that explain the picture */
  float echo;
  float wow;        /* the wow/flutter setting, not the deviation */
  float chorus;
  float saturation;
  float dropouts;
  float hiss;
  float spread;
  float mix;
  float intensity;
  float reverb;
  int   mode;       /* 0..11, index into the mode table */
  int   fb;         /* 0 Safe, 1 Hot, 2 Unstable */
  int   sync;       /* 0 Off, 1 1/32, 2 1/16, 3 1/8 */
  int   drift;      /* 0/1 */
  int   tapeage;    /* 0 New, 1 Used, 2 Worn */
  int   preamp;     /* 0 Clean, 1 Warm, 2 Dirty */
  int   wetonly;    /* 0/1 */
  int   motion;     /* 0/1 */
  int   twisttype;  /* 0 Normal, 1 Hard, 2 Natural */
  int   warptype;   /* 0 Normal, 1 Deep, 2 Wild */
  int   revmode;    /* 0 201, 1 202 */
  /* straight from the core's mode table, so the drawing cannot disagree with the sound */
  int   heads[3];   /* 1 = this head reads the tape in this mode */
  int   reverb_head;
};

/* A parameter change asked for by the page. The HTTP thread only queues it; the audio
 * thread applies it in process_block, because set_one()/apply_params() touch the DSP state
 * that the render loop owns. Same shape as VADER's mirror inbox (2026-11-02). */
struct Command {
  int idx;    /* VST parameter index (append-only contract) */
  float val;  /* real units: 0..1 for continuous, the option index for enums */
};

/* The parameter names, so the server can refuse an unknown key instead of guessing. The module
 * owns the list (it is the append-only contract); it hands it over once at load. */
void register_param_keys(const char *const *keys, int count);

/* Each instance has its OWN ring. A shared ring looked simpler and was wrong: the first audio
 * thread to drain advanced the tail past the other instance's entries (review, 2026-11-02). */
int  provide(int slot, const Command &c);        /* HTTP thread: 1 queued, 0 when full */
int  take_commands(int slot, Command *out, int max);   /* audio thread: what arrived */

int  attach();                         /* slot index, or -1 when all four are taken */
void detach(int slot);
void publish(int slot, const Snapshot &s);

/* Port the listener actually bound (0 = none). For tests and the docs. */
int  port();
void stop_server();                    /* host test / shutdown only */

}  // namespace mhec

#endif
