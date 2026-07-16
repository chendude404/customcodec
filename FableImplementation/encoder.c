#include "glx.h"
#include "compression.h"
#include <stdlib.h>
#include <string.h>

/*
GLX ENCODER
===========
Usage: ./our.exe our.wav alpha seed bitdepth dither out.glx [huff]
         alpha    — dither index (1 -> 0.0, 2 -> 0.1, ... 11 -> 1.0)
         seed     — LFSR seed for the subtractive dither
         bitdepth — quantizer bits per sample, 1..3
         dither   — 1 = masked RPDF (zero when masked),
                    2 = spiked: the masked (1-alpha) mass sits at ±alpha*Delta/2

Mu-law companding is always on (the CLI flag was removed); the header still
carries the mulaw byte so the decoder stays format-driven.

Pipeline: read .wav -> resample 48 kHz -> 16 kHz -> mu-law compress
          -> dither -> quantize to `bitdepth` bits -> delta compress -> write .glx

BUFFERED: the file is processed one 10 ms packet at a time (480 samples in,
160 out) through fixed stack buffers — nothing is malloc'd and the whole
file is never in RAM, so this runs on small devices and is the stepping
stone to real-time encoding. The dither LFSR, delta predictor and bit
packer all carry across packets, so the output byte stream is identical
to the old whole-file pass. numPackets = ceil(numSamples / 160): a trailing
partial packet is ZERO-PADDED to a full 10 ms instead of dropped (the old
behavior), so the decoded file can end with up to 10 ms of silence but
never loses speech.
*/

static void write_glx_header(FILE *fp, const GlxHeader *h, long *numPacketsOff)
{
    fwrite(h->magic, 1, 4, fp);
    fwrite(&h->sampleRate, sizeof(uint32_t), 1, fp);
    fwrite(&h->bitsPerSym, sizeof(uint8_t), 1, fp);
    fwrite(&h->alphaIdx, sizeof(uint8_t), 1, fp);
    fwrite(&h->mulaw, sizeof(uint8_t), 1, fp);
    fwrite(&h->huff, sizeof(uint8_t), 1, fp);
    fwrite(&h->ditherType, sizeof(uint8_t), 1, fp);
    fwrite(&h->seed, sizeof(uint32_t), 1, fp);
    *numPacketsOff = ftell(fp);   /* so a short read can patch the count */
    fwrite(&h->numPackets, sizeof(uint32_t), 1, fp);
}

int main(int argc, char **argv)
{
    /* ./our.exe our.wav alpha seed bitdepth dither out.glx [huff] mulaw
     * huff (optional): 1 = Huffman-code the deltas (default), 0 = raw */
    if (argc != 9) {
        printf("call of format ./our.exe our.wav alpha seed bitdepth dither out.glx [huff] mulaw\n");
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

    int bitdepth = atoi(argv[4]);
    if (bitdepth < GLX_BITS_MIN || bitdepth > GLX_BITS_MAX)
    {
        printf("Argv[4] (bitdepth) must be %d..%d bits per sample\n",
               GLX_BITS_MIN, GLX_BITS_MAX);
        return -1;
    }

    int mulaw = atoi(argv[8]);   /* always on — kept as a header field, not a CLI arg */ //can change this JIC
    if(mulaw != 1 && mulaw != 0)
    {
        printf("Mu Law needs to be defined , argb[8]");
        return -1;
    }

    int ditherType = atoi(argv[5]);
    //spiked dither is from https://arxiv.org/pdf/2402.16447
    if (ditherType != GLX_DITHER_MASKED && ditherType != GLX_DITHER_SPIKED)
    {
        printf("Argv[5] (dither) must be 1 (masked RPDF) or 2 (spiked)\n");
        return -1;
    }

    int huff = atoi(argv[7]);
       //* default: entropy-code *[✓][✓]
    if(huff != 1 && huff != 0)
    {
        printf("Argv[7] (huff) must be 1 (Huffman) or 0 (raw fixed-width)\n");
        return -1;
    }

    /* Step 1: open the wav — only the header is parsed here; samples are
     * pulled one packet at a time in the loop below [✓] */ 
    WavReader wr;
    if(wav_reader_open(&wr, argv[1]) != 0)
    {
        printf("Could not open File");
        return -1;
    }


    if (wr.rate != GLX_IN_RATE) //48khz
    {
        /* design decision: limited to ONLY 48 kHz so downsampling is cleaner, wrap non 48khz in ffmeg upsampler anyway */
        printf("Input must be %u Hz (got %u Hz)\n", GLX_IN_RATE, wr.rate);
        wav_reader_close(&wr);
        return -1;
    }

    /* packet count is known up front from the data chunk size; round UP so
     * trailing samples that don't fill a whole 10 ms packet are kept — the
     * final packet is zero-padded to full length in the loop below */
    size_t nsamples16khz = wr.framesTotal / GLX_DECIMATION;
    uint32_t numPackets = (uint32_t)((nsamples16khz + GLX_FRAMES_PER_PACKET - 1) / GLX_FRAMES_PER_PACKET); //round up

    /* Step 2: pick the designed Huffman lengths for this bit depth +
     * companding mode and build the table. The lengths are embedded in the
     * file so the decoder is self-describing (no compiled-in table needed). */
    GlxHuffTable htab;
    const uint8_t *huff_len = NULL;
    if (huff) 
    {
        huff_len = glx_huff_lengths_for(bitdepth, mulaw); //should be precomputed
        if (!huff_len || glx_huff_build(&htab, huff_len) != 0) {
            printf("Internal error: invalid Huffman length table\n");
            wav_reader_close(&wr);
            return -1;
        }
    }

    /* Step 3: write header (+ embedded Huffman table) */
    FILE *outfp = fopen(argv[6], "wb");
    if (!outfp) {
        printf("Could not write to output file, Error\n");
        wav_reader_close(&wr);
        return -1;
    }
    printf("We are writing to output file %s\n", argv[6]);

    GlxHeader header = {
        .sampleRate = GLX_OUT_RATE,
        .bitsPerSym = (uint8_t)bitdepth,
        .alphaIdx   = (uint8_t)alphaIdx,
        .mulaw      = (uint8_t)mulaw,
        .huff       = (uint8_t)huff,
        .ditherType = (uint8_t)ditherType,
        .seed       = seed,
        .numPackets = numPackets,
    };
    memcpy(header.magic, GLX_MAGIC, 4);
    long numPacketsOff;
    write_glx_header(outfp, &header, &numPacketsOff);

    /* self-describing table: 8-byte packed code-length block after the header */
    if (huff) {
        uint8_t tbl[GLX_HUFF_TABLE_BYTES];
        glx_huff_pack_lengths(huff_len, tbl);
        fwrite(tbl, 1, sizeof tbl, outfp);
    }

    /* Step 4: packet loop — decimate, [mu-law] -> dither -> quantize, delta,
     * pack, write. All state (LFSR, delta predictor, bit packer) persists
     * across iterations so the stream is seamless at packet boundaries. */
    float alpha = glx_alpha_from_idx(alphaIdx);
    uint32_t rawmask = (1u << GLX_DELTA_WIDTH(bitdepth)) - 1u;

    glx_dither_state st;
    glx_dither_init(&st, seed);
    int prev = -1;
    BitPacker bp = {0};

    int16_t in48[GLX_PACKET_IN_SAMPLES];      /* 480 samples = 10 ms at 48 kHz */
    int16_t pcm16k[GLX_FRAMES_PER_PACKET];    /* 160 samples = 10 ms at 16 kHz */
    uint8_t codes[GLX_FRAMES_PER_PACKET];
    int8_t  deltas[GLX_FRAMES_PER_PACKET];
    uint8_t bytebuf[8];

    uint32_t done = 0;
    for (; done < numPackets; done++) {
        size_t got = wav_reader_read(&wr, in48, GLX_PACKET_IN_SAMPLES);
        if (got == 0)
            break;   /* data chunk shorter than its header claimed */

        /* zero-pad a short final read: the packet's tail encodes as silence
         * instead of the old behavior of dropping the partial packet */
        if (got < GLX_PACKET_IN_SAMPLES)
            memset(in48 + got, 0,
                   (GLX_PACKET_IN_SAMPLES - got) * sizeof(int16_t));

        /* box-filter + decimate 48 kHz -> 16 kHz */
        for (size_t i = 0; i < GLX_FRAMES_PER_PACKET; i++)
            pcm16k[i] = (int16_t)(((int32_t)in48[i*3] + in48[i*3+1] + in48[i*3+2]) / 3);

        if (glx_encode(pcm16k, GLX_FRAMES_PER_PACKET, codes,
                       bitdepth, alpha, &st, mulaw, ditherType) != 0) {
            printf("Encode failed (bad parameters)\n");
            wav_reader_close(&wr);
            fclose(outfp);
            return -1;
        }

        glx_delta_encode(codes, GLX_FRAMES_PER_PACKET, deltas, &prev);

        for (size_t i = 0; i < GLX_FRAMES_PER_PACKET; i++) {
            /* huff: bias delta to a symbol index and emit its canonical code.
             * raw: (bitdepth+1)-bit two's-complement fixed field. */
            size_t w = huff
                ? glx_huff_encode(&bp, &htab, (uint32_t)(deltas[i] + GLX_HUFF_BIAS),
                                  bytebuf, sizeof bytebuf)
                : bitpack_write(&bp, (uint32_t)deltas[i] & rawmask,
                                GLX_DELTA_WIDTH(bitdepth), bytebuf, sizeof bytebuf);
            fwrite(bytebuf, 1, w, outfp);
        }
    }
    size_t w = bitpack_flush(&bp, bytebuf, sizeof bytebuf);
    fwrite(bytebuf, 1, w, outfp);
    wav_reader_close(&wr);

    if (done != numPackets) {
        /* truncated input: patch the header so it describes what was written */
        printf("Input ended early: wrote %u of %u expected packets\n", done, numPackets);
        fseek(outfp, numPacketsOff, SEEK_SET);
        fwrite(&done, sizeof(uint32_t), 1, outfp);
        numPackets = done;
    }
    fclose(outfp);

    size_t n16 = (size_t)numPackets * GLX_FRAMES_PER_PACKET;
    printf("wrote %u packets (%zu samples, %.2f s) at %d bits/sample to %s\n",
           numPackets, n16, (double)n16 / GLX_OUT_RATE, bitdepth, argv[6]);
    return EXIT_SUCCESS;
}
