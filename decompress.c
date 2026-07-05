#include "bitpack.h"

/*
DECOMPRESSION SIDE
Mirror of the bit packer in sampling.c: MSB-first, width 1..24 per call.
The decoder should read exactly the symbol count recorded in the header,
then ignore whatever pad bits remain in the last byte.
*/

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

//decompress() -> huffman stage, later
