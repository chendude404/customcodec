#include "declarations.h"
#include "dithering.h"
#include "bitpack.h"

#include <math.h>

// Precomputed constant for ln(1 + 255)
#define LN_256 5.545177444479562
/*
RESAMPLING + 3-BIT ENCODING MODULE
===================================

Goal:
  - Downsample input PCM (e.g. 48kHz or 44.1kHz) to 16kHz
  - Quantize each output sample to 3 bits (8 levels)

Assumptions:
  - Input: int16_t samples, mono (already downmixed)
  - Output: uint8_t values in range [0, 7] (3-bit symbols)
  - dither_quantize() is available from dithering.c
*/
void resamplepackets(int16_t * databus, FILE * fp, int16_t numsamples, int alphaval, uint32_t * seed, int mulaw)
{
    int decimation_factor = 3; // 48kHz -> 16kHz
    int out_count = numsamples / decimation_factor;

    // Step 1: box-filter + decimate (average 3 input samples per output sample)
    int16_t downsampled[out_count];
    for(int i = 0; i < out_count; i++)
        downsampled[i] = ((int32_t)databus[i*3] + databus[i*3+1] + databus[i*3+2]) / 3;
    //CONVERT TO 16khz
    
    if(mulaw)
    {
        //convert each sample into a double between -1, 1
        
        //apply mulaw math
    }
    // Step 2: dither and quantize the decimated samples to 3 bits
    else
    {
        dither_quantize_fast(downsampled, out_count, 3, threshol_lut(alphaval), seed);
    }
    

    // Step 3: bit-pack 8 x 3-bit symbols into 3 bytes, write each group
    //CHECK THIS THOROUGHLY
    int counter = 0;
    char bitpack[3] = {0, 0, 0};
    for(int i = 0; i < out_count; i++)
    {
        uint8_t s = (uint8_t)((downsampled[i] + 32768) >> 13);

        if      (counter == 0) { bitpack[0] |= (s << 5); }
        else if (counter == 1) { bitpack[0] |= (s << 2); }
        else if (counter == 2) { bitpack[0] |= (s >> 1);  bitpack[1] |= ((s & 0x01) << 7); }
        else if (counter == 3) { bitpack[1] |= (s << 4); }
        else if (counter == 4) { bitpack[1] |= (s << 1); }
        else if (counter == 5) { bitpack[1] |= (s >> 2);  bitpack[2] |= ((s & 0x03) << 6); }
        else if (counter == 6) { bitpack[2] |= (s << 3); }
        else if (counter == 7)
        {
            bitpack[2] |= s;
            fwrite(bitpack, 1, 3, fp);
            bitpack[0] = 0; bitpack[1] = 0; bitpack[2] = 0;
            counter = -1;
        }
        counter++;
    }
}

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

void init_mulaw_lut(int8_t mulaw_lut) 
{
    for (int i = -32768; i <= 32767; i++) {
        double sign = (i < 0) ? -1.0 : 1.0;
        double normalized = fabs((double)i) / 32768.0;
        double result = sign * (log(1.0 + 255.0 * normalized) / log(256.0));
        mulaw_lut[(uint16_t)i] = (int8_t)(result * 127.0);
    }
}