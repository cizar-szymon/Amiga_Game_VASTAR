/* Vastar port - AY-3-8910 sound chip, see ay8910.h
 *
 * Time is counted in ticks of 8 chip clocks. A tone output toggles every TP ticks
 * (f = clock / 16TP), the noise LFSR shifts every 2*NP ticks and the envelope steps
 * every 2*EP ticks (16 steps = clock / 256EP). Each output sample averages the
 * tone outputs over the ticks it covers, which keeps high notes free of aliasing.
 */
#include <string.h>
#include "ay8910.h"

/* measured AY-3-8910 DAC levels (0..1) scaled to 8000 per channel */
static const int32_t vol_table[16] = {
       0,   80,  116,  168,  246,  364,  516,  859,
    1013, 1638, 2338, 2983, 3940, 5083, 6445, 8000
};

/* 65536 / n: the tone averages divide by the ticks of a sample (at most ~25) */
static uint32_t recip[64];

static const uint8_t reg_mask[16] = {
    0xff, 0x0f, 0xff, 0x0f, 0xff, 0x0f, 0x1f, 0xff,
    0x1f, 0x1f, 0x1f, 0xff, 0xff, 0x0f, 0xff, 0xff
};

void ay_init(AY8910 *ay, uint32_t clock, uint32_t sample_rate)
{
    uint32_t ticks = clock / 8;   /* 16.16 without 64 bit arithmetic */
    int i;
    memset(ay, 0, sizeof *ay);
    ay->tick_step = ((ticks / sample_rate) << 16) + (((ticks % sample_rate) << 16) / sample_rate);
    ay->port_a = ay->port_b = 0xff;
    for (i = 1; i < 64; i++)
        recip[i] = 65536 / (uint32_t)i;
    ay_reset(ay);
}

static void env_restart(AY8910 *ay)
{
    uint8_t shape = ay->regs[13];
    ay->env_attack = (shape & 0x04) ? 0x0f : 0x00;
    if (!(shape & 0x08)) {
        ay->env_hold = 1;
        ay->env_alternate = ay->env_attack;
    } else {
        ay->env_hold = shape & 0x01;
        ay->env_alternate = shape & 0x02;
    }
    ay->env_step = 15;
    ay->env_holding = 0;
    ay->env_count = 0;
}

void ay_reset(AY8910 *ay)
{
    int i;
    for (i = 0; i < 16; i++)
        ay->regs[i] = 0;
    ay->regs[7] = 0xff;
    ay->latch = 0;
    for (i = 0; i < 3; i++) {
        ay->tone_count[i] = 0;
        ay->tone_out[i] = 0;
    }
    ay->noise_count = 0;
    ay->rng = 1;
    env_restart(ay);
    ay->tick_frac = 0;
    ay->dc_in = ay->dc_out = 0;
}

void ay_address_w(AY8910 *ay, uint8_t v)
{
    ay->latch = v & 0x0f;
}

void ay_data_w(AY8910 *ay, uint8_t v)
{
    uint8_t r = ay->latch;
    ay->regs[r] = v & reg_mask[r];
    if (r == 13)
        env_restart(ay);
}

uint8_t ay_data_r(AY8910 *ay)
{
    uint8_t r = ay->latch;
    if (r == 14 && !(ay->regs[7] & 0x40))
        return ay->port_a;
    if (r == 15 && !(ay->regs[7] & 0x80))
        return ay->port_b;
    return ay->regs[r];
}

static void env_advance(AY8910 *ay, int ticks)
{
    int period = ((ay->regs[12] << 8) | ay->regs[11]) * 2;
    if (period == 0)
        period = 2;
    if (ay->env_holding)
        return;
    ay->env_count += ticks;
    while (ay->env_count >= period) {
        ay->env_count -= period;
        if (--ay->env_step < 0) {
            if (ay->env_hold) {
                if (ay->env_alternate)
                    ay->env_attack ^= 0x0f;
                ay->env_holding = 1;
                ay->env_step = 0;
                return;
            }
            if (ay->env_alternate)
                ay->env_attack ^= 0x0f;
            ay->env_step = 15;
        }
    }
}

static void noise_advance(AY8910 *ay, int ticks)
{
    int period = (ay->regs[6] & 0x1f) * 2;
    if (period == 0)
        period = 2;
    ay->noise_count += ticks;
    while (ay->noise_count >= period) {
        ay->noise_count -= period;
        ay->rng ^= (((ay->rng & 1) ^ ((ay->rng >> 3) & 1)) << 17);
        ay->rng >>= 1;
    }
}

/* number of the `ticks` ticks during which tone channel ch is high */
static int tone_advance(AY8910 *ay, int ch, int ticks)
{
    int period = ((ay->regs[ch * 2 + 1] & 0x0f) << 8) | ay->regs[ch * 2];
    int cnt = ay->tone_count[ch], high = 0, step;
    uint8_t out = ay->tone_out[ch];
    if (period == 0)
        period = 1;
    if (cnt >= period)
        cnt = period - 1;
    while (ticks > 0) {
        step = period - cnt;
        if (step > ticks)
            step = ticks;
        if (out)
            high += step;
        cnt += step;
        ticks -= step;
        if (cnt >= period) {
            cnt = 0;
            out ^= 1;
        }
    }
    ay->tone_count[ch] = cnt;
    ay->tone_out[ch] = out;
    return high;
}

/* Renders generator by generator over the whole buffer (a slice is ~8 samples; the
   registers do not change during a call). Same result as advancing everything sample
   by sample: generators nobody listens to are advanced by the total in one step. */
#define AY_CHUNK 256

static void render_chunk(AY8910 *ay, int16_t *out, int count)
{
    static uint8_t tk[AY_CHUNK], nz[AY_CHUNK], ev[AY_CHUNK];
    static int32_t mix[AY_CHUNK];
    uint8_t mixer = ay->regs[7];
    int i, ch, total = 0, use_noise = 0, use_env = 0;

    for (i = 0; i < count; i++) {
        uint32_t acc = ay->tick_frac + ay->tick_step;
        int ticks = (int)(acc >> 16);
        ay->tick_frac = acc & 0xffff;
        if (ticks < 1)
            ticks = 1;
        if (ticks > 63)
            ticks = 63;
        tk[i] = (uint8_t)ticks;
        total += ticks;
        mix[i] = 0;
    }
    for (ch = 0; ch < 3; ch++) {
        if (ay->regs[8 + ch] & 0x10)
            use_env = 1;
        if (!(mixer & (0x08 << ch)))
            use_noise = 1;
    }
    if (use_noise) {
        for (i = 0; i < count; i++) {
            noise_advance(ay, tk[i]);
            nz[i] = (uint8_t)(ay->rng & 1);
        }
    } else {
        noise_advance(ay, total);
    }
    if (use_env) {
        for (i = 0; i < count; i++) {
            env_advance(ay, tk[i]);
            ev[i] = (uint8_t)((ay->env_step ^ ay->env_attack) & 0x0f);
        }
    } else {
        env_advance(ay, total);
    }

    for (ch = 0; ch < 3; ch++) {
        uint8_t vreg = ay->regs[8 + ch];
        int env = vreg & 0x10, tone_off = mixer & (0x01 << ch), noise_on = !(mixer & (0x08 << ch));
        int period = ((ay->regs[ch * 2 + 1] & 0x0f) << 8) | ay->regs[ch * 2];
        int cnt = ay->tone_count[ch];
        uint8_t o = ay->tone_out[ch];
        if (!env && vol_table[vreg & 0x0f] == 0) {
            tone_advance(ay, ch, total);                /* silent: only keep the phase */
            continue;
        }
        if (period == 0)
            period = 1;
        if (cnt >= period)
            cnt = period - 1;
        for (i = 0; i < count; i++) {
            int ticks = tk[i], high = 0, step;
            int32_t level = vol_table[env ? ev[i] : vreg & 0x0f];
            while (ticks > 0) {                         /* tone: ticks spent high */
                step = period - cnt;
                if (step > ticks)
                    step = ticks;
                if (o)
                    high += step;
                cnt += step;
                ticks -= step;
                if (cnt >= period) {
                    cnt = 0;
                    o ^= 1;
                }
            }
            if (noise_on && !nz[i])
                continue;                               /* noise enabled and low */
            if (tone_off)
                mix[i] += level;                        /* tone disabled: output stays high */
            else
                mix[i] += (int32_t)(((uint32_t)level * (uint32_t)high * recip[tk[i]]) >> 16);
        }
        ay->tone_count[ch] = cnt;
        ay->tone_out[ch] = o;
    }

    for (i = 0; i < count; i++) {
        /* remove the DC offset (the chip only outputs positive levels) */
        int32_t y = mix[i] - ay->dc_in + ay->dc_out - (ay->dc_out >> 8);
        ay->dc_in = mix[i];
        ay->dc_out = y;
        if (y > 32767) y = 32767;
        if (y < -32768) y = -32768;
        out[i] = (int16_t)y;
    }
}

void ay_render(AY8910 *ay, int16_t *out, int count)
{
    while (count > 0) {
        int n = count > AY_CHUNK ? AY_CHUNK : count;
        render_chunk(ay, out, n);
        out += n;
        count -= n;
    }
}
