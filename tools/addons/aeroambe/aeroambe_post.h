/* Post-filter for the Aero AMBE vocoder output (8 kHz, 16-bit): what mbelib's synthesis leaves that the ear does not want.
 * - high-pass 250 Hz (2nd order): rumble and DC from the synthesis;
 * - low-pass 3400 Hz (4th order): the hiss of the random-phase unvoiced synthesis above the telephone band;
 * - slow AGC (+9.5 dB at most, 10 ms attack, 800 ms release, frozen below -45 dBFS so silence is not pumped up) and a soft limiter.
 * Shared by the OnAir plug-in (onair-codec-aeroambe.c) and the decode_bits tool: same sound live and in the A/B files. */
#ifndef AEROAMBE_POST_H
#define AEROAMBE_POST_H
#include <math.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct { double b0, b1, b2, a1, a2, z1, z2; } aeropost_biquad;
typedef struct { aeropost_biquad hp, lp1, lp2; double env, gain; } aeropost;

static void aeropost_biquad_set(aeropost_biquad* q, double fs, double fc, int highpass) {
    const double w = 2.0 * M_PI * fc / fs, cw = cos(w), sw = sin(w), alpha = sw / (2.0 * 0.70710678);
    const double a0 = 1.0 + alpha;
    if (highpass) { q->b0 = (1.0 + cw) / 2.0 / a0; q->b1 = -(1.0 + cw) / a0; q->b2 = q->b0; }
    else { q->b0 = (1.0 - cw) / 2.0 / a0; q->b1 = (1.0 - cw) / a0; q->b2 = q->b0; }
    q->a1 = -2.0 * cw / a0; q->a2 = (1.0 - alpha) / a0;
    q->z1 = q->z2 = 0.0;
}
static inline double aeropost_biquad_run(aeropost_biquad* q, double x) {   /* transposed direct form II */
    const double y = q->b0 * x + q->z1;
    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}
static void aeropost_init(aeropost* p) {
    aeropost_biquad_set(&p->hp, 8000.0, 250.0, 1);
    aeropost_biquad_set(&p->lp1, 8000.0, 3400.0, 0);
    aeropost_biquad_set(&p->lp2, 8000.0, 3400.0, 0);
    p->env = 0.0; p->gain = 1.0;
}
static void aeropost_run(aeropost* p, int16_t* pcm, int n) {
    const double target = 0.35, gmax = 3.0, gate = 0.0056;               /* -9 dBFS peak envelope, +9.5 dB, -45 dBFS */
    const double att = 1.0 - exp(-1.0 / (8000.0 * 0.010)), rel = 1.0 - exp(-1.0 / (8000.0 * 0.800));
    for (int i = 0; i < n; i++) {
        double x = pcm[i] / 32768.0;
        x = aeropost_biquad_run(&p->hp, x);
        x = aeropost_biquad_run(&p->lp2, aeropost_biquad_run(&p->lp1, x));
        const double a = fabs(x);
        if (a > gate) p->env += (a > p->env ? att : rel) * (a - p->env);  /* silence leaves the envelope (and the gain) where it is */
        double g = p->env > 1e-6 ? target / p->env : gmax;
        if (g > gmax) g = gmax;
        if (g < 1.0) g = 1.0;
        p->gain += (g < p->gain ? 0.01 : 0.0005) * (g - p->gain);       /* gain falls fast, rises slowly */
        double y = x * p->gain;
        if (y > 0.7) y = 0.7 + 0.3 * tanh((y - 0.7) / 0.3);              /* soft knee above -3 dBFS */
        else if (y < -0.7) y = -0.7 + 0.3 * tanh((y + 0.7) / 0.3);
        pcm[i] = (int16_t)lrint(y * 32767.0);
    }
}
#endif
