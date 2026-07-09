#ifndef GLX_H
#define GLX_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

/* ── Codec constants ────────────────────────────────────────────────── */

#define GLX_MAGIC             "GLX1"
#define GLX_MU                255.0f
#define LNMU                  5.54517744 //precomupted natural log
#define GLX_BITS              3       /* quantizer bit depth */
#define GLX_IN_RATE           48000u  /* design decision: 48 kHz input only */
#define GLX_OUT_RATE          16000u
#define GLX_DECIMATION        3       /* 48 kHz -> 16 kHz */
#define GLX_FRAMES_PER_PACKET 160     /* 10 ms at 16 kHz */
#define GLX_DELTA_WIDTH       4       /* signed 4-bit deltas on the wire */

/* Dither PDF selector (CLI arg + header field) */
#define GLX_DITHER_MASKED     1       /* alpha*Pi_aD(v) + (1-alpha)*delta(v) */
#define GLX_DITHER_SPIKED     2       /* alpha*Pi_aD(v) + ((1-alpha)/2)*
                                       * [delta(v - aD/2) + delta(v + aD/2)] */

/* ── .glx header (21 bytes, little-endian) ──────────────────────────── */

#pragma pack(push, 1)
typedef struct GlxHeader {
    char     magic[4];    /* "GLX1" */
    uint32_t sampleRate;  /* output rate (16000) */
    uint8_t  bitsPerSym;  /* 3 */
    uint8_t  alphaIdx;    /* alpha index: 1 -> 0.0, 2 -> 0.1, ... 11 -> 1.0 */
    uint8_t  mulaw;       /* 1 for mu-law companding, 0 for linear */
    uint8_t  huff;        /* 1 = payload is Huffman-coded deltas, 0 = raw 4-bit */
    uint8_t  ditherType;  /* GLX_DITHER_MASKED (1) or GLX_DITHER_SPIKED (2) */
    uint32_t seed;        /* dither LFSR seed */
    uint32_t numPackets;  /* total 10 ms packets (160 samples each) */
} GlxHeader;
#pragma pack(pop)

/* ── WAV I/O (wav.c) ────────────────────────────────────────────────── */

/* Reads a 16-bit PCM WAV, downmixing stereo to mono. Returns a malloc'd
 * sample buffer (caller frees) or NULL on error; fills rate and count. */
int16_t *wav_read_mono16(const char *path, uint32_t *rate_out, size_t *n_out);

/* Writes a mono 16-bit PCM WAV. Returns 0 on success, -1 on error. */
int wav_write_mono16(const char *path, const int16_t *pcm, size_t n, uint32_t rate);

/* ── Companding + dither + quantizer pipeline (mulaw.c) ─────────────── */

float   glx_pcm16_to_float(int16_t s);
int16_t glx_float_to_pcm16(float x);

float glx_ulaw_compress(float x);
float glx_ulaw_expand(float y);

/* Minimal headroom for the dither: both PDFs peak at ±alpha*Delta/2, so
 *   hrf = 1 - alpha * Delta / 2,   Delta = 2 / 2^bitdepth */
float glx_headroom_factor(float alpha, int bitdepth);

/* Maps the CLI/header alpha index to the dither amplitude in [0, 1].
 * Same convention as the old threshol_lut: 1 -> 0.0 ... 11+ -> 1.0. */
float glx_alpha_from_idx(int alphaIdx);

/* Deterministic dither source: same seed regenerates the same stream at
 * the decoder for subtractive dithering. */
typedef struct {
    uint32_t lfsr;   /* must be nonzero */
} glx_dither_state;

/* One dither sample. ditherType selects the PDF:
 *   GLX_DITHER_MASKED: alpha*Pi_aD(v) + (1-alpha)*delta(v)
 *   GLX_DITHER_SPIKED: alpha*Pi_aD(v)
 *                      + ((1-alpha)/2) * [delta(v - aD/2) + delta(v + aD/2)]
 * Both types consume exactly two LFSR draws per sample, so encoder and
 * decoder stay in lockstep for either choice. */
void  glx_dither_init(glx_dither_state *st, uint32_t seed);
float glx_dither_next(glx_dither_state *st, float alpha, int bitdepth, int ditherType);

uint8_t glx_quantize(float x, int bitdepth);
float   glx_dequantize(uint8_t code, int bitdepth);

/* Encode: pcm16 -> normalize -> [mu-law if mulaw] -> *hrf -> +dither -> quantize.
 * Decode: dequantize -> -dither -> /hrf -> [mu-law expand if mulaw] -> pcm16.
 * Must be called with the same bitdepth/alpha/seed/mulaw/ditherType on both sides. */
int glx_encode(const int16_t *pcm, size_t n, uint8_t *codes,
               int bitdepth, float alpha, uint32_t seed, int mulaw, int ditherType);
int glx_decode(const uint8_t *codes, size_t n, int16_t *pcm,
               int bitdepth, float alpha, uint32_t seed, int mulaw, int ditherType);

/* Delta transform for the compression stage (first element kept as-is,
 * rest become successive differences). */
void glx_delta_encode(const uint8_t *codes, size_t n, int8_t *deltas);
void glx_delta_decode(const int8_t *deltas, size_t n, uint8_t *codes);

/* ── Generic MSB-first bit packer / unpacker (bitpack.c) ────────────── */

typedef struct {
    uint32_t bitbuf;   /* pending bits, right-aligned */
    int      bitcount; /* number of pending bits, 0..7 between calls */
} BitPacker;

typedef struct {
    uint32_t       bitbuf;
    int            bitcount;
    const uint8_t *data;
    size_t         len;
    size_t         pos;
} BitUnpacker;

size_t bitpack_write(BitPacker *bp, uint32_t symbol, int width, uint8_t *out, size_t out_cap);
size_t bitpack_flush(BitPacker *bp, uint8_t *out, size_t out_cap);

void bitunpack_init(BitUnpacker *bu, const uint8_t *data, size_t len);
void bitunpack_feed(BitUnpacker *bu, const uint8_t *data, size_t len);
bool bitunpack_read(BitUnpacker *bu, int width, uint32_t *symbol_out);

#endif /* GLX_H */
