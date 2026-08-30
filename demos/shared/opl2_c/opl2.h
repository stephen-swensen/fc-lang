/* OPL2 (YM3812) FM synthesis emulator — C port of demos/shared/opl2.fc.
 *
 * Models 9 melodic channels with two operators each (modulator + carrier)
 * connectable in FM or additive mode, plus rhythm mode (reg 0xBD bit 5) —
 * five percussion voices on channels 6..8, three driven by the chip's
 * noise generator.
 *
 * A chip is a heap value driven exactly as the hardware was: program its
 * registers, key notes on and off, and pull samples out one at a time.
 *
 *     opl2_chip *c = opl2_init(44100.0);       // sample rate, Hz
 *     opl2_load_instrument(c, 0, ...);         // 11 AdLib bytes onto a channel
 *     opl2_note_on(c, 0, block, fnum);         // key on
 *     int32_t s = opl2_sample(c);              // one mono sample; repeat
 *     free(c);
 *
 * opl2_write takes raw register/value pairs for anything the helpers don't
 * cover, so an existing Adlib register dump plays back unchanged.
 * opl2_fill_ticked / opl2_fill_ticked_vol run the common driver shape —
 * fill a stereo i32 buffer while calling back at a fixed tick rate.
 * Chips are independent, so music and sound effects can each have their own.
 *
 * Two things a caller owns. Sampling runs at the host's audio rate rather
 * than the chip's native 49716 Hz, with the envelope rate constants
 * calibrated against the output rate. And opl2_sample does not clamp: a
 * full chip sums to roughly ±9 * OPL2_OP_PEAK in FM mode, so the final
 * limiting belongs to the caller's mixer.
 *
 * See opl2.c for the internal precision model — envelope units, operator
 * scale, and the rate generator's sub-rate patterns.
 */
#ifndef OPL2_H
#define OPL2_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    OPL2_NUM_CHANNELS  = 9,   /* melodic channels; valid channel args are 0..8 */
    OPL2_NUM_OPERATORS = 18,  /* two per channel: modulator + carrier */
    OPL2_ENV_MAX       = 511, /* 9-bit envelope, units of 0.1875 dB -> 96 dB */
    OPL2_OP_PEAK       = 4096 /* peak operator output (signed 13-bit) */
};

/* Envelope stages. OFF = 0 so a zero-initialised chip starts every
 * operator in the silent stage automatically. */
typedef enum {
    OPL2_STAGE_OFF = 0,
    OPL2_STAGE_ATTACK,
    OPL2_STAGE_DECAY,
    OPL2_STAGE_SUSTAIN,
    OPL2_STAGE_RELEASE
} opl2_stage;

/* The five voices rhythm mode puts on channels 6..8. The bass drum is a
 * whole two-operator channel (channel 6); the other four are single
 * operators that reach the output directly:
 *   hihat  ch7 modulator   snare  ch7 carrier
 *   tom    ch8 modulator   cymbal ch8 carrier */
typedef enum {
    OPL2_RHYTHM_BASS_DRUM = 0,
    OPL2_RHYTHM_SNARE,
    OPL2_RHYTHM_TOM,
    OPL2_RHYTHM_CYMBAL,
    OPL2_RHYTHM_HIHAT
} opl2_rhythm_voice;

/* One emulated chip. State is flat arrays indexed by operator (0..17) or
 * channel (0..8) rather than nested structs, which keeps the per-sample
 * inner loop a straight walk over int32 arrays.
 *
 * Build one with opl2_init rather than by hand: a zeroed chip has every
 * operator at full volume in the OFF stage, whereas init silences the
 * operators and seeds the rate caches. Free with free(). */
typedef struct {
    /* ---- Per-operator state ---- */
    double     op_phase[OPL2_NUM_OPERATORS]; /* accumulator, 0..1 (wraps) */
    int32_t    op_env[OPL2_NUM_OPERATORS];   /* 0 = full vol, ENV_MAX = silent */
    opl2_stage op_stage[OPL2_NUM_OPERATORS];
    /* Register fields (raw nibbles/bytes). */
    int32_t    op_tl[OPL2_NUM_OPERATORS];    /* 0..63, 0.75 dB per step */
    int32_t    op_mult[OPL2_NUM_OPERATORS];  /* 0..15 */
    int32_t    op_ar[OPL2_NUM_OPERATORS];    /* 0..15 attack rate */
    int32_t    op_dr[OPL2_NUM_OPERATORS];    /* 0..15 decay rate */
    int32_t    op_sl[OPL2_NUM_OPERATORS];    /* 0..15 sustain level */
    int32_t    op_rr[OPL2_NUM_OPERATORS];    /* 0..15 release rate */
    int32_t    op_wf[OPL2_NUM_OPERATORS];    /* 0..3 waveform select */
    bool       op_am[OPL2_NUM_OPERATORS];
    bool       op_vib[OPL2_NUM_OPERATORS];
    bool       op_egt[OPL2_NUM_OPERATORS];
    bool       op_ksr[OPL2_NUM_OPERATORS];
    int32_t    op_ksl[OPL2_NUM_OPERATORS];   /* 0..3 key-scale level */
    /* Cached effective rates (recomputed when rate field / KSR / block /
     * fnum changes). The rate generator reads these each sample. */
    int32_t    op_eff_ar[OPL2_NUM_OPERATORS];
    int32_t    op_eff_dr[OPL2_NUM_OPERATORS];
    int32_t    op_eff_rr[OPL2_NUM_OPERATORS];
    /* Cached KSL attenuation in env units. */
    int32_t    op_ksl_atten[OPL2_NUM_OPERATORS];

    /* ---- Per-channel state ---- */
    int32_t    ch_fnum[OPL2_NUM_CHANNELS];
    int32_t    ch_block[OPL2_NUM_CHANNELS];
    bool       ch_key[OPL2_NUM_CHANNELS];
    int32_t    ch_fb[OPL2_NUM_CHANNELS];
    bool       ch_add[OPL2_NUM_CHANNELS];
    /* Previous two modulator outputs, for the 2-sample feedback sum. */
    int32_t    ch_prev0[OPL2_NUM_CHANNELS];
    int32_t    ch_prev1[OPL2_NUM_CHANNELS];

    /* ---- Global state ---- */
    double     rate;          /* output sample rate */
    uint64_t   counter;       /* free-running rate-generator counter */
    double     lfo_am_phase;
    double     lfo_vib_phase;
    bool       dam;           /* tremolo depth (false=1.0 dB, true=4.8 dB) */
    bool       dvb;           /* vibrato depth (false=±7c, true=±14c) */

    /* ---- Rhythm mode ---- */
    bool       rhythm;        /* 0xBD bit 5 */
    int32_t    rhythm_keys;   /* shadow of 0xBD bits 4..0, for edge detection */
    /* 23-bit LFSR, the chip's noise source. Free-running: advances every
     * sample whether or not rhythm mode is on, as the hardware's does. */
    uint64_t   noise;
    /* The cymbal's phase bits from the previous sample; the hi-hat reads
     * them first, exactly as the hardware's slot ordering makes it. */
    int32_t    rm_tc_bit3;
    int32_t    rm_tc_bit5;
} opl2_chip;

/* Allocate and initialize a chip for the given output sample rate (Hz).
 * Also builds the shared log-sin/gain tables on first call. The caller
 * owns the returned chip and frees it with free(). Returns NULL on OOM. */
opl2_chip *opl2_init(double sample_rate);

/* Write one OPL2 register, exactly as software wrote the hardware port.
 * This is the chip's whole control surface — the helpers below are
 * conveniences over it — so an Adlib register dump can be replayed
 * straight through. Unmodeled registers are ignored. */
void opl2_write(opl2_chip *c, int32_t reg, int32_t val);

/* Produce one output sample (mono, unclamped — see header comment). */
int32_t opl2_sample(opl2_chip *c);

/* ---- Higher-level helpers for AdLib-style sound programming ---- */

/* Program an 11-byte AdLib instrument onto a melodic channel. */
void opl2_load_instrument(opl2_chip *c, int32_t channel,
                          int32_t m_char, int32_t c_char,
                          int32_t m_scale, int32_t c_scale,
                          int32_t m_atk, int32_t c_atk,
                          int32_t m_sus, int32_t c_sus,
                          int32_t m_wave, int32_t c_wave,
                          int32_t n_conn);

/* Key-on with (block, F-number). F-number is 10 bits. */
void opl2_note_on(opl2_chip *c, int32_t channel, int32_t block, int32_t fnum);

/* Key-off: clears the keyon bit (starts the release envelope) while
 * preserving block and F-number, so the tail rings out at pitch instead
 * of decaying as frozen DC at the lowest key scale. */
void opl2_note_off(opl2_chip *c, int32_t channel);

/* Retune a channel without touching its key state — a sounding note bends
 * instead of re-articulating (what a MIDI pitch-bend wants). Harmless on
 * a silent channel. */
void opl2_set_pitch(opl2_chip *c, int32_t channel, int32_t block, int32_t fnum);

/* ---- Rhythm mode ---- */

/* Turn rhythm mode on or off. On, channels 6..8 become five drums (the
 * melodic pool drops to 0..5). Off, the three channels come back with
 * their operators silenced rather than left ringing. */
void opl2_rhythm_mode(opl2_chip *c, bool on);

/* Articulate one drum (drops and re-raises the key bit: the chip keys on
 * the rising edge, so striking an already-sounding drum must re-edge). */
void opl2_rhythm_strike(opl2_chip *c, opl2_rhythm_voice v);

/* Let one drum go. */
void opl2_rhythm_release(opl2_chip *c, opl2_rhythm_voice v);

/* Program one single-operator rhythm voice (five AdLib register bytes).
 * Not for the bass drum, which is a whole two-operator channel and takes
 * opl2_load_instrument(c, 6, ...). */
void opl2_load_rhythm_op(opl2_chip *c, opl2_rhythm_voice v,
                         int32_t op_char, int32_t op_scale, int32_t op_atk,
                         int32_t op_sus, int32_t op_wave);

/* Whether both of a channel's operators have finished releasing, so a
 * voice allocator can steal it without an audible click. */
bool opl2_channel_idle(const opl2_chip *c, int32_t channel);

/* Force every voice silent and clear phase + feedback history, keeping
 * instrument programming intact (for driver loop wraps). */
void opl2_silence_voices(opl2_chip *c);

/* ---- Ticked sample producer ---- */

/* Mix `count` stereo frames into `buf` (i32 L/R interleaved, summed into
 * whatever is already there). Every sample_rate / tick_rate frames the
 * `advance` callback runs once with `ctx` to apply the next event tick.
 * No clipping anywhere in this path. */
void opl2_fill_ticked(opl2_chip *c,
                      int32_t *buf, int32_t count,
                      int32_t sample_rate, int32_t tick_rate,
                      double *tick_accum,
                      void (*advance)(void *ctx), void *ctx);

/* Gain-scaled variant (linear scalar; 1.0 = default mix; floored at 0). */
void opl2_fill_ticked_vol(opl2_chip *c,
                          int32_t *buf, int32_t count,
                          int32_t sample_rate, int32_t tick_rate,
                          double *tick_accum,
                          void (*advance)(void *ctx), void *ctx,
                          double gain);

#ifdef __cplusplus
}
#endif

#endif /* OPL2_H */
