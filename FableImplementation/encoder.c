#include "glx.h"
#include <stdlib.h>
#include <string.h>

/*
GLX ENCODER
===========
Usage: ./our.exe our.wav alpha seed mulaw out.glx
         alpha — dither index (1 -> 0.0, 2 -> 0.1, ... 11 -> 1.0)
         seed  — LFSR seed for the subtractive dither
         mulaw — 1 for mu-law companding, 0 for linear

Pipeline: read .wav -> resample 48 kHz -> 16 kHz -> [mu-law compress]
          -> dither -> quantize to 3 bits -> delta compress -> write .glx

Refactored from ../top.c + ../sampling.c: the whole file is processed in
one pass instead of 10 ms packets, so the dither LFSR and the delta
predictor run unbroken across the stream. numPackets in the header keeps
the old meaning (numSamples / 160) and, as before, trailing samples that
don't fill a whole packet are dropped.
*/

static void write_glx_header(FILE *fp, const GlxHeader *h)
{
    fwrite(h->magic, 1, 4, fp);
    fwrite(&h->sampleRate, sizeof(uint32_t), 1, fp);
    fwrite(&h->bitsPerSym, sizeof(uint8_t), 1, fp);
    fwrite(&h->alphaIdx, sizeof(uint8_t), 1, fp);
    fwrite(&h->mulaw, sizeof(uint8_t), 1, fp);
    fwrite(&h->seed, sizeof(uint32_t), 1, fp);
    fwrite(&h->numPackets, sizeof(uint32_t), 1, fp);
}

int main(int argc, char **argv)
{
    /* call of format ./our.exe our.wav alpha seed int (1 for mu law, 0 for not) out.glx */
    if (argc != 6) {
        printf("call of format ./our.exe our.wav alpha seed mulaw out.glx\n");
        return -1;
    }

    int alphaIdx  = atoi(argv[2]);
    //check
    uint32_t seed = (uint32_t)strtoul(argv[3], NULL, 10);
    if(seed == 0)
    {
        printf("Seed Cannot be 0, otherwise Dither RNG breaks\n");
        return -1;
    } //added seed check

    int mulaw = atoi(argv[4]);

    if (mulaw != 1 && mulaw != 0) 
    {
        printf("Argv[4] must be Mu Law: 1 for Yes, 0 for No\n");
        return -1;
    }
    //good thus far
    /* Step 1: read the wav, downmixed to mono */
    uint32_t rate;
    size_t n48; //check what sizet does
    int16_t *pcm48 = wav_read_mono16(argv[1], &rate, &n48); //this should get how many packets?

    if (!pcm48) return -1;

    if (rate != GLX_IN_RATE) {
        /* design decision: limited to ONLY 48 kHz so downsampling works better */
        printf("Input must be %u Hz (got %u Hz)\n", GLX_IN_RATE, rate);
        free(pcm48);
        return -1;
    }

    /* Step 2: box-filter + decimate 48 kHz -> 16 kHz, then drop the
     * trailing partial packet so numPackets describes the stream exactly */

    //FAHHH DO NOT DROP THE LAST PARTIAL PACKET
    size_t n16 = n48 / GLX_DECIMATION;
    uint32_t numPackets = (uint32_t)(n16 / GLX_FRAMES_PER_PACKET);
    n16 = (size_t)numPackets * GLX_FRAMES_PER_PACKET;

    int16_t *pcm16k = malloc(n16 * sizeof(int16_t));
    uint8_t *codes  = malloc(n16);
    int8_t  *deltas = malloc(n16);
    if (!pcm16k || !codes || !deltas) {
        printf("Out of memory\n");
        return -1;
    }

    for (size_t i = 0; i < n16; i++)
        pcm16k[i] = (int16_t)(((int32_t)pcm48[i*3] + pcm48[i*3+1] + pcm48[i*3+2]) / 3);
    free(pcm48);

    /* Step 3: [mu-law] -> dither -> quantize to 3 bits */
    float alpha = glx_alpha_from_idx(alphaIdx);
    if (glx_encode(pcm16k, n16, codes, GLX_BITS, alpha, seed, mulaw) != 0) {
        printf("Encode failed (bad parameters)\n");
        return -1;
    }
    free(pcm16k);

    /* Step 4: delta compress */
    glx_delta_encode(codes, n16, deltas);
    free(codes);

    /* Step 5: write header + bit-packed signed deltas */
    FILE *outfp = fopen(argv[5], "wb");
    if (!outfp) {
        printf("Could not write to output file, Error\n");
        return -1;
    }
    printf("We are writing to output file %s\n", argv[5]);

    GlxHeader header = {
        .sampleRate = GLX_OUT_RATE,
        .bitsPerSym = GLX_BITS,
        .alphaIdx   = (uint8_t)alphaIdx,
        .mulaw      = (uint8_t)mulaw,
        .seed       = seed,
        .numPackets = numPackets,
    };
    memcpy(header.magic, GLX_MAGIC, 4);
    write_glx_header(outfp, &header);

    BitPacker bp = {0};
    uint8_t bytebuf[8];
    for (size_t i = 0; i < n16; i++) {
        /* deltas are in [-7, 7]; 4-bit two's complement on the wire */
        size_t w = bitpack_write(&bp, (uint32_t)deltas[i] & 0xF,
                                 GLX_DELTA_WIDTH, bytebuf, sizeof bytebuf);
        fwrite(bytebuf, 1, w, outfp);
    }
    size_t w = bitpack_flush(&bp, bytebuf, sizeof bytebuf);
    fwrite(bytebuf, 1, w, outfp);
    free(deltas);
    fclose(outfp);

    printf("wrote %u packets (%zu samples, %.2f s) to %s\n",
           numPackets, n16, (double)n16 / GLX_OUT_RATE, argv[5]);
    return EXIT_SUCCESS;
}
