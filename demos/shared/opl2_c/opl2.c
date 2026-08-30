/* OPL2 (YM3812) FM synthesis emulator — C port of demos/shared/opl2.fc.
 *
 * Written from scratch against the YM3812 Application Manual and the
 * Adlib programmer's manual. Internal precision tracks the chip:
 *
 *   Envelope: 9-bit attenuation, 0..511 units of 0.1875 dB (0..96 dB).
 *     0 = full volume, OPL2_ENV_MAX = silent.
 *
 *   Operator output: signed 13-bit (±~4096 peak). Feeds the carrier
 *     phase accumulator in FM mode; the wave table has 1024 entries per
 *     cycle, so peak modulator output produces ±4 wavelengths of FM
 *     shift (β = 8π rad).
 *
 *   Rate generator: a free-running counter advances envelopes through a
 *     sub-rate pattern with bit-mask gating. For each operator's active
 *     stage the chip computes an effective rate (0..63):
 *
 *       eff_rate = (rate << 2) + ksr_adjust
 *       ksr_adjust = key_scale       (KSR = 1)
 *                  = key_scale >> 2  (KSR = 0)
 *       key_scale  = (block << 1) | (fnum >> 9)     0..15
 *
 *     Slow regime (eff_rate 4..47): step once every 1 << shift samples,
 *     shift = 12 - (eff_rate >> 2), advancing by the sub-rate fire
 *     pattern. Fast regime (48..63): step every sample, amount doubling
 *     per 4-rate sub-group.
 *
 *   Phase generator: 256-entry log-sin table covers the first quadrant;
 *     the other three come from index mirroring and sign flipping.
 *     Total attenuation (env + TL + KSL + AM + logsin) feeds a gain
 *     table for exp lookup. Wave shapes 0..3 are the four OPL2 patterns
 *     (sine, half-sine, abs-sine, quarter-pulse).
 *
 * We sample at the host audio rate (44.1 kHz typical) rather than the
 * chip's native 49716 Hz; the rate constants are calibrated against the
 * output rate.
 */
#include "opl2.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define OUTPUT_SCALE 2.0  /* per-channel sum -> i32 mix-buffer units; the
                           * caller's mixer is the single saturation stage */
#define TWO_PI 6.283185307179587

/* ============================================================
 * Shared tables (identical for every chip, built once)
 * ============================================================ */

/* First-quadrant log-sin attenuation in fine units (1/16 env unit =
 * 0.01172 dB), matching the YM3812's ~12-bit log_sin precision; storing
 * at env-unit precision would round away the chip's real resolution. */
static int32_t logsin_table[256];

/* Atten (fine units) -> linear amplitude scaled to OPL2_OP_PEAK.
 * 16384 entries cover 0..192 dB; beyond i ≈ 6400 the value rounds to 0. */
static int32_t gain_table[16384];

/* block -> 2^(block-1), and feedback level -> 2^(9-fb). Both sit in the
 * per-sample per-channel path — nine voices at 44.1 kHz is ~800k pow
 * calls a second if recomputed. */
static double block_scale[8];
static double fb_div[8];

static bool tables_ready = false;

static void build_tables(void)
{
    static const double pi = 3.14159265358979323846;
    static const double fine_unit_db = 0.01171875; /* 0.1875 / 16 */
    for (int i = 0; i < 256; i++) {
        double x = ((double)i + 0.5) * pi / 512.0;
        double s = sin(x);
        double atten = 0.0 - 20.0 * log10(s) / fine_unit_db;
        logsin_table[i] = (int32_t)atten;
    }
    for (int i = 0; i < 16384; i++) {
        double atten_db = (double)i * fine_unit_db;
        double amp = (double)OPL2_OP_PEAK * pow(10.0, 0.0 - atten_db / 20.0);
        gain_table[i] = (int32_t)amp;
    }
    for (int b = 0; b < 8; b++) {
        block_scale[b] = pow(2.0, (double)(b - 1));
        fb_div[b] = pow(2.0, (double)(9 - b));
    }
    tables_ready = true;
}

/* YM3812 KSL ROM, indexed by the F-number's top 4 bits. Units of
 * 0.375 dB before scaling. */
static const int32_t ksl_rom[16] = {
    0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64
};

/* Frequency multiplier table per the YM3812 spec. Non-monotone in the
 * 11/13/14 slots — approximated as their neighbors per the chip's
 * published table. */
static const double mult_val[16] = {
    0.5, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0,
    8.0, 9.0, 10.0, 10.0, 12.0, 12.0, 15.0, 15.0
};

/* Sub-rate fire patterns: for each of the 4 sub-rates within a
 * rate-doubling group, fire on 4/8, 5/8, 6/8, 7/8 of the 8 cycle
 * positions, spread evenly so the envelope advances at the right
 * average rate without bunching steps in time. */
static const int32_t eg_fire[4][8] = {
    { 0, 1, 0, 1, 0, 1, 0, 1 },
    { 0, 1, 0, 1, 1, 1, 0, 1 },
    { 0, 1, 1, 1, 0, 1, 1, 1 },
    { 0, 1, 1, 1, 1, 1, 1, 1 }
};

/* ============================================================
 * Initialization
 * ============================================================ */

opl2_chip *opl2_init(double sample_rate)
{
    if (!tables_ready)
        build_tables();
    opl2_chip *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->rate = sample_rate;
    for (int i = 0; i < OPL2_NUM_OPERATORS; i++) {
        c->op_tl[i] = 63;
        c->op_mult[i] = 1;
        c->op_env[i] = OPL2_ENV_MAX;
        c->op_stage[i] = OPL2_STAGE_OFF;
    }
    /* The noise LFSR must not start at zero — zero is a fixed point and
     * the register would never leave it, silencing the snare, cymbal
     * and hi-hat for the life of the chip. */
    c->noise = 1;
    return c;
}

/* ============================================================
 * Register decoding
 * ============================================================ */

/* Map an operator register offset (0..31) to an operator index 0..17.
 * OPL2's 3-channels-per-bank layout leaves gaps (slots 6,7 unused in
 * each 8-slot bank), which this returns -1 for. */
static int32_t op_idx(int32_t offset)
{
    int32_t group = offset / 8;
    int32_t slot = offset % 8;
    if (group > 2 || slot > 5)
        return -1;
    return (group * 3 + slot % 3) * 2 + slot / 3;
}

/* ============================================================
 * Key-scale, rate generator, envelope steps
 * ============================================================ */

/* Key-scale value (0..15) from block + fnum; feeds both KSR and KSL. */
static int32_t key_scale(int32_t block, int32_t fnum)
{
    return (block << 1) | ((fnum >> 9) & 1);
}

/* Effective rate (0..63). rate=0 is "frozen" regardless of KSR. */
static int32_t effective_rate(int32_t rate, bool ksr, int32_t ks)
{
    if (rate <= 0)
        return 0;
    int32_t eff = (rate << 2) + (ksr ? ks : ks >> 2);
    return eff > 63 ? 63 : eff;
}

/* Envelope advance per sample for a given effective rate and the chip's
 * free-running counter. Returns the step amount (0 = no advance).
 *
 *   eff_rate 0..3  : frozen
 *   eff_rate 4..47 : step once every 2^shift samples,
 *                    shift = 12 - (eff_rate >> 2), by the fire pattern
 *   eff_rate 48..63: step every sample; amount doubles per sub-group */
static int32_t rate_step(uint64_t counter, int32_t eff_rate)
{
    if (eff_rate < 4)
        return 0;
    if (eff_rate < 48) {
        int32_t shift = 12 - (eff_rate >> 2);
        uint64_t mask = ((uint64_t)1 << shift) - 1;
        if ((counter & mask) != 0)
            return 0;
        return eg_fire[eff_rate & 3][(counter >> shift) & 7];
    }
    int32_t inc_mul = 1 << ((eff_rate - 48) >> 2);
    return eg_fire[eff_rate & 3][counter & 7] * inc_mul;
}

/* Attack: log-curve approach toward 0. Each step removes ~env/8 of the
 * remaining attenuation; the +1 ensures we reach 0 instead of
 * asymptoting. Iterated `inc` times because the curve is non-linear in
 * env — multiplying the per-step delta by inc would over-decrement in
 * the fast regime where inc can reach 8. */
static int32_t attack_step(int32_t env, int32_t inc)
{
    int32_t e = env;
    for (int32_t k = 0; k < inc; k++) {
        if (e <= 0)
            break;
        e -= (e >> 3) + 1;
    }
    return e < 0 ? 0 : e;
}

/* Decay / release: linear add in atten units, capped at silence. */
static int32_t linear_step(int32_t env, int32_t inc)
{
    int32_t new_env = env + inc;
    return new_env > OPL2_ENV_MAX ? OPL2_ENV_MAX : new_env;
}

/* Advance one operator's envelope by one sample. */
static void env_advance(opl2_chip *c, int32_t idx)
{
    switch (c->op_stage[idx]) {
    case OPL2_STAGE_OFF:
    case OPL2_STAGE_SUSTAIN:
        break;
    case OPL2_STAGE_ATTACK: {
        int32_t inc = rate_step(c->counter, c->op_eff_ar[idx]);
        int32_t new_env = attack_step(c->op_env[idx], inc);
        c->op_env[idx] = new_env;
        if (new_env <= 0) {
            c->op_env[idx] = 0;
            c->op_stage[idx] = OPL2_STAGE_DECAY;
        }
        break;
    }
    case OPL2_STAGE_DECAY: {
        int32_t inc = rate_step(c->counter, c->op_eff_dr[idx]);
        int32_t new_env = linear_step(c->op_env[idx], inc);
        c->op_env[idx] = new_env;
        /* Decay target. SL=15 is special-cased to silence (per spec);
         * SL 0..14 maps to 0..42 dB in 3 dB steps = 16 env units per
         * SL step. */
        int32_t target = c->op_sl[idx] == 15 ? OPL2_ENV_MAX
                                             : c->op_sl[idx] * 16;
        if (new_env >= target) {
            c->op_env[idx] = target;
            /* EGT=1 holds at sustain; EGT=0 keeps releasing
             * (percussive) — what makes percussive AdLib SFX die away
             * cleanly instead of hanging at sustain. */
            c->op_stage[idx] = c->op_egt[idx] ? OPL2_STAGE_SUSTAIN
                                              : OPL2_STAGE_RELEASE;
        }
        break;
    }
    case OPL2_STAGE_RELEASE: {
        int32_t inc = rate_step(c->counter, c->op_eff_rr[idx]);
        int32_t new_env = linear_step(c->op_env[idx], inc);
        c->op_env[idx] = new_env;
        if (new_env >= OPL2_ENV_MAX) {
            c->op_env[idx] = OPL2_ENV_MAX;
            c->op_stage[idx] = OPL2_STAGE_OFF;
        }
        break;
    }
    }
}

/* ============================================================
 * KSL (key-scale level)
 * ============================================================ */

/* Recompute one operator's cached key-scale-level attenuation, which
 * depends on the channel's block and F-number as well as the KSL field. */
static void recompute_op_ksl(opl2_chip *c, int32_t idx,
                             int32_t block, int32_t fnum)
{
    int32_t ksl = c->op_ksl[idx];
    if (ksl == 0) {
        c->op_ksl_atten[idx] = 0;
        return;
    }
    int32_t raw = ksl_rom[(fnum >> 6) & 15] - (7 - block) * 8;
    int32_t units = raw < 0 ? 0 : raw;
    /* KSL scaling per the actual silicon: KSL=1/2/3 -> 3/1.5/6 dB per
     * octave (the original datasheet printed codes 1 and 2 swapped; the
     * chip never did that). The ROM gives 8 units per octave at
     * 0.375 dB/unit; converting to env units (0.1875 dB) doubles, so
     * the multipliers land on 2 / 1 / 4 for KSL 1 / 2 / 3. */
    c->op_ksl_atten[idx] = ksl == 1 ? units * 2
                         : ksl == 2 ? units
                                    : units * 4;
}

/* ============================================================
 * Rate cache invalidation
 * ============================================================ */

static void recompute_op_rates(opl2_chip *c, int32_t idx, int32_t ks)
{
    c->op_eff_ar[idx] = effective_rate(c->op_ar[idx], c->op_ksr[idx], ks);
    c->op_eff_dr[idx] = effective_rate(c->op_dr[idx], c->op_ksr[idx], ks);
    c->op_eff_rr[idx] = effective_rate(c->op_rr[idx], c->op_ksr[idx], ks);
}

/* Refresh both of a channel's operators after its block or F-number
 * moved — the two things that feed key scale, and so every rate and KSL
 * cache hanging off it. */
static void refresh_channel_ops(opl2_chip *c, int32_t ch)
{
    int32_t ks = key_scale(c->ch_block[ch], c->ch_fnum[ch]);
    recompute_op_rates(c, ch * 2, ks);
    recompute_op_rates(c, ch * 2 + 1, ks);
    recompute_op_ksl(c, ch * 2, c->ch_block[ch], c->ch_fnum[ch]);
    recompute_op_ksl(c, ch * 2 + 1, c->ch_block[ch], c->ch_fnum[ch]);
}

/* ============================================================
 * Register write
 * ============================================================ */

/* Key one rhythm operator on a rising edge and release it on a falling
 * one. Key-on resets the phase and restarts the attack but leaves the
 * envelope where it is, same as a channel key-on — a drum retriggered
 * faster than its own release should not restart from silence. */
static void rhythm_gate(opl2_chip *c, int32_t op, bool was, bool now)
{
    if (now && !was) {
        c->op_stage[op] = OPL2_STAGE_ATTACK;
        c->op_phase[op] = 0.0;
    }
    if (!now && was && c->op_stage[op] != OPL2_STAGE_OFF)
        c->op_stage[op] = OPL2_STAGE_RELEASE;
}

void opl2_write(opl2_chip *c, int32_t reg, int32_t val)
{
    /* Operator-indexed groups (0x20/0x40/0x60/0x80/0xE0) span 32
     * register slots each with gaps; channel-indexed groups
     * (0xA0/0xB0/0xC0) span 16 slots. Mixing the masks silently drops
     * every operator write at offset >= 16 — all of channels 6..8. */
    int32_t off = reg & 0x1F;
    int32_t idx;
    switch (reg & 0xE0) {
    case 0x20: /* AM/VIB/EGT/KSR/MULT */
        if ((idx = op_idx(off)) >= 0) {
            c->op_mult[idx] = val & 15;
            c->op_ksr[idx] = (val & 16) != 0;
            c->op_egt[idx] = (val & 32) != 0;
            c->op_vib[idx] = (val & 64) != 0;
            c->op_am[idx] = (val & 128) != 0;
            int32_t ch = idx / 2;
            recompute_op_rates(c, idx,
                               key_scale(c->ch_block[ch], c->ch_fnum[ch]));
        }
        break;
    case 0x40: /* KSL / TL */
        if ((idx = op_idx(off)) >= 0) {
            c->op_tl[idx] = val & 63;
            c->op_ksl[idx] = (val >> 6) & 3;
            int32_t ch = idx / 2;
            recompute_op_ksl(c, idx, c->ch_block[ch], c->ch_fnum[ch]);
        }
        break;
    case 0x60: /* Attack / Decay rate */
        if ((idx = op_idx(off)) >= 0) {
            c->op_ar[idx] = (val >> 4) & 15;
            c->op_dr[idx] = val & 15;
            int32_t ch = idx / 2;
            recompute_op_rates(c, idx,
                               key_scale(c->ch_block[ch], c->ch_fnum[ch]));
        }
        break;
    case 0x80: /* Sustain level / Release rate */
        if ((idx = op_idx(off)) >= 0) {
            c->op_sl[idx] = (val >> 4) & 15;
            c->op_rr[idx] = val & 15;
            int32_t ch = idx / 2;
            recompute_op_rates(c, idx,
                               key_scale(c->ch_block[ch], c->ch_fnum[ch]));
        }
        break;
    case 0xE0: /* Waveform select */
        if ((idx = op_idx(off)) >= 0)
            c->op_wf[idx] = val & 3;
        break;
    }

    int32_t ci = reg & 15;
    switch (reg & 0xF0) {
    case 0xA0: /* fnum low byte */
        if (ci < 9) {
            c->ch_fnum[ci] = (c->ch_fnum[ci] & 0x300) | (val & 255);
            refresh_channel_ops(c, ci);
        }
        break;
    case 0xB0: /* fnum high 2 bits, block, key-on bit */
        if (ci < 9) {
            c->ch_fnum[ci] = (c->ch_fnum[ci] & 0xFF) | ((val & 3) << 8);
            c->ch_block[ci] = (val >> 2) & 7;
            refresh_channel_ops(c, ci);
            /* In rhythm mode channels 6..8 are keyed through 0xBD, so
             * the key bit here is not theirs to obey. Their fnum and
             * block still are — that's how a driver tunes the tom and
             * the hi-hat. */
            bool new_key = (val & 32) != 0 && !(c->rhythm && ci >= 6);
            if (new_key && !c->ch_key[ci]) {
                /* Key-on: reset phase and start attack. DON'T reset
                 * env — real OPL2 preserves it, which matters for
                 * rapid retriggers tighter than the release time. */
                c->op_stage[ci * 2] = OPL2_STAGE_ATTACK;
                c->op_phase[ci * 2] = 0.0;
                c->op_stage[ci * 2 + 1] = OPL2_STAGE_ATTACK;
                c->op_phase[ci * 2 + 1] = 0.0;
                c->ch_prev0[ci] = 0;
                c->ch_prev1[ci] = 0;
            }
            if (!new_key && c->ch_key[ci]) {
                /* Key-off: drop to release on any still-live op. */
                if (c->op_stage[ci * 2] != OPL2_STAGE_OFF)
                    c->op_stage[ci * 2] = OPL2_STAGE_RELEASE;
                if (c->op_stage[ci * 2 + 1] != OPL2_STAGE_OFF)
                    c->op_stage[ci * 2 + 1] = OPL2_STAGE_RELEASE;
            }
            c->ch_key[ci] = new_key;
        }
        break;
    case 0xC0: /* Feedback / Connection */
        if (ci < 9) {
            c->ch_fb[ci] = (val >> 1) & 7;
            c->ch_add[ci] = (val & 1) != 0;
        }
        break;
    }

    /* 0xBD: LFO depths, rhythm-mode enable, and the five percussion key
     * bits — edge-triggered exactly like a channel's key bit. */
    if (reg == 0xBD) {
        c->dam = (val & 128) != 0;
        c->dvb = (val & 64) != 0;
        bool on = (val & 32) != 0;
        if (!on && c->rhythm) {
            /* Leaving rhythm mode hands channels 7 and 8 back as
             * melodic voices. Silence their operators on the way out:
             * what is sounding is a drum, and letting it ring under
             * whatever comes next is a click at best. */
            for (int op = 14; op < 18; op++) {
                c->op_stage[op] = OPL2_STAGE_OFF;
                c->op_env[op] = OPL2_ENV_MAX;
            }
            c->rhythm_keys = 0;
        }
        c->rhythm = on;
        if (on) {
            int32_t keys = val & 31;
            int32_t prev = c->rhythm_keys;
            bool bd_now = (keys & 16) != 0;
            bool bd_was = (prev & 16) != 0;
            /* The bass drum is a two-operator channel: both operators
             * key together and its feedback history resets with them —
             * the same thing a 0xB0 key-on does. */
            rhythm_gate(c, 12, bd_was, bd_now);
            rhythm_gate(c, 13, bd_was, bd_now);
            if (bd_now && !bd_was) {
                c->ch_prev0[6] = 0;
                c->ch_prev1[6] = 0;
            }
            rhythm_gate(c, 15, (prev & 8) != 0, (keys & 8) != 0); /* snare */
            rhythm_gate(c, 16, (prev & 4) != 0, (keys & 4) != 0); /* tom */
            rhythm_gate(c, 17, (prev & 2) != 0, (keys & 2) != 0); /* cymbal */
            rhythm_gate(c, 14, (prev & 1) != 0, (keys & 1) != 0); /* hi-hat */
            c->rhythm_keys = keys;
        }
    }
}

/* ============================================================
 * Wave / FM sample generation
 * ============================================================ */

/* Sample the OPL2 waveform from a 10-bit phase index with total
 * attenuation (env + TL + KSL + AM, in env units) already summed.
 * Returns the signed 13-bit operator output.
 *
 * Phase decomposition: half = bit 9 (which half-cycle), quarter = bit 8
 * (rising/falling), idx = low 8 bits (log-sin index, mirrored when
 * falling). The four OPL2 waveforms gate the negative half and/or
 * falling quarter and may force the sign positive. */
static int32_t wave_at_index(int32_t idx10, int32_t wf, int32_t total_atten)
{
    int32_t half = (idx10 >> 9) & 1;
    int32_t quarter = (idx10 >> 8) & 1;
    int32_t idx = idx10 & 255;
    int32_t table_idx = quarter ? 255 - idx : idx;
    /* Waveform 3 is the rising quarter of |sin| in BOTH halves — two
     * identical pulses per cycle, so its spectrum is dominated by the
     * second harmonic and it reads an octave above the F-number. */
    bool active = wf == 0 ? true
                : wf == 1 ? half == 0
                : wf == 2 ? true
                          : quarter == 0;
    if (!active)
        return 0;
    bool positive = wf >= 2 ? true : half == 0;
    /* Combine attenuation contributions in fine units: shift the
     * env-unit total left by 4 to match log_sin's 1/16-unit precision. */
    int32_t total_fine = (total_atten << 4) + logsin_table[table_idx];
    int32_t amp = (total_fine >= 16384 || total_fine < 0)
                      ? 0
                      : gain_table[total_fine];
    return positive ? amp : -amp;
}

static int32_t wave_sample(double phase, int32_t wf, int32_t total_atten)
{
    double p = phase - floor(phase);
    return wave_at_index((int32_t)(p * 1024.0) & 1023, wf, total_atten);
}

/* Advance one operator's phase by a sample and return its 10-bit phase
 * index. `ch` supplies fnum and block, because an operator's frequency
 * is its channel's times its own MULT. */
static int32_t step_op(opl2_chip *c, int32_t op, int32_t ch, double vib_mul)
{
    double base_freq = (double)c->ch_fnum[ch] * block_scale[c->ch_block[ch]]
                       * 49716.0 / 524288.0;
    double f = base_freq * mult_val[c->op_mult[op]]
               * (c->op_vib[op] ? vib_mul : 1.0);
    c->op_phase[op] += f / c->rate;
    if (c->op_phase[op] >= 1.0)
        c->op_phase[op] -= floor(c->op_phase[op]);
    return (int32_t)(c->op_phase[op] * 1024.0) & 1023;
}

static int32_t op_atten(const opl2_chip *c, int32_t op, int32_t am_atten)
{
    return c->op_env[op] + c->op_tl[op] * 4 + c->op_ksl_atten[op]
           + (c->op_am[op] ? am_atten : 0);
}

/* The four single-operator rhythm voices, summed. The snare, cymbal and
 * hi-hat do not read their own phase accumulator to make sound: they
 * build a phase index out of the noise register and bits borrowed from
 * the hi-hat's and cymbal's accumulators — the OPL2's entire answer to
 * "how do you make a noise on an FM chip". The accumulators still
 * advance (they are the bit source), so tuning channels 7 and 8 still
 * changes the sound. */
static int32_t rhythm_sample(opl2_chip *c, int32_t am_atten, double vib_mul)
{
    enum { HH = 14, SD = 15, TT = 16, TC = 17 };
    for (int op = 14; op < 18; op++)
        env_advance(c, op);
    int32_t hh_idx = step_op(c, HH, 7, vib_mul);
    (void)step_op(c, SD, 7, vib_mul);
    int32_t tt_idx = step_op(c, TT, 8, vib_mul);
    int32_t tc_idx = step_op(c, TC, 8, vib_mul);

    int32_t hh_bit2 = (hh_idx >> 2) & 1;
    int32_t hh_bit3 = (hh_idx >> 3) & 1;
    int32_t hh_bit7 = (hh_idx >> 7) & 1;
    int32_t hh_bit8 = (hh_idx >> 8) & 1;
    /* Three exclusive-ors folded together, mixing the hi-hat's phase
     * with the cymbal's — what makes both inharmonic rather than merely
     * high. The cymbal bits are last sample's, because the hi-hat is
     * generated first — the hardware's slot order. */
    int32_t rm_xor = (hh_bit2 ^ hh_bit7) | (hh_bit3 ^ c->rm_tc_bit5)
                     | (c->rm_tc_bit3 ^ c->rm_tc_bit5);
    int32_t n_bit = (int32_t)(c->noise & 1);

    int32_t sum = 0;
    /* Hi-hat: one of two phase indices, chosen by noise. */
    if (c->op_stage[HH] != OPL2_STAGE_OFF) {
        int32_t idx = (rm_xor << 9) | ((rm_xor ^ n_bit) ? 0xD0 : 0x34);
        sum += wave_at_index(idx, c->op_wf[HH], op_atten(c, HH, am_atten));
    }
    /* Snare: one bit of the hi-hat's phase, one bit of noise. */
    if (c->op_stage[SD] != OPL2_STAGE_OFF) {
        int32_t idx = (hh_bit8 << 9) | ((hh_bit8 ^ n_bit) << 8);
        sum += wave_at_index(idx, c->op_wf[SD], op_atten(c, SD, am_atten));
    }
    /* Tom-tom: an ordinary pitched operator — the only one of the four
     * that plays a note. */
    if (c->op_stage[TT] != OPL2_STAGE_OFF)
        sum += wave_at_index(tt_idx, c->op_wf[TT], op_atten(c, TT, am_atten));
    /* Cymbal: the same fold as the hi-hat, half a cycle along. */
    if (c->op_stage[TC] != OPL2_STAGE_OFF)
        sum += wave_at_index((rm_xor << 9) | 0x80, c->op_wf[TC],
                             op_atten(c, TC, am_atten));
    c->rm_tc_bit3 = (tc_idx >> 3) & 1;
    c->rm_tc_bit5 = (tc_idx >> 5) & 1;
    return sum;
}

int32_t opl2_sample(opl2_chip *c)
{
    c->counter++;
    /* LFO advance. Tremolo at 3.7 Hz and vibrato at 6.1 Hz per the
     * Adlib programmer's manual; the chip's staircase patterns are
     * approximated with smooth curves of the same period and amplitude. */
    c->lfo_am_phase += 3.7 / c->rate;
    if (c->lfo_am_phase >= 1.0)
        c->lfo_am_phase -= 1.0;
    c->lfo_vib_phase += 6.1 / c->rate;
    if (c->lfo_vib_phase >= 1.0)
        c->lfo_vib_phase -= 1.0;
    /* Tremolo as a triangle wave: 0 -> peak -> 0 over one LFO cycle.
     * Peak attenuation 1.0 dB (DAM=0) or 4.8 dB (DAM=1) = ~5 or ~26 env
     * units. One peak per period — |sin| would double the modulation
     * frequency and alias its cusps into audible buzz. */
    double am_unit = c->lfo_am_phase < 0.5 ? c->lfo_am_phase * 2.0
                                           : 2.0 - c->lfo_am_phase * 2.0;
    int32_t am_atten = (int32_t)(am_unit * (c->dam ? 25.6 : 5.33));
    /* Vibrato as a frequency multiplier (~±7 or ±14 cents). */
    double vib_mul = 1.0 + sin(c->lfo_vib_phase * TWO_PI)
                               * (c->dvb ? 0.00811 : 0.00405);

    /* The noise register runs free, one step per sample, whether or not
     * rhythm mode is on. 23-bit LFSR; the xor constant is the tap set. */
    if (c->noise & 1)
        c->noise ^= 0x800302;
    c->noise >>= 1;

    int32_t sum = 0;
    for (int32_t ch = 0; ch < OPL2_NUM_CHANNELS; ch++) {
        /* In rhythm mode channels 7 and 8 stop being two-operator
         * voices — their four operators are four separate drums.
         * Channel 6 stays: the bass drum IS an ordinary FM voice, only
         * keyed from somewhere else. */
        if (c->rhythm && ch >= 7)
            continue;
        int32_t mi = ch * 2;
        int32_t ci = ch * 2 + 1;
        /* Skip channels with both ops idle — no audible output. */
        if (c->op_stage[ci] == OPL2_STAGE_OFF
            && c->op_stage[mi] == OPL2_STAGE_OFF)
            continue;
        env_advance(c, mi);
        env_advance(c, ci);
        /* If the carrier is silent after this advance, skip the rest. */
        if (c->op_stage[ci] == OPL2_STAGE_OFF)
            continue;
        /* YM3812 frequency formula: f = fnum * 2^(block-1) * 49716/2^19. */
        double base_freq = (double)c->ch_fnum[ch]
                           * block_scale[c->ch_block[ch]]
                           * 49716.0 / 524288.0;
        /* Modulator phase advance (with vibrato if enabled). floor wrap
         * (not a single subtract) so per-sample advances > 1 cycle stay
         * bounded — possible at block=7 / mult=15 / high fnum. */
        double mf = base_freq * mult_val[c->op_mult[mi]]
                    * (c->op_vib[mi] ? vib_mul : 1.0);
        c->op_phase[mi] += mf / c->rate;
        if (c->op_phase[mi] >= 1.0)
            c->op_phase[mi] -= floor(c->op_phase[mi]);
        int32_t m_total_atten = op_atten(c, mi, am_atten);
        /* Feedback: the chip sums the modulator's two most recent
         * outputs and right-shifts by (9-fb). fb=7 -> ±2 cycles (4π
         * rad) of self-modulation, halving per fb step. In normalized
         * phase units the divisor works out to 2^(9-fb) * 1024. */
        int32_t fb = c->ch_fb[ch];
        double fb_phase_offset = 0.0;
        if (fb > 0)
            fb_phase_offset = (double)(c->ch_prev0[ch] + c->ch_prev1[ch])
                              / fb_div[fb] / 1024.0;
        int32_t mout = wave_sample(c->op_phase[mi] + fb_phase_offset,
                                   c->op_wf[mi], m_total_atten);
        c->ch_prev1[ch] = c->ch_prev0[ch];
        c->ch_prev0[ch] = mout;
        /* Carrier phase advance; same floor-wrap rationale. */
        double cf = base_freq * mult_val[c->op_mult[ci]]
                    * (c->op_vib[ci] ? vib_mul : 1.0);
        c->op_phase[ci] += cf / c->rate;
        if (c->op_phase[ci] >= 1.0)
            c->op_phase[ci] -= floor(c->op_phase[ci]);
        int32_t c_total_atten = op_atten(c, ci, am_atten);
        if (c->ch_add[ch]) {
            /* Additive: both ops play independently and sum. */
            sum += mout + wave_sample(c->op_phase[ci], c->op_wf[ci],
                                      c_total_atten);
        } else {
            /* FM: modulator output (±~4096) added to the carrier's
             * 10-bit phase index gives peak FM shift of ±4 wavelengths
             * (β = 8π rad); in normalized phase that's mout / 1024. */
            sum += wave_sample(c->op_phase[ci] + (double)mout / 1024.0,
                               c->op_wf[ci], c_total_atten);
        }
    }
    if (c->rhythm)
        sum += rhythm_sample(c, am_atten, vib_mul);
    /* Scale into mix-buffer units. No per-chip clamp: the caller's
     * mixer soft-clip is the single saturation stage in the pipeline. */
    return (int32_t)((double)sum * OUTPUT_SCALE);
}

/* ============================================================
 * Higher-level helpers for AdLib-style sound programming
 * ============================================================ */

/* Modulator register-offset for channel N. Carrier sits at mo + 3. */
static int32_t op_mod_offset(int32_t channel)
{
    if (channel < 3)
        return channel;
    return channel < 6 ? channel + 5 : channel + 10;
}

void opl2_load_instrument(opl2_chip *c, int32_t channel,
                          int32_t m_char, int32_t c_char,
                          int32_t m_scale, int32_t c_scale,
                          int32_t m_atk, int32_t c_atk,
                          int32_t m_sus, int32_t c_sus,
                          int32_t m_wave, int32_t c_wave,
                          int32_t n_conn)
{
    int32_t mo = op_mod_offset(channel);
    int32_t co = mo + 3;
    opl2_write(c, 0x20 + mo, m_char);
    opl2_write(c, 0x40 + mo, m_scale);
    opl2_write(c, 0x60 + mo, m_atk);
    opl2_write(c, 0x80 + mo, m_sus);
    opl2_write(c, 0xE0 + mo, m_wave);
    opl2_write(c, 0x20 + co, c_char);
    opl2_write(c, 0x40 + co, c_scale);
    opl2_write(c, 0x60 + co, c_atk);
    opl2_write(c, 0x80 + co, c_sus);
    opl2_write(c, 0xE0 + co, c_wave);
    opl2_write(c, 0xC0 + channel, n_conn);
}

void opl2_note_on(opl2_chip *c, int32_t channel, int32_t block, int32_t fnum)
{
    opl2_write(c, 0xA0 + channel, fnum & 255);
    opl2_write(c, 0xB0 + channel,
               32 | ((block & 7) << 2) | ((fnum >> 8) & 3));
}

/* Preserving block/fnum here is not a nicety: a bare 0 write would stop
 * the phase generator (the tail decays as frozen DC instead of ringing
 * at pitch) and recompute the release rates at the lowest key scale.
 * The chip struct shadows both, so key-off clears bit 5 and leaves the
 * rest. */
void opl2_note_off(opl2_chip *c, int32_t channel)
{
    opl2_write(c, 0xB0 + channel,
               ((c->ch_block[channel] & 7) << 2)
                   | ((c->ch_fnum[channel] >> 8) & 3));
}

void opl2_set_pitch(opl2_chip *c, int32_t channel, int32_t block, int32_t fnum)
{
    opl2_write(c, 0xA0 + channel, fnum & 255);
    opl2_write(c, 0xB0 + channel,
               (c->ch_key[channel] ? 32 : 0)
                   | ((block & 7) << 2) | ((fnum >> 8) & 3));
}

/* ============================================================
 * Rhythm mode
 * ============================================================ */

void opl2_rhythm_mode(opl2_chip *c, bool on)
{
    opl2_write(c, 0xBD, (c->dam ? 128 : 0) | (c->dvb ? 64 : 0)
                            | (on ? 32 : 0) | (on ? c->rhythm_keys : 0));
}

/* The 0xBD key bit for one voice. */
static int32_t rhythm_bit(opl2_rhythm_voice v)
{
    switch (v) {
    case OPL2_RHYTHM_BASS_DRUM: return 16;
    case OPL2_RHYTHM_SNARE:     return 8;
    case OPL2_RHYTHM_TOM:       return 4;
    case OPL2_RHYTHM_CYMBAL:    return 2;
    case OPL2_RHYTHM_HIHAT:     return 1;
    }
    return 0;
}

static void rhythm_write(opl2_chip *c, int32_t keys)
{
    opl2_write(c, 0xBD, (c->dam ? 128 : 0) | (c->dvb ? 64 : 0)
                            | (c->rhythm ? 32 : 0) | keys);
}

void opl2_rhythm_strike(opl2_chip *c, opl2_rhythm_voice v)
{
    int32_t b = rhythm_bit(v);
    if (c->rhythm_keys & b)
        rhythm_write(c, c->rhythm_keys & ~b);
    rhythm_write(c, c->rhythm_keys | b);
}

void opl2_rhythm_release(opl2_chip *c, opl2_rhythm_voice v)
{
    rhythm_write(c, c->rhythm_keys & ~rhythm_bit(v));
}

/* Register offset of the operator a rhythm voice lives on. */
static int32_t rhythm_op_offset(opl2_rhythm_voice v)
{
    switch (v) {
    case OPL2_RHYTHM_BASS_DRUM: return 0x10; /* channel 6 modulator */
    case OPL2_RHYTHM_HIHAT:     return 0x11; /* channel 7 modulator */
    case OPL2_RHYTHM_TOM:       return 0x12; /* channel 8 modulator */
    case OPL2_RHYTHM_SNARE:     return 0x14; /* channel 7 carrier */
    case OPL2_RHYTHM_CYMBAL:    return 0x15; /* channel 8 carrier */
    }
    return 0;
}

void opl2_load_rhythm_op(opl2_chip *c, opl2_rhythm_voice v,
                         int32_t op_char, int32_t op_scale, int32_t op_atk,
                         int32_t op_sus, int32_t op_wave)
{
    int32_t o = rhythm_op_offset(v);
    opl2_write(c, 0x20 + o, op_char);
    opl2_write(c, 0x40 + o, op_scale);
    opl2_write(c, 0x60 + o, op_atk);
    opl2_write(c, 0x80 + o, op_sus);
    opl2_write(c, 0xE0 + o, op_wave);
}

bool opl2_channel_idle(const opl2_chip *c, int32_t channel)
{
    return c->op_stage[channel * 2] == OPL2_STAGE_OFF
           && c->op_stage[channel * 2 + 1] == OPL2_STAGE_OFF;
}

void opl2_silence_voices(opl2_chip *c)
{
    for (int i = 0; i < OPL2_NUM_OPERATORS; i++) {
        c->op_env[i] = OPL2_ENV_MAX;
        c->op_stage[i] = OPL2_STAGE_OFF;
        c->op_phase[i] = 0.0;
    }
    for (int i = 0; i < OPL2_NUM_CHANNELS; i++) {
        c->ch_key[i] = false;
        c->ch_prev0[i] = 0;
        c->ch_prev1[i] = 0;
    }
}

/* ============================================================
 * Ticked sample producer
 * ============================================================ */

void opl2_fill_ticked(opl2_chip *c,
                      int32_t *buf, int32_t count,
                      int32_t sample_rate, int32_t tick_rate,
                      double *tick_accum,
                      void (*advance)(void *ctx), void *ctx)
{
    opl2_fill_ticked_vol(c, buf, count, sample_rate, tick_rate, tick_accum,
                         advance, ctx, 1.0);
}

void opl2_fill_ticked_vol(opl2_chip *c,
                          int32_t *buf, int32_t count,
                          int32_t sample_rate, int32_t tick_rate,
                          double *tick_accum,
                          void (*advance)(void *ctx), void *ctx,
                          double gain)
{
    double samples_per_tick = (double)sample_rate / (double)tick_rate;
    double scale = gain < 0.0 ? 0.0 : gain;
    for (int32_t i = 0; i < count; i++) {
        int32_t scaled = (int32_t)((double)opl2_sample(c) * scale);
        buf[i * 2] += scaled;
        buf[i * 2 + 1] += scaled;
        *tick_accum += 1.0;
        if (*tick_accum >= samples_per_tick) {
            *tick_accum -= samples_per_tick;
            advance(ctx);
        }
    }
}
