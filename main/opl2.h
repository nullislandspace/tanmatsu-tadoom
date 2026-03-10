/* opl2.h -- Lightweight OPL2 (YM3812) emulator for embedded systems
 *
 * Designed for real-time operation at non-native sample rates (e.g. 22050 Hz).
 * Runs phase generation at the target rate with scaled increments, and advances
 * the envelope generator at the correct real-time rate using fractional
 * accumulation. Lookup tables from Nuked-OPL3 (proven correct from YM3812 die).
 *
 * Only supports OPL2 features: 9 2-op channels, 4 waveforms, no rhythm mode.
 */

#ifndef OPL2_H
#define OPL2_H

#include <stdint.h>

#ifndef OPL2_NUM_CHANNELS
#define OPL2_NUM_CHANNELS 9
#endif

#define OPL2_MAX_CHANNELS 9

typedef struct {
    /* Registers */
    uint8_t reg_mult;   /* Frequency multiplier (0-15) */
    uint8_t reg_ksr;    /* Key scale rate */
    uint8_t reg_egt;    /* Envelope type (1=sustaining) */
    uint8_t reg_vib;    /* Vibrato enable */
    uint8_t reg_am;     /* Tremolo (AM) enable */
    uint8_t reg_tl;     /* Total level / attenuation (0-63) */
    uint8_t reg_ksl;    /* Key scale level (0-3) */
    uint8_t reg_ar;     /* Attack rate (0-15) */
    uint8_t reg_dr;     /* Decay rate (0-15) */
    uint8_t reg_sl;     /* Sustain level (0-15, 15→31) */
    uint8_t reg_rr;     /* Release rate (0-15) */
    uint8_t reg_wf;     /* Waveform select (0-3) */
    uint8_t key;        /* Key on/off */

    /* Phase generator */
    uint32_t pg_phase;
    uint16_t pg_phase_out;
    uint8_t pg_reset;   /* Sticky: set by envelope, cleared by phase gen */

    /* Envelope generator */
    uint16_t eg_rout;   /* Raw envelope output (0-511) */
    uint16_t eg_out;    /* Total attenuation (computed before waveform) */
    uint8_t eg_gen;     /* ADSR state */
    uint8_t eg_ksl;     /* KSL attenuation value */

    /* Output */
    int16_t out;
    int16_t prev_out;
} opl2_op_t;

typedef struct {
    opl2_op_t op[2];    /* [0]=modulator, [1]=carrier */
    uint16_t fnum;
    uint8_t block;
    uint8_t fb;         /* Feedback level (0-7) */
    uint8_t con;        /* Connection: 0=FM, 1=additive */
    uint8_t ksv;        /* Key scale value */
    uint8_t cha;        /* Output to left channel */
    uint8_t chb;        /* Output to right channel */
} opl2_ch_t;

typedef struct {
    opl2_ch_t ch[OPL2_MAX_CHANNELS];

    /* Global timers */
    uint32_t timer;
    uint64_t eg_timer;
    uint8_t eg_timerrem;
    uint8_t eg_state;       /* Toggles 0/1 each internal tick */
    uint8_t eg_add;
    uint8_t eg_timer_lo;

    /* Tremolo / vibrato */
    uint8_t tremolo;
    uint8_t trem_pos;
    uint8_t trem_shift;     /* 1=deep, 4=shallow */
    uint8_t vib_pos;
    uint8_t vib_shift;      /* 0=deep, 1=shallow */

    /* Config */
    uint8_t wse;            /* Waveform select enable */
    uint8_t nts;            /* Note select */
    uint32_t sample_rate;
    uint32_t phase_scale;   /* Fixed-point 49716/sample_rate (16-bit frac) */
    uint32_t env_accum;     /* Fractional accumulator for envelope timing */
} opl2_chip_t;

void opl2_init(opl2_chip_t *chip, uint32_t sample_rate);
void opl2_write(opl2_chip_t *chip, uint16_t reg, uint8_t val);
void opl2_generate(opl2_chip_t *chip, int16_t *buf, uint32_t num_samples);

#endif
