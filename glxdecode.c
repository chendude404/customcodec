// Decodes a .glx file back to a 16 kHz mono 16-bit PCM .wav so it can be played.
// Usage: ./glxdecode out.glx out.wav
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

//bitmasks v&0xff isolates the lowest 8 bits
uint32_t threshol_lut(int alphaIdx)
{
    switch (alphaIdx) {
    case 1: return 0; // alpha=0
    case 2: return 6564; // alpha=0.1
    case 3: return 13128; // alpha=0.2
    case 4: return 19691; // alpha=0.3
    case 5: return 26255; // alpha=0.4
    case 6: return 32818; // alpha=0.5
    case 7: return 39387; // alpha=0.6
    case 8: return 45945; // alpha=0.7 
    case 9: return 52509; // alpha=0.8
    case 10: return 59072; // alpha=0.9
    default: return 65536; // alpha=1
    };
}

short lcg_rng(short prev, short a, short c, short m)
{
/*
x_n+1 = (a * x_n + c) mod m
Time complexity:
Generating a sequesnce: O(n)
Generating next smaple: O(1)
Space complexity: O(1)
Notes:
- fast + easy to implement
- can only generate m unique numbers 
*/
    return (a * prev + c) % m;
}

void lfsr32(uint32_t *seedptr)
{
/*
Taps from: https://docs.amd.com/v/u/en-US/xapp052
*/
    uint32_t bit = ((
        (*seedptr >> 31) ^ 
        (*seedptr >> 21) ^ 
        (*seedptr >> 1) ^ 
        (*seedptr >> 0)) & 
        1) ;
    *seedptr = (*seedptr >> 1) | (bit << 31);
}

uint32_t headroom_lut(int target_bits)
{
    // worst-case |dither| at alpha=1.0 is delta/2 = 2^(15 - target_bits)
    switch (target_bits) {
    case 1: return 16384;
    case 2: return 8192;
    case 3: return 4096;
    case 4: return 2048;
    case 5: return 1024;
    case 6: return 512;
    case 7: return 256;
    case 8: return 128;
    default: return 0;
    };
}

int16_t mulaw_expand(int16_t compressed, int target_bits)
{
/*
Inverse of mulaw_remap() in dithering.c. Integer only.
Input is a companded-domain sample (bin centre minus subtractive dither),
which is why this takes an arbitrary int16 rather than a 3-bit symbol.
Call AFTER the dither subtraction, on the companded sample.
*/
    int32_t safe_max = 32767 - (int32_t)headroom_lut(target_bits);

    int neg = compressed < 0;
    int32_t mag = neg ? -(int32_t)compressed : (int32_t)compressed;
    if (mag > safe_max) mag = safe_max;   // dither can push slightly past; clamp

    // undo the linear spread: recover the 7-bit code, rounding to nearest
    int32_t code = (mag * 127 + (safe_max >> 1)) / safe_max;  // <= 32767*127 < 2^22
    if (code > 127) code = 127;

    int32_t seg  = code >> 4;
    int32_t mant = code & 0x0F;

    // inverse of the segment packing, then unbias and return to 16-bit domain
    int32_t m14 = (((mant << 1) + 33) << seg) - 33;   // <= 8031, no overflow
    int32_t out = m14 << 2;                            // <= 32124
    if (out > 32767) out = 32767;
    return (int16_t)(neg ? -out : out);
}

static void write_le16(FILE *f, uint16_t v)
{
    fputc(v&0xff, f);
    fputc((v>>8)&0xff, f);
}

static void write_le32(FILE *f, uint32_t v)
{
    fputc(v&0xff, f);
    fputc((v>>8)&0xff, f);
    fputc((v>>16)&0xff, f);
    fputc((v>>24)&0xff, f);
}

int main(int argc, char **argv)
{
    if(argc != 3)
    { 
        printf("usage: %s in.glx out.wav\n", argv[0]); 
        return -1; 
    }

    FILE *in = fopen(argv[1], "rb");
    if(!in)
    { 
        printf("cannot open %s\n", argv[1]); 
        return -1; 
    }

    // --- GLX header (14 bytes): magic[4], sampleRate u32, bitsPerSym u8, alphaIdx u8, numPackets u32 ---
    char magic[4];
    uint32_t sampleRate, numPackets, seedidx;
    uint8_t bitsPerSym, alphaIdx, mulaw;
    fread(magic, 1, 4, in);
    fread(&sampleRate, sizeof(uint32_t), 1, in);
    fread(&bitsPerSym, 1, 1, in);
    fread(&alphaIdx, 1, 1, in);
    fread(&mulaw, sizeof(uint8_t), 1, in);
    fread(&seedidx, sizeof(uint32_t), 1, in);
    fread(&numPackets, sizeof(uint32_t), 1, in);

    if(memcmp(magic, "GLX1", 4) != 0)
    { 
        printf("not a GLX1 file\n"); 
        return -1; 
    }
    printf("rate=%u bits=%u alpha=%u packets=%u\n", sampleRate, bitsPerSym, alphaIdx, numPackets);
    //DECOMPRESS HERE -> WHAT ALGORITHM


    // --- decode 3-bit symbols (8 symbols per 3 bytes, MSB-first) into 16-bit PCM ---
    //FIRST WE HAVE TO RECONSTRUCT DITHER
    int target_bits = 3; //magic value all we have rn
    int shift_bits = 16 - target_bits;
    int32_t delta = 1 << shift_bits;
    uint32_t alpha_q16 = threshol_lut(alphaIdx);
    int32_t active_width = (alpha_q16 * delta) >> 16;
    uint64_t threshold = (uint64_t)alpha_q16 << 16;




    FILE *tmp = tmpfile();
    uint32_t nsamples = 0;
    unsigned char b[3];
    while(fread(b, 1, 3, in) == 3)
    {
        uint32_t v = ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
        for(int k = 0; k < 8; k++)
        {
            int s = (v >> (21 - 3*k)) & 0x7;          // 0..7
            int16_t bin_center = (int16_t)(s * 8192 - 28672); // inverse of (x+32768)>>13 bin centre

            //subtractive dither -> mirrored on other side of encoder
            int32_t dither = 0;
            lfsr32(&seedidx);                 // unconditional per-sample gate draw
            if (seedidx <= threshold)
            {
                lfsr32(&seedidx);             // noise draw, only if gated
                uint32_t uniform_noise = ((uint64_t)seedidx * active_width) >> 32;
                dither = (int32_t)uniform_noise - (active_width >> 1);
            }

            int32_t sample = (int32_t)bin_center - dither;
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;
            write_le16(tmp, (uint16_t)sample);
            nsamples++;
        }
    }

    //new function to requantize

    // --- write a standard 16 kHz mono 16-bit WAV ---
    FILE *out = fopen(argv[2], "wb");
    if(!out){ printf("cannot write %s\n", argv[2]); return -1; }
    uint32_t dataBytes = nsamples * 2;
    fwrite("RIFF", 1, 4, out); 
    write_le32(out, 36 + dataBytes); 
    fwrite("WAVE", 1, 4, out);
    fwrite("fmt ", 1, 4, out); 
    write_le32(out, 16);
    write_le16(out, 1);            // PCM
    write_le16(out, 1);            // mono
    write_le32(out, sampleRate);
    write_le32(out, sampleRate*2); // byte rate
    write_le16(out, 2);            // block align
    write_le16(out, 16);          // bits per sample
    fwrite("data", 1, 4, out); write_le32(out, dataBytes);

    rewind(tmp);
    unsigned char buf[4096]; size_t n;
    while((n = fread(buf, 1, sizeof buf, tmp)) > 0) fwrite(buf, 1, n, out);

    fclose(tmp); fclose(in); fclose(out);
    printf("wrote %u samples (%.2f s) to %s\n", nsamples, (double)nsamples/sampleRate, argv[2]);
    return 0;
}

