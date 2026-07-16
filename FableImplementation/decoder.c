#include "glx.h"
#include "compression.h"
#include <stdlib.h>
#include <string.h>

/*
GLX DECODER
===========
Usage: ./glxdecode.exe in.glx out.wav

Pipeline: read .glx -> decompress (bit-unpack + delta decode)
          -> subtractive dither -> [mu-law expand] -> write 16 kHz .wav

BUFFERED: mirrors the encoder's packet loop — the payload is pulled through
a small file buffer and decoded one 10 ms packet (160 samples) at a time
into fixed stack buffers, and the WAV is written incrementally. Neither
the .glx payload nor the PCM output is ever whole in RAM. The dither LFSR,
delta predictor and bit unpacker carry across packets, so reconstruction
is identical to the old whole-file pass.
*/

/* payload file buffer: plenty for one packet's worth of coded symbols */
#define GLX_IOBUF_BYTES 512

static int read_glx_header(FILE *fp, GlxHeader *h)
{
    if (fread(h->magic, 1, 4, fp) != 4 ||
        fread(&h->sampleRate, sizeof(uint32_t), 1, fp) != 1 ||
        fread(&h->bitsPerSym, sizeof(uint8_t), 1, fp) != 1 ||
        fread(&h->alphaIdx, sizeof(uint8_t), 1, fp) != 1 ||
        fread(&h->mulaw, sizeof(uint8_t), 1, fp) != 1 ||
        fread(&h->huff, sizeof(uint8_t), 1, fp) != 1 ||
        fread(&h->ditherType, sizeof(uint8_t), 1, fp) != 1 ||
        fread(&h->seed, sizeof(uint32_t), 1, fp) != 1 ||
        fread(&h->numPackets, sizeof(uint32_t), 1, fp) != 1)
        return -1;
    return 0;
}

/* Top the unpacker up to at least `needbits` buffered bits (enough for one
 * symbol) before decoding, because a mid-symbol starve inside the Huffman
 * walker would lose the bits it already consumed. Unread buffer bytes are
 * slid to the front and the tail refilled from the file; at EOF the
 * unpacker just runs down to whatever bits remain. */
static void glx_refill(BitUnpacker *bu, uint8_t *buf, size_t cap, FILE *fp,
                       int needbits)
{
    while (bu->bitcount + 8 * (long)(bu->len - bu->pos) < needbits) {
        size_t left = bu->len - bu->pos;
        memmove(buf, bu->data + bu->pos, left);
        size_t got = fread(buf + left, 1, cap - left, fp);
        bitunpack_feed(bu, buf, left + got);
        if (got == 0) break;   /* EOF */
    }
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        printf("usage: %s in.glx out.wav\n", argv[0]);
        return -1;
    }

    FILE *in = fopen(argv[1], "rb");
    if (!in) {
        printf("cannot open %s\n", argv[1]);
        return -1;
    }

    GlxHeader h;
    if (read_glx_header(in, &h) != 0 || memcmp(h.magic, GLX_MAGIC, 4) != 0) {
        printf("not a GLX1 file\n");
        fclose(in);
        return -1;
    }
    printf("rate=%u bits=%u alpha=%u mulaw=%u huff=%u dither=%u seed=%u packets=%u\n",
           h.sampleRate, h.bitsPerSym, h.alphaIdx, h.mulaw, h.huff,
           h.ditherType, h.seed, h.numPackets);

    if (h.bitsPerSym < GLX_BITS_MIN || h.bitsPerSym > GLX_BITS_MAX) {
        printf("unsupported bit depth %u (must be %d..%d)\n",
               h.bitsPerSym, GLX_BITS_MIN, GLX_BITS_MAX);
        fclose(in);
        return -1;
    }

    /* If Huffman-coded, the file carries its own table: read the embedded
     * length block and rebuild the canonical table from it (self-describing). */
    GlxHuffTable htab;
    if (h.huff) {
        uint8_t tbl[GLX_HUFF_TABLE_BYTES], hlen[GLX_HUFF_NSYM];
        if (fread(tbl, 1, sizeof tbl, in) != sizeof tbl) {
            printf("truncated Huffman table\n");
            fclose(in);
            return -1;
        }
        glx_huff_unpack_lengths(tbl, hlen);
        if (glx_huff_build(&htab, hlen) != 0) {
            printf("invalid embedded Huffman table\n");
            fclose(in);
            return -1;
        }
    }

    /* Step 1: open the output wav — samples are appended per packet and the
     * header sizes patched on close */
    WavWriter ww;
    if (wav_writer_open(&ww, argv[2], h.sampleRate) != 0) {
        fclose(in);
        return -1;
    }

    /* Step 2: packet loop — unpack 160 delta symbols, delta decode,
     * dequantize -> subtract dither -> [mu-law expand] -> pcm16, append to
     * the wav. All state carries across packets, mirroring the encoder. */
    float alpha = glx_alpha_from_idx(h.alphaIdx);
    int   rawWidth = GLX_DELTA_WIDTH(h.bitsPerSym);
    int   rawHalf  = 1 << h.bitsPerSym;   /* two's-complement fold point */
    /* one symbol never needs more bits than the longest Huffman code */
    int   needbits = h.huff ? GLX_HUFF_MAXLEN : rawWidth;

    glx_dither_state st;
    glx_dither_init(&st, h.seed);
    int prev = -1;

    uint8_t iobuf[GLX_IOBUF_BYTES];
    BitUnpacker bu;
    bitunpack_init(&bu, iobuf, 0);

    int8_t  deltas[GLX_FRAMES_PER_PACKET];
    uint8_t codes[GLX_FRAMES_PER_PACKET];
    int16_t pcm[GLX_FRAMES_PER_PACKET];

    for (uint32_t p = 0; p < h.numPackets; p++) {
        for (size_t i = 0; i < GLX_FRAMES_PER_PACKET; i++) {
            glx_refill(&bu, iobuf, sizeof iobuf, in, needbits);

            uint32_t sym;
            bool ok = h.huff ? glx_huff_decode(&bu, &htab, &sym)
                             : bitunpack_read(&bu, rawWidth, &sym);
            if (!ok) {
                printf("truncated payload: got %zu of %zu symbols\n",
                       (size_t)p * GLX_FRAMES_PER_PACKET + i,
                       (size_t)h.numPackets * GLX_FRAMES_PER_PACKET);
                fclose(in);
                wav_writer_close(&ww);
                return -1;
            }
            /* huff: sym is a biased index (delta+BIAS); raw: two's complement */
            deltas[i] = h.huff
                ? (int8_t)((int)sym - GLX_HUFF_BIAS)
                : (int8_t)((int)sym >= rawHalf ? (int)sym - 2 * rawHalf : (int)sym);
        }

        glx_delta_decode(deltas, GLX_FRAMES_PER_PACKET, codes, &prev);

        if (glx_decode(codes, GLX_FRAMES_PER_PACKET, pcm,
                       h.bitsPerSym, alpha, &st, h.mulaw, h.ditherType) != 0) {
            printf("Decode failed (bad header parameters)\n");
            fclose(in);
            wav_writer_close(&ww);
            return -1;
        }

        if (wav_writer_write(&ww, pcm, GLX_FRAMES_PER_PACKET) != 0) {
            printf("write error on %s\n", argv[2]);
            fclose(in);
            wav_writer_close(&ww);
            return -1;
        }
    }
    fclose(in);

    if (wav_writer_close(&ww) != 0) {
        printf("could not finalize %s\n", argv[2]);
        return -1;
    }

    size_t n = (size_t)h.numPackets * GLX_FRAMES_PER_PACKET;
    printf("wrote %zu samples (%.2f s) to %s\n",
           n, (double)n / h.sampleRate, argv[2]);
    return EXIT_SUCCESS;
}
