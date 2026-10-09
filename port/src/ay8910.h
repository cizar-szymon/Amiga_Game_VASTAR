/* Vastar port - AY-3-8910 sound chip */
#ifndef AY8910_H
#define AY8910_H

#include <stdint.h>

typedef struct AY8910 {
    uint8_t regs[16];
    uint8_t latch;                 /* selected register */
    uint8_t port_a, port_b;        /* values seen on the I/O ports when they are inputs */

    /* generators, time unit = 1 "tick" = 8 chip clocks */
    int tone_count[3];
    uint8_t tone_out[3];
    int noise_count;
    uint32_t rng;
    int env_count;
    int env_step;
    uint8_t env_attack, env_hold, env_alternate, env_holding;

    uint32_t tick_step;            /* ticks per output sample, 16.16 fixed point */
    uint32_t tick_frac;
    int32_t dc_in, dc_out;         /* DC blocker state */
} AY8910;

void ay_init(AY8910 *ay, uint32_t clock, uint32_t sample_rate);
void ay_reset(AY8910 *ay);
void ay_address_w(AY8910 *ay, uint8_t v);
void ay_data_w(AY8910 *ay, uint8_t v);
uint8_t ay_data_r(AY8910 *ay);

/* render mono signed 16 bit samples */
void ay_render(AY8910 *ay, int16_t *out, int count);

#endif
