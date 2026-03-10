/* opl2.c -- Lightweight OPL2 (YM3812) emulator
 *
 * Lookup tables extracted from Nuked-OPL3 by Nuke.YKT (LGPL 2.1+),
 * which were verified against YM3812 die shots. Algorithm simplified
 * for OPL2-only operation at non-native sample rates.
 *
 * Key differences from Nuked-OPL3:
 *   - 9 channels / 18 operators only (no OPL3 second register bank)
 *   - 4 waveforms only (no OPL3 waveforms 4-7)
 *   - No rhythm/percussion mode
 *   - No write buffer (immediate register writes)
 *   - Phase runs at target sample rate with scaled increments
 *   - Envelope timer advanced via fractional accumulation for correct timing
 *   - Inline waveform calc (no function pointer indirection)
 */

#include <string.h>
#include "opl2.h"

#define OPL2_NATIVE_RATE 49716

/* Envelope states */
enum {
    EG_ATTACK  = 0,
    EG_DECAY   = 1,
    EG_SUSTAIN = 2,
    EG_RELEASE = 3
};

/* ------------------------------------------------------------------ */
/* Lookup tables (from Nuked-OPL3, verified against YM3812 die)       */
/* ------------------------------------------------------------------ */

static const uint16_t logsinrom[256] = {
    0x859, 0x6c3, 0x607, 0x58b, 0x52e, 0x4e4, 0x4a6, 0x471,
    0x443, 0x41a, 0x3f5, 0x3d3, 0x3b5, 0x398, 0x37e, 0x365,
    0x34e, 0x339, 0x324, 0x311, 0x2ff, 0x2ed, 0x2dc, 0x2cd,
    0x2bd, 0x2af, 0x2a0, 0x293, 0x286, 0x279, 0x26d, 0x261,
    0x256, 0x24b, 0x240, 0x236, 0x22c, 0x222, 0x218, 0x20f,
    0x206, 0x1fd, 0x1f5, 0x1ec, 0x1e4, 0x1dc, 0x1d4, 0x1cd,
    0x1c5, 0x1be, 0x1b7, 0x1b0, 0x1a9, 0x1a2, 0x19b, 0x195,
    0x18f, 0x188, 0x182, 0x17c, 0x177, 0x171, 0x16b, 0x166,
    0x160, 0x15b, 0x155, 0x150, 0x14b, 0x146, 0x141, 0x13c,
    0x137, 0x133, 0x12e, 0x129, 0x125, 0x121, 0x11c, 0x118,
    0x114, 0x10f, 0x10b, 0x107, 0x103, 0x0ff, 0x0fb, 0x0f8,
    0x0f4, 0x0f0, 0x0ec, 0x0e9, 0x0e5, 0x0e2, 0x0de, 0x0db,
    0x0d7, 0x0d4, 0x0d1, 0x0cd, 0x0ca, 0x0c7, 0x0c4, 0x0c1,
    0x0be, 0x0bb, 0x0b8, 0x0b5, 0x0b2, 0x0af, 0x0ac, 0x0a9,
    0x0a7, 0x0a4, 0x0a1, 0x09f, 0x09c, 0x099, 0x097, 0x094,
    0x092, 0x08f, 0x08d, 0x08a, 0x088, 0x086, 0x083, 0x081,
    0x07f, 0x07d, 0x07a, 0x078, 0x076, 0x074, 0x072, 0x070,
    0x06e, 0x06c, 0x06a, 0x068, 0x066, 0x064, 0x062, 0x060,
    0x05e, 0x05c, 0x05b, 0x059, 0x057, 0x055, 0x053, 0x052,
    0x050, 0x04e, 0x04d, 0x04b, 0x04a, 0x048, 0x046, 0x045,
    0x043, 0x042, 0x040, 0x03f, 0x03e, 0x03c, 0x03b, 0x039,
    0x038, 0x037, 0x035, 0x034, 0x033, 0x031, 0x030, 0x02f,
    0x02e, 0x02d, 0x02b, 0x02a, 0x029, 0x028, 0x027, 0x026,
    0x025, 0x024, 0x023, 0x022, 0x021, 0x020, 0x01f, 0x01e,
    0x01d, 0x01c, 0x01b, 0x01a, 0x019, 0x018, 0x017, 0x017,
    0x016, 0x015, 0x014, 0x014, 0x013, 0x012, 0x011, 0x011,
    0x010, 0x00f, 0x00f, 0x00e, 0x00d, 0x00d, 0x00c, 0x00c,
    0x00b, 0x00a, 0x00a, 0x009, 0x009, 0x008, 0x008, 0x007,
    0x007, 0x007, 0x006, 0x006, 0x005, 0x005, 0x005, 0x004,
    0x004, 0x004, 0x003, 0x003, 0x003, 0x002, 0x002, 0x002,
    0x002, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000
};

static const uint16_t exprom[256] = {
    0x7fa, 0x7f5, 0x7ef, 0x7ea, 0x7e4, 0x7df, 0x7da, 0x7d4,
    0x7cf, 0x7c9, 0x7c4, 0x7bf, 0x7b9, 0x7b4, 0x7ae, 0x7a9,
    0x7a4, 0x79f, 0x799, 0x794, 0x78f, 0x78a, 0x784, 0x77f,
    0x77a, 0x775, 0x770, 0x76a, 0x765, 0x760, 0x75b, 0x756,
    0x751, 0x74c, 0x747, 0x742, 0x73d, 0x738, 0x733, 0x72e,
    0x729, 0x724, 0x71f, 0x71a, 0x715, 0x710, 0x70b, 0x706,
    0x702, 0x6fd, 0x6f8, 0x6f3, 0x6ee, 0x6e9, 0x6e5, 0x6e0,
    0x6db, 0x6d6, 0x6d2, 0x6cd, 0x6c8, 0x6c4, 0x6bf, 0x6ba,
    0x6b5, 0x6b1, 0x6ac, 0x6a8, 0x6a3, 0x69e, 0x69a, 0x695,
    0x691, 0x68c, 0x688, 0x683, 0x67f, 0x67a, 0x676, 0x671,
    0x66d, 0x668, 0x664, 0x65f, 0x65b, 0x657, 0x652, 0x64e,
    0x649, 0x645, 0x641, 0x63c, 0x638, 0x634, 0x630, 0x62b,
    0x627, 0x623, 0x61e, 0x61a, 0x616, 0x612, 0x60e, 0x609,
    0x605, 0x601, 0x5fd, 0x5f9, 0x5f5, 0x5f0, 0x5ec, 0x5e8,
    0x5e4, 0x5e0, 0x5dc, 0x5d8, 0x5d4, 0x5d0, 0x5cc, 0x5c8,
    0x5c4, 0x5c0, 0x5bc, 0x5b8, 0x5b4, 0x5b0, 0x5ac, 0x5a8,
    0x5a4, 0x5a0, 0x59c, 0x599, 0x595, 0x591, 0x58d, 0x589,
    0x585, 0x581, 0x57e, 0x57a, 0x576, 0x572, 0x56f, 0x56b,
    0x567, 0x563, 0x560, 0x55c, 0x558, 0x554, 0x551, 0x54d,
    0x549, 0x546, 0x542, 0x53e, 0x53b, 0x537, 0x534, 0x530,
    0x52c, 0x529, 0x525, 0x522, 0x51e, 0x51b, 0x517, 0x514,
    0x510, 0x50c, 0x509, 0x506, 0x502, 0x4ff, 0x4fb, 0x4f8,
    0x4f4, 0x4f1, 0x4ed, 0x4ea, 0x4e7, 0x4e3, 0x4e0, 0x4dc,
    0x4d9, 0x4d6, 0x4d2, 0x4cf, 0x4cc, 0x4c8, 0x4c5, 0x4c2,
    0x4be, 0x4bb, 0x4b8, 0x4b5, 0x4b1, 0x4ae, 0x4ab, 0x4a8,
    0x4a4, 0x4a1, 0x49e, 0x49b, 0x498, 0x494, 0x491, 0x48e,
    0x48b, 0x488, 0x485, 0x482, 0x47e, 0x47b, 0x478, 0x475,
    0x472, 0x46f, 0x46c, 0x469, 0x466, 0x463, 0x460, 0x45d,
    0x45a, 0x457, 0x454, 0x451, 0x44e, 0x44b, 0x448, 0x445,
    0x442, 0x43f, 0x43c, 0x439, 0x436, 0x433, 0x430, 0x42d,
    0x42a, 0x428, 0x425, 0x422, 0x41f, 0x41c, 0x419, 0x416,
    0x414, 0x411, 0x40e, 0x40b, 0x408, 0x406, 0x403, 0x400
};

/* Frequency multiplier table (×2): 1/2,1,2,3,4,5,6,7,8,9,10,10,12,12,15,15 */
static const uint8_t mt[16] = {
    1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30
};

/* Key scale level ROM */
static const uint8_t kslrom[16] = {
    0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64
};
static const uint8_t kslshift[4] = { 8, 1, 2, 0 };

/* Envelope increment step patterns */
static const uint8_t eg_incstep[4][4] = {
    { 0, 0, 0, 0 },
    { 1, 0, 0, 0 },
    { 1, 0, 1, 0 },
    { 1, 1, 1, 0 }
};

/* ------------------------------------------------------------------ */
/* Register address decoding                                          */
/* ------------------------------------------------------------------ */

/* Map operator register offset (0x00-0x15) to channel and op index.
 * OPL2 layout: offsets 0-5 = ch0-2, 8-0D = ch3-5, 10-15 = ch6-8.
 * Within each group of 3: first 3 = modulator, next 3 = carrier. */
static const int8_t reg_to_ch[0x20] = {
     0,  1,  2,  0,  1,  2, -1, -1,
     3,  4,  5,  3,  4,  5, -1, -1,
     6,  7,  8,  6,  7,  8, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1
};
static const int8_t reg_to_op[0x20] = {
     0,  0,  0,  1,  1,  1, -1, -1,
     0,  0,  0,  1,  1,  1, -1, -1,
     0,  0,  0,  1,  1,  1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1
};

/* ------------------------------------------------------------------ */
/* KSL / KSV helpers                                                  */
/* ------------------------------------------------------------------ */

static void update_ksv(opl2_chip_t *chip, int ch)
{
    opl2_ch_t *c = &chip->ch[ch];
    c->ksv = (c->block << 1) | ((c->fnum >> (9 - chip->nts)) & 1);
}

static void update_ksl_op(opl2_chip_t *chip, int ch, int op_idx)
{
    opl2_ch_t *c = &chip->ch[ch];
    opl2_op_t *o = &c->op[op_idx];
    int16_t ksl = (kslrom[c->fnum >> 6] << 2) - ((8 - c->block) << 5);
    if (ksl < 0) ksl = 0;
    o->eg_ksl = (uint8_t)ksl;
}

static void update_ksl_ch(opl2_chip_t *chip, int ch)
{
    update_ksl_op(chip, ch, 0);
    update_ksl_op(chip, ch, 1);
}

/* ------------------------------------------------------------------ */
/* Envelope generator (from Nuked-OPL3, simplified for OPL2)          */
/* ------------------------------------------------------------------ */

static void envelope_calc(opl2_op_t *op, opl2_chip_t *chip, int ch_idx)
{
    opl2_ch_t *ch = &chip->ch[ch_idx];
    uint8_t reg_rate = 0;
    uint8_t reset = 0;

    if (op->key && op->eg_gen == EG_RELEASE) {
        reset = 1;
        reg_rate = op->reg_ar;
    } else {
        switch (op->eg_gen) {
        case EG_ATTACK:  reg_rate = op->reg_ar; break;
        case EG_DECAY:   reg_rate = op->reg_dr; break;
        case EG_SUSTAIN: if (!op->reg_egt) reg_rate = op->reg_rr; break;
        case EG_RELEASE: reg_rate = op->reg_rr; break;
        }
    }

    /* Sticky reset flag — consumed by phase generator later */
    op->pg_reset |= reset;

    /* Rate calculation */
    uint8_t ks = ch->ksv >> ((op->reg_ksr ^ 1) << 1);
    uint8_t nonzero = (reg_rate != 0);
    uint8_t rate = ks + (reg_rate << 2);
    uint8_t rate_hi = rate >> 2;
    uint8_t rate_lo = rate & 0x03;
    if (rate_hi & 0x10) rate_hi = 0x0F;

    uint8_t eg_shift = rate_hi + chip->eg_add;
    uint8_t shift = 0;

    if (nonzero) {
        if (rate_hi < 12) {
            if (chip->eg_state) {
                if (eg_shift == 12)      shift = 1;
                else if (eg_shift == 13) shift = (rate_lo >> 1) & 1;
                else if (eg_shift == 14) shift = rate_lo & 1;
            }
        } else {
            shift = (rate_hi & 3) + eg_incstep[rate_lo][chip->eg_timer_lo];
            if (shift & 4) shift = 3;
            if (!shift) shift = chip->eg_state;
        }
    }

    uint16_t eg_rout = op->eg_rout;
    int16_t eg_inc = 0;
    uint8_t eg_off = ((op->eg_rout & 0x1F8) == 0x1F8);

    /* Instant attack */
    if (reset && rate_hi == 0x0F) eg_rout = 0;
    /* Envelope off */
    if (op->eg_gen != EG_ATTACK && !reset && eg_off) eg_rout = 0x1FF;

    switch (op->eg_gen) {
    case EG_ATTACK:
        if (!op->eg_rout) {
            op->eg_gen = EG_DECAY;
        } else if (op->key && shift > 0 && rate_hi != 0x0F) {
            eg_inc = ~op->eg_rout >> (4 - shift);
        }
        break;
    case EG_DECAY:
        if ((op->eg_rout >> 4) == op->reg_sl) {
            op->eg_gen = EG_SUSTAIN;
        } else if (!eg_off && !reset && shift > 0) {
            eg_inc = 1 << (shift - 1);
        }
        break;
    case EG_SUSTAIN:
    case EG_RELEASE:
        if (!eg_off && !reset && shift > 0) {
            eg_inc = 1 << (shift - 1);
        }
        break;
    }

    op->eg_rout = (eg_rout + eg_inc) & 0x1FF;

    if (reset) op->eg_gen = EG_ATTACK;
    if (!op->key) op->eg_gen = EG_RELEASE;
}

/* ------------------------------------------------------------------ */
/* Global timer advancement (tremolo, vibrato, envelope timer)        */
/* ------------------------------------------------------------------ */

static void advance_global_timer(opl2_chip_t *chip)
{
    /* Tremolo */
    if ((chip->timer & 0x3F) == 0x3F) {
        chip->trem_pos = (chip->trem_pos + 1) % 210;
    }
    if (chip->trem_pos < 105) {
        chip->tremolo = chip->trem_pos >> chip->trem_shift;
    } else {
        chip->tremolo = (210 - chip->trem_pos) >> chip->trem_shift;
    }

    /* Vibrato */
    if ((chip->timer & 0x3FF) == 0x3FF) {
        chip->vib_pos = (chip->vib_pos + 1) & 7;
    }

    chip->timer++;

    /* Envelope global state */
    if (chip->eg_state) {
        uint8_t shift = 0;
        while (shift < 13 && ((chip->eg_timer >> shift) & 1) == 0) {
            shift++;
        }
        chip->eg_add = (shift > 12) ? 0 : shift + 1;
        chip->eg_timer_lo = (uint8_t)(chip->eg_timer & 3);
    }

    if (chip->eg_timerrem || chip->eg_state) {
        if (chip->eg_timer == UINT64_C(0xFFFFFFFFF)) {
            chip->eg_timer = 0;
            chip->eg_timerrem = 1;
        } else {
            chip->eg_timer++;
            chip->eg_timerrem = 0;
        }
    }

    chip->eg_state ^= 1;
}

/* ------------------------------------------------------------------ */
/* Waveform generation (inline, no function pointer indirection)      */
/* ------------------------------------------------------------------ */

static inline int16_t calc_exp(uint32_t level)
{
    if (level > 0x1FFF) level = 0x1FFF;
    return (int16_t)((exprom[level & 0xFF] << 1) >> (level >> 8));
}

static inline int16_t calc_sin(uint16_t phase, uint16_t envelope, uint8_t wf)
{
    uint16_t out;
    uint16_t neg;
    phase &= 0x3FF;

    switch (wf & 0x03) {
    case 0: /* Full sine */
        neg = (phase & 0x200) ? 0xFFFF : 0;
        out = (phase & 0x100) ? logsinrom[(phase & 0xFF) ^ 0xFF]
                              : logsinrom[phase & 0xFF];
        return calc_exp(out + (envelope << 3)) ^ neg;

    case 1: /* Half-sine (positive half only, negative half silent) */
        if (phase & 0x200) {
            out = 0x1000;
        } else if (phase & 0x100) {
            out = logsinrom[(phase & 0xFF) ^ 0xFF];
        } else {
            out = logsinrom[phase & 0xFF];
        }
        return calc_exp(out + (envelope << 3));

    case 2: /* Abs-sine (rectified) */
        out = (phase & 0x100) ? logsinrom[(phase & 0xFF) ^ 0xFF]
                              : logsinrom[phase & 0xFF];
        return calc_exp(out + (envelope << 3));

    case 3: /* Quarter-sine (first quarter only, rest silent) */
        if (phase & 0x100) {
            out = 0x1000;
        } else {
            out = logsinrom[phase & 0xFF];
        }
        return calc_exp(out + (envelope << 3));
    }

    return 0; /* unreachable */
}

/* ------------------------------------------------------------------ */
/* Phase generator                                                    */
/* ------------------------------------------------------------------ */

static void phase_generate(opl2_op_t *op, opl2_ch_t *ch, opl2_chip_t *chip)
{
    uint16_t f_num = ch->fnum;

    /* Vibrato */
    if (op->reg_vib) {
        int8_t range = (f_num >> 7) & 7;
        uint8_t vibpos = chip->vib_pos;
        if (!(vibpos & 3))      range = 0;
        else if (vibpos & 1)    range >>= 1;
        range >>= chip->vib_shift;
        if (vibpos & 4) range = -range;
        f_num = (uint16_t)(f_num + range);
    }

    uint32_t basefreq = ((uint32_t)f_num << ch->block) >> 1;
    op->pg_phase_out = (uint16_t)(op->pg_phase >> 9);

    if (op->pg_reset) {
        op->pg_phase = 0;
        op->pg_reset = 0;
    }

    /* Phase increment scaled for target sample rate:
     * Original at 49716 Hz: inc = (basefreq * mt[mult]) >> 1
     * At target rate: inc = original_inc * (49716 / sample_rate)
     * phase_scale = (49716 << 16) / sample_rate */
    uint32_t phase_inc = (basefreq * mt[op->reg_mult]) >> 1;
    op->pg_phase += (uint32_t)(((uint64_t)phase_inc * chip->phase_scale) >> 16);
}

/* ------------------------------------------------------------------ */
/* Sample generation                                                  */
/* ------------------------------------------------------------------ */

void opl2_generate(opl2_chip_t *chip, int16_t *buf, uint32_t num_samples)
{
    for (uint32_t s = 0; s < num_samples; s++) {
        /* Advance envelope timer at native OPL2 rate via fractional accumulation.
         * This runs ~2.255 envelope ticks per output sample at 22050 Hz,
         * maintaining correct real-time envelope/tremolo/vibrato behavior. */
        chip->env_accum += OPL2_NATIVE_RATE;
        while (chip->env_accum >= chip->sample_rate) {
            chip->env_accum -= chip->sample_rate;
            advance_global_timer(chip);
            for (int c = 0; c < OPL2_NUM_CHANNELS; c++) {
                envelope_calc(&chip->ch[c].op[0], chip, c);
                envelope_calc(&chip->ch[c].op[1], chip, c);
            }
        }

        /* Generate output for each channel */
        int32_t out_l = 0, out_r = 0;

        for (int c = 0; c < OPL2_NUM_CHANNELS; c++) {
            opl2_ch_t *ch = &chip->ch[c];
            opl2_op_t *mod = &ch->op[0];
            opl2_op_t *car = &ch->op[1];

            /* Compute total attenuation for both operators */
            mod->eg_out = mod->eg_rout + (mod->reg_tl << 2)
                        + (mod->eg_ksl >> kslshift[mod->reg_ksl])
                        + (mod->reg_am ? chip->tremolo : 0);
            car->eg_out = car->eg_rout + (car->reg_tl << 2)
                        + (car->eg_ksl >> kslshift[car->reg_ksl])
                        + (car->reg_am ? chip->tremolo : 0);

            /* Phase generation */
            phase_generate(mod, ch, chip);
            phase_generate(car, ch, chip);

            /* Modulator: feedback + waveform */
            int16_t fb = 0;
            if (ch->fb) {
                fb = (mod->out + mod->prev_out) >> (9 - ch->fb);
            }
            mod->prev_out = mod->out;
            mod->out = calc_sin(mod->pg_phase_out + fb,
                                mod->eg_out, mod->reg_wf);

            /* Carrier: FM modulation from modulator, or independent */
            int16_t mod_in = ch->con ? 0 : mod->out;
            car->out = calc_sin(car->pg_phase_out + mod_in,
                                car->eg_out, car->reg_wf);

            /* Channel output */
            int16_t accm;
            if (ch->con) {
                accm = mod->out + car->out; /* Additive */
            } else {
                accm = car->out;            /* FM: carrier only */
            }
            if (ch->cha) out_l += accm;
            if (ch->chb) out_r += accm;
        }

        /* Amplify — OPL2 operator peak is ~4084, typical GENMIDI instruments
         * output much less due to TL attenuation. Real hardware had an analog
         * amplifier after the DAC. 4x gain brings output to useful levels. */
        out_l <<= 2;
        out_r <<= 2;

        /* Clip */
        if (out_l > 32767) out_l = 32767;
        else if (out_l < -32768) out_l = -32768;
        if (out_r > 32767) out_r = 32767;
        else if (out_r < -32768) out_r = -32768;

        buf[s * 2]     = (int16_t)out_l;
        buf[s * 2 + 1] = (int16_t)out_r;
    }
}

/* ------------------------------------------------------------------ */
/* Register write                                                     */
/* ------------------------------------------------------------------ */

void opl2_write(opl2_chip_t *chip, uint16_t reg, uint8_t val)
{
    uint8_t r = reg & 0xFF;

    /* Operator registers: 0x20-0x35, 0x40-0x55, 0x60-0x75, 0x80-0x95, 0xE0-0xF5 */
    if ((r >= 0x20 && r <= 0x35) || (r >= 0x40 && r <= 0x55) ||
        (r >= 0x60 && r <= 0x75) || (r >= 0x80 && r <= 0x95) ||
        (r >= 0xE0 && r <= 0xF5)) {
        uint8_t offset = r & 0x1F;
        if (offset >= 0x18) return;
        int ch = reg_to_ch[offset];
        int op = reg_to_op[offset];
        if (ch < 0) return;

        opl2_op_t *o = &chip->ch[ch].op[op];

        switch (r & 0xE0) {
        case 0x20: /* AM / VIB / EGT / KSR / MULT */
            o->reg_am   = (val >> 7) & 1;
            o->reg_vib  = (val >> 6) & 1;
            o->reg_egt  = (val >> 5) & 1;
            o->reg_ksr  = (val >> 4) & 1;
            o->reg_mult = val & 0x0F;
            break;
        case 0x40: /* KSL / TL */
            o->reg_ksl = (val >> 6) & 3;
            o->reg_tl  = val & 0x3F;
            update_ksl_op(chip, ch, op);
            break;
        case 0x60: /* AR / DR */
            o->reg_ar = (val >> 4) & 0x0F;
            o->reg_dr = val & 0x0F;
            break;
        case 0x80: /* SL / RR */
            o->reg_sl = (val >> 4) & 0x0F;
            if (o->reg_sl == 0x0F) o->reg_sl = 0x1F;
            o->reg_rr = val & 0x0F;
            break;
        case 0xE0: /* Waveform */
            o->reg_wf = chip->wse ? (val & 0x03) : 0;
            break;
        }
        return;
    }

    /* Channel registers: 0xA0-0xA8, 0xB0-0xB8, 0xC0-0xC8 */
    if (r >= 0xA0 && r <= 0xC8) {
        int ch = r & 0x0F;
        if (ch >= OPL2_MAX_CHANNELS) return;
        opl2_ch_t *c = &chip->ch[ch];

        switch (r & 0xF0) {
        case 0xA0: /* F-Number low */
            c->fnum = (c->fnum & 0x300) | val;
            update_ksv(chip, ch);
            update_ksl_ch(chip, ch);
            break;
        case 0xB0: /* Key-On / Block / F-Num high */
            c->fnum = (c->fnum & 0xFF) | ((val & 3) << 8);
            c->block = (val >> 2) & 7;
            update_ksv(chip, ch);
            update_ksl_ch(chip, ch);
            c->op[0].key = (val >> 5) & 1;
            c->op[1].key = (val >> 5) & 1;
            break;
        case 0xC0: /* Feedback / Connection / L-R output */
            c->fb  = (val >> 1) & 7;
            c->con = val & 1;
            c->cha = (val & 0x10) ? 1 : 0;
            c->chb = (val & 0x20) ? 1 : 0;
            break;
        }
        return;
    }

    /* Global registers */
    switch (r) {
    case 0x01: /* Waveform Select Enable */
        chip->wse = (val >> 5) & 1;
        break;
    case 0x08: /* Note Select */
        chip->nts = (val >> 6) & 1;
        break;
    case 0xBD: /* Rhythm / tremolo-vibrato depth */
        chip->trem_shift = (val & 0x80) ? 1 : 4; /* deep=1, shallow=4 */
        chip->vib_shift  = (val & 0x40) ? 0 : 1; /* deep=0, shallow=1 */
        /* Rhythm mode key-on bits ignored (no rhythm mode support) */
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Init / Reset                                                       */
/* ------------------------------------------------------------------ */

void opl2_init(opl2_chip_t *chip, uint32_t sample_rate)
{
    memset(chip, 0, sizeof(opl2_chip_t));

    chip->sample_rate = sample_rate;
    chip->phase_scale = (uint32_t)(((uint64_t)OPL2_NATIVE_RATE << 16) / sample_rate);

    /* Default envelope state: all operators in release, fully attenuated */
    for (int c = 0; c < OPL2_MAX_CHANNELS; c++) {
        chip->ch[c].cha = 1;
        chip->ch[c].chb = 1;
        for (int o = 0; o < 2; o++) {
            chip->ch[c].op[o].eg_rout = 0x1FF;
            chip->ch[c].op[o].eg_gen = EG_RELEASE;
        }
    }

    /* Default tremolo/vibrato: shallow */
    chip->trem_shift = 4;
    chip->vib_shift = 1;
}
