#ifndef BITPACK_H
#define BITPACK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
Generic MSB-first bit packer/unpacker.
For width=3 this produces byte-for-byte the same stream as the old
hardcoded 8-symbols-into-3-bytes loop, so existing .glx files stay valid.

Width may vary per call (1..24 bits), so the same packer works for the
fixed-width quantizer symbols today and variable-length huffman codes later.

State persists across calls: keep ONE BitPacker alive for the whole file
(same idea as the LFSR seed pointer), because packet boundaries generally
do not land on byte boundaries once target_bits != 3.
*/

typedef struct {
    uint32_t bitbuf;   // pending bits, right-aligned
    int      bitcount; // number of pending bits, 0..7 between calls
} BitPacker;

typedef struct {
    uint32_t      bitbuf;
    int           bitcount;
    const uint8_t *data;   // current input chunk
    size_t        len;
    size_t        pos;
} BitUnpacker;

/* --- packer (implemented in sampling.c) --- */

// Packs the low `width` bits of `symbol`. Any completed bytes are written
// to `out` (caller-owned, at least (width/8)+1 bytes). Returns number of
// bytes written this call.
size_t bitpack_write(BitPacker *bp, uint32_t symbol, int width, uint8_t *out, size_t out_cap);

// End of stream: writes the final partial byte (zero-padded on the right),
// if any. Returns number of bytes written (0 or 1). Resets the packer.
size_t bitpack_flush(BitPacker *bp, uint8_t *out, size_t out_cap);

/* --- unpacker (implemented in decompress.c) --- */

// Point the unpacker at its first input chunk.
void bitunpack_init(BitUnpacker *bu, const uint8_t *data, size_t len);

// Continue with the next input chunk WITHOUT dropping buffered bits
// (for streaming reads, e.g. fread 4096 bytes at a time).
void bitunpack_feed(BitUnpacker *bu, const uint8_t *data, size_t len);

// Reads the next `width`-bit symbol into *symbol_out. Returns false when
// fewer than `width` bits remain (end of chunk: feed more, or stop —
// leftover bits are the flush padding, not a symbol).
bool bitunpack_read(BitUnpacker *bu, int width, uint32_t *symbol_out);

#endif /* BITPACK_H */
