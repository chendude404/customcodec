#include "mulaw.h"
#include <math.h>

/* ── PCM conversion ─────────────────────────────────────────────────── */

float glx_pcm16_to_float(int16_t s)
{
    /* Divide by 32768 so the full int16 range maps into [-1, 1).
     * (+32767 → 0.99997, -32768 → -1.0 exactly.) */
    return (float)s / 32768.0f;
}

int16_t glx_float_to_pcm16(float x)
{
    float y = x * 32768.0f;
    if (y >  32767.0f) y =  32767.0f;
    if (y < -32768.0f) y = -32768.0f;
    /* round-to-nearest, ties away from zero */
    return (int16_t)(y >= 0.0f ? y + 0.5f : y - 0.5f);
}

/* ── µ-law ──────────────────────────────────────────────────────────── */

float glx_ulaw_compress(float x)
{
    /* sign(x) * ln(1 + µ|x|) / ln(1 + µ)  — log1pf for stability near 0 */
    float s = (x < 0.0f) ? -1.0f : 1.0f;
    return s * log1pf(GLX_MU * fabsf(x)) / log1pf(GLX_MU);
}

float glx_ulaw_expand(float y)
{
    /* sign(y) * ((1+µ)^|y| - 1) / µ  — expm1f is the stable form */
    float s = (y < 0.0f) ? -1.0f : 1.0f;
    return s * expm1f(fabsf(y) * log1pf(GLX_MU)) / GLX_MU;
}

/* ── Headroom ───────────────────────────────────────────────────────── */

static float glx_delta(int bitdepth)
{
    return 2.0f / (float)(1u << bitdepth);
}

float glx_headroom_factor(float alpha, int bitdepth)
{
    /* Class-2 dither peaks at ±alpha*Delta/2, so this is the minimal
     * shrink that keeps compress(x)*hrf + dither inside [-1, 1].
     * Scales with alpha: at alpha=0 no headroom is charged at all. */
    return 1.0f - alpha * glx_delta(bitdepth) * 0.5f;
}

/* ── Dither (Galois LFSR, deterministic) ────────────────────────────── */

#define GLX_LFSR_MASK 0xB5CA369Cu

static uint32_t lfsr_next(glx_dither_state *st)
{
    /* Galois form: shift right, XOR the tap mask when the LSB was set. */
    uint32_t lsb = st->lfsr & 1u;
    st->lfsr >>= 1;
    if (lsb) st->lfsr ^= GLX_LFSR_MASK;
    return st->lfsr;
}

/* uniform float in [0, 1) from one LFSR step */
static float lfsr_unif(glx_dither_state *st)
{
    return (float)lfsr_next(st) * (1.0f / 4294967296.0f);
}

void glx_dither_init(glx_dither_state *st, uint32_t seed)
{
    st->lfsr = (seed != 0u) ? seed : 0xDEADBEEFu;  /* LFSR must not be 0 */
}

float glx_dither_next(glx_dither_state *st, float alpha, int bitdepth)
{
    /* Fixed draw order — R then mask — so decoder replays identically. */
    float Delta = glx_delta(bitdepth);
    float R     = (lfsr_unif(st) - 0.5f) * Delta;      /* RPDF, ±Delta/2 */
    float mask  = (lfsr_unif(st) < alpha) ? 1.0f : 0.0f;
    return alpha * R * mask;                           /* class 2 */
}

/* ── Quantizer ──────────────────────────────────────────────────────── */

uint8_t glx_quantize(float x, int bitdepth)
{
    float Delta = glx_delta(bitdepth);
    int   nlev  = 1 << bitdepth;
    int   code  = (int)floorf((x + 1.0f) / Delta);
    if (code < 0)     code = 0;
    if (code >= nlev) code = nlev - 1;
    return (uint8_t)code;
}

float glx_dequantize(uint8_t code, int bitdepth)
{
    float Delta = glx_delta(bitdepth);
    return -1.0f + ((float)code + 0.5f) * Delta;   /* midrise level center */
}

/* ── Full pipeline ──────────────────────────────────────────────────── */

int glx_encode(const int16_t *pcm, size_t n,
               uint8_t *codes,
               int bitdepth, float alpha, uint32_t seed)
{
    if (!pcm || !codes || bitdepth < 1 || bitdepth > 8 ||
        alpha < 0.0f || alpha > 1.0f)
        return -1;

    glx_dither_state st;
    glx_dither_init(&st, seed);
    float hrf = glx_headroom_factor(alpha, bitdepth);

    for (size_t i = 0; i < n; i++) {
        float x = glx_pcm16_to_float(pcm[i]);
        float c = glx_ulaw_compress(x) * hrf;            /* compand + headroom */
        float d = glx_dither_next(&st, alpha, bitdepth); /* class-2 dither    */
        codes[i] = glx_quantize(c + d, bitdepth);
    }
    return 0;
}

int glx_decode(const uint8_t *codes, size_t n,
               int16_t *pcm,
               int bitdepth, float alpha, uint32_t seed)
{
    if (!codes || !pcm || bitdepth < 1 || bitdepth > 8 ||
        alpha < 0.0f || alpha > 1.0f)
        return -1;

    glx_dither_state st;
    glx_dither_init(&st, seed);                 /* same seed → same stream */
    float hrf = glx_headroom_factor(alpha, bitdepth);

    for (size_t i = 0; i < n; i++) {
        float q = glx_dequantize(codes[i], bitdepth);
        float d = glx_dither_next(&st, alpha, bitdepth);
        float c = (q - d) / hrf;                /* subtract, undo headroom */
        pcm[i]  = glx_float_to_pcm16(glx_ulaw_expand(c));
    }
    return 0;
}

/* ── Delta transform for the entropy-coding stage ───────────────────── */

void glx_delta_encode(const uint8_t *codes, size_t n, int8_t *deltas)
{
    if (n == 0) return;
    deltas[0] = (int8_t)codes[0];               /* codes fit in 8 bits */
    for (size_t i = 1; i < n; i++)
        deltas[i] = (int8_t)((int)codes[i] - (int)codes[i - 1]);
}

void glx_delta_decode(const int8_t *deltas, size_t n, uint8_t *codes)
{
    if (n == 0) return;
    int acc = deltas[0];
    codes[0] = (uint8_t)acc;
    for (size_t i = 1; i < n; i++) {
        acc += deltas[i];
        codes[i] = (uint8_t)acc;
    }
}
