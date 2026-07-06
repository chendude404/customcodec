#include "glx.h"
#include <stdlib.h>
#include <string.h>

/*
GLX DECODER
===========
Usage: ./glxdecode.exe in.glx out.wav

Pipeline: read .glx -> decompress (bit-unpack + delta decode)
          -> subtractive dither -> [mu-law expand] -> write 16 kHz .wav

Refactored from ../glxdecode.c: the header layout and WAV output are the
same; the symbol stream is now 4-bit signed deltas of the 3-bit quantizer
codes, and reconstruction runs through the float pipeline in mulaw.c so it
mirrors the encoder exactly (same LFSR draws, same headroom factor).
*/

static int read_glx_header(FILE *fp, GlxHeader *h)
{
    if (fread(h->magic, 1, 4, fp) != 4 ||
        fread(&h->sampleRate, sizeof(uint32_t), 1, fp) != 1 ||
        fread(&h->bitsPerSym, sizeof(uint8_t), 1, fp) != 1 ||
        fread(&h->alphaIdx, sizeof(uint8_t), 1, fp) != 1 ||
        fread(&h->mulaw, sizeof(uint8_t), 1, fp) != 1 ||
        fread(&h->seed, sizeof(uint32_t), 1, fp) != 1 ||
        fread(&h->numPackets, sizeof(uint32_t), 1, fp) != 1)
        return -1;
    return 0;
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
    printf("rate=%u bits=%u alpha=%u mulaw=%u seed=%u packets=%u\n",
           h.sampleRate, h.bitsPerSym, h.alphaIdx, h.mulaw, h.seed, h.numPackets);

    size_t n = (size_t)h.numPackets * GLX_FRAMES_PER_PACKET;

    /* Step 1: decompress — read the packed payload, unpack 4-bit signed
     * deltas, then undo the delta transform to recover quantizer codes */
    size_t payload_cap = (n * GLX_DELTA_WIDTH + 7) / 8 + 1;
    uint8_t *payload = malloc(payload_cap);
    int8_t  *deltas  = malloc(n);
    uint8_t *codes   = malloc(n);
    int16_t *pcm     = malloc(n * sizeof(int16_t));
    if (!payload || !deltas || !codes || !pcm) {
        printf("Out of memory\n");
        return -1;
    }
    size_t payload_len = fread(payload, 1, payload_cap, in);
    fclose(in);

    BitUnpacker bu;
    bitunpack_init(&bu, payload, payload_len);
    for (size_t i = 0; i < n; i++) {
        uint32_t sym;
        if (!bitunpack_read(&bu, GLX_DELTA_WIDTH, &sym)) {
            printf("truncated payload: got %zu of %zu symbols\n", i, n);
            return -1;
        }
        /* sign-extend 4-bit two's complement */
        deltas[i] = (int8_t)(sym >= 8 ? (int)sym - 16 : (int)sym);
    }
    free(payload);

    glx_delta_decode(deltas, n, codes);
    free(deltas);

    /* Step 2+3: dequantize -> subtract dither -> [mu-law expand] -> pcm16 */
    float alpha = glx_alpha_from_idx(h.alphaIdx);
    if (glx_decode(codes, n, pcm, h.bitsPerSym, alpha, h.seed, h.mulaw) != 0) {
        printf("Decode failed (bad header parameters)\n");
        return -1;
    }
    free(codes);

    /* Step 4: write a standard 16 kHz mono 16-bit WAV */
    if (wav_write_mono16(argv[2], pcm, n, h.sampleRate) != 0)
        return -1;
    free(pcm);

    printf("wrote %zu samples (%.2f s) to %s\n",
           n, (double)n / h.sampleRate, argv[2]);
    return EXIT_SUCCESS;
}
