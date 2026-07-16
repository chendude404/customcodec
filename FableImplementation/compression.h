#ifndef GLX_COMPRESSION_H
#define GLX_COMPRESSION_H

#include "glx.h"

/*
Entropy coding for the .glx codec (delta stream -> canonical Huffman).

Alphabet: the 15 delta values in [-7, +7]; symbol index = delta + GLX_HUFF_BIAS.
The table is fully described by its per-symbol code lengths — canonical codes and
the decode tables are rebuilt from lengths by glx_huff_build() on BOTH sides. So
a .glx can carry its own 8-byte length block and be self-describing: the decoder
never needs a table baked in at compile time.

Encoder path: pick designed lengths via glx_huff_lengths_for(bitdepth, mulaw)
-> build -> embed with glx_huff_pack_lengths -> encode each symbol.
Decoder path: unpack the embedded lengths -> build -> decode each symbol.
*/

#define GLX_HUFF_NSYM        15
#define GLX_HUFF_BIAS         7    /* symbol index = delta + BIAS */
#define GLX_HUFF_MAXLEN      15    /* lengths ride a 4-bit field; natural max 14 */
#define GLX_HUFF_TABLE_BYTES  8    /* 15 nibble lengths packed, MSB-first */

typedef struct {
    uint8_t  len[GLX_HUFF_NSYM];              /* code length per symbol */
    uint16_t code[GLX_HUFF_NSYM];             /* canonical code per symbol */
    uint16_t count[GLX_HUFF_MAXLEN + 1];      /* #codes of each length */
    uint8_t  symbols[GLX_HUFF_NSYM];          /* symbols sorted by (len, sym) */
    int      maxlen;
} GlxHuffTable;

/* Reconstruct canonical codes + decode tables from code lengths.
 * Returns 0 on success, -1 if the lengths are not a complete prefix code. */
int glx_huff_build(GlxHuffTable *t, const uint8_t len[GLX_HUFF_NSYM]);

/* The designed static length table for a bit depth (GLX_BITS_MIN..MAX) and
 * companding mode (mulaw 0 or 1). The alphabet is always the full 15 symbols
 * — at lower depths the outer deltas just never occur — so the embedded
 * table block stays 8 bytes for every depth. Returns NULL if bitdepth is
 * out of range. */
const uint8_t *glx_huff_lengths_for(int bitdepth, int mulaw);

/* Wire (de)serialization of the length table: 15 nibbles <-> 8 bytes. */
void glx_huff_pack_lengths(const uint8_t len[GLX_HUFF_NSYM],
                           uint8_t out[GLX_HUFF_TABLE_BYTES]);
void glx_huff_unpack_lengths(const uint8_t in[GLX_HUFF_TABLE_BYTES],
                             uint8_t len[GLX_HUFF_NSYM]);

/* Encode one symbol index (delta + BIAS); returns bytes emitted into `out`. */
size_t glx_huff_encode(BitPacker *bp, const GlxHuffTable *t, uint32_t sym,
                       uint8_t *out, size_t cap);

/* Decode one symbol index; false only if the bit stream is starved. */
bool glx_huff_decode(BitUnpacker *bu, const GlxHuffTable *t, uint32_t *sym_out);

#endif /* GLX_COMPRESSION_H */
