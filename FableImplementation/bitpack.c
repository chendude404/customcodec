#include "glx.h"

/*
Generic MSB-first bit packer/unpacker.
Packer refactored from ../sampling.c, unpacker from ../decompress.c —
merged into one translation unit since they are two halves of one format.

Width may vary per call (1..24 bits), so the same packer works for the
fixed-width delta symbols today and variable-length huffman codes later.

State persists across calls: keep ONE BitPacker/BitUnpacker alive for the
whole stream, because symbol boundaries generally do not land on byte
boundaries.
*/

/* ── Packer ─────────────────────────────────────────────────────────── */

size_t bitpack_write(BitPacker *bp, uint32_t symbol, int width, uint8_t *out, size_t out_cap)
{
    if (width <= 0 || width > 24) return 0;  // keeps bitbuf within uint32_t: 7 leftover + 24 new = 31 bits max

    bp->bitbuf = (bp->bitbuf << width) | (symbol & ((1u << width) - 1u));
    bp->bitcount += width;

    size_t written = 0;
    while (bp->bitcount >= 8 && written < out_cap) {
        bp->bitcount -= 8;
        out[written++] = (uint8_t)(bp->bitbuf >> bp->bitcount);
    }
    return written;
}

size_t bitpack_flush(BitPacker *bp, uint8_t *out, size_t out_cap)
{
    if (bp->bitcount == 0) return 0;
    if (out_cap == 0) return 0;

    out[0] = (uint8_t)(bp->bitbuf << (8 - bp->bitcount)); // pad bits are zeros on the right
    bp->bitbuf = 0;
    bp->bitcount = 0;
    return 1;
}

/* ── Unpacker ───────────────────────────────────────────────────────── */

void bitunpack_init(BitUnpacker *bu, const uint8_t *data, size_t len)
{
    bu->bitbuf = 0;
    bu->bitcount = 0;
    bitunpack_feed(bu, data, len);
}

void bitunpack_feed(BitUnpacker *bu, const uint8_t *data, size_t len)
{
    bu->data = data;
    bu->len = len;
    bu->pos = 0;
}

bool bitunpack_read(BitUnpacker *bu, int width, uint32_t *symbol_out)
{
    if (width <= 0 || width > 24) return false;

    while (bu->bitcount < width) {
        if (bu->pos >= bu->len) return false;  // starved — buffered bits are kept for the next feed
        bu->bitbuf = (bu->bitbuf << 8) | bu->data[bu->pos++];
        bu->bitcount += 8;
    }

    bu->bitcount -= width;
    *symbol_out = (bu->bitbuf >> bu->bitcount) & ((1u << width) - 1u);
    return true;
}
