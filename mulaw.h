#ifndef GLX_MULAW_H
#define GLX_MULAW_H

#include <stdint.h>
#include <stddef.h>

/* ── µ-law companding (float, µ = 255) ──────────────────────────────── */
#define GLX_MU 255.0f

/* 16-bit PCM  →  float in [-1, 1) */
float glx_pcm16_to_float(int16_t s);

/* float in [-1, 1]  →  16-bit PCM (with clamp + round) */
int16_t glx_float_to_pcm16(float x);

/* µ-law compress / expand, x,y in [-1, 1] */
float glx_ulaw_compress(float x);
float glx_ulaw_expand(float y);

/* ── Headroom ───────────────────────────────────────────────────────── */
/* Minimal headroom for class-2 (masked+scaled) dither:
 *   hrf = 1 - alpha * Delta / 2,   Delta = 2 / 2^bitdepth
 * Guarantees compress(x)*hrf + dither stays inside [-1, 1]. */
float glx_headroom_factor(float alpha, int bitdepth);

/* ── Deterministic dither source (Galois LFSR) ──────────────────────── */
/* Same dither stream must be regenerated at the decoder for subtractive
 * dithering, so the generator is an explicit, seedable state — no rand(). */
typedef struct {
    uint32_t lfsr;   /* must be nonzero */
} glx_dither_state;

void glx_dither_init(glx_dither_state *st, uint32_t seed);

/* One class-2 dither sample: alpha * R * mask,
 *   R    = uniform(-Delta/2, +Delta/2)
 *   mask = (uniform(0,1) < alpha)
 * Consumes exactly two LFSR draws per call (fixed order: R first, mask
 * second) so encoder and decoder stay in lockstep. */
float glx_dither_next(glx_dither_state *st, float alpha, int bitdepth);

/* ── Quantizer ──────────────────────────────────────────────────────── */
/* Midrise uniform quantizer on [-1, 1):
 *   code  = floor((x + 1) / Delta), clamped to [0, 2^bitdepth - 1]
 *   level = -1 + (code + 0.5) * Delta
 * Codes are small unsigned ints — ready for delta coding downstream. */
uint8_t glx_quantize(float x, int bitdepth);
float   glx_dequantize(uint8_t code, int bitdepth);

/* ── Buffer pipeline ────────────────────────────────────────────────── */
/* Encode: pcm16 → normalize → µ-law → *hrf → +dither → quantize.
 * Writes n quantizer codes into `codes` (caller-allocated, length n).
 * Returns 0 on success, -1 on bad args. */
int glx_encode(const int16_t *pcm, size_t n,
               uint8_t *codes,
               int bitdepth, float alpha, uint32_t seed);

/* Decode: dequantize → -dither → /hrf → µ-law expand → pcm16.
 * Must be called with the same bitdepth/alpha/seed as glx_encode. */
int glx_decode(const uint8_t *codes, size_t n,
               int16_t *pcm,
               int bitdepth, float alpha, uint32_t seed);

/* In-place delta transform on the code array (first element kept as-is,
 * rest become successive differences, stored as int8_t). */
void glx_delta_encode(const uint8_t *codes, size_t n, int8_t *deltas);
void glx_delta_decode(const int8_t *deltas, size_t n, uint8_t *codes);

#endif /* GLX_MULAW_H */
