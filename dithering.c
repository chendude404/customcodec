#include "dithering.h"

/*
THE asr codec in C -  
.glx
Input 16bit 48 kHz 
*/

//https://www.itu.int/rec/T-REC-G.191/en 
uint32_t threshol_lut(int alphaIdx){
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

//implement MU law creation
//https://www.cs.columbia.edu/~hgs/research/projects/NetworkAudioLibrary/nal_spring/src/Codecs/g711.cpp
//from 
const int BIAS = 0x84;
const int CLIP = 8159;

int16_t lineartomulaw(int16_t sample)
{
    short         mask;
    short	 seg;
    unsigned char uval;
    /* Get the sign and the magnitude of the value. */
    sample = sample >> 2;
    if (sample < 0) {
        sample = -sample;
        mask = 0x7F;
    } else {
        mask = 0xFF;
    }
    if ( sample > CLIP ) sample = CLIP;		/* clip the magnitude */
    sample += (BIAS >> 2);
    
    /* Convert the scaled magnitude to segment number. */
    seg = searchUEnd(sample);
    
    /*
    * Combine the sign, segment, quantization bits;
    * and complement the code word.
    */
    if (seg >= 8)		/* out of range, return maximum value. */
        return (unsigned char) (0x7F ^ mask);
    else {
        uval = (unsigned char) (seg << 4) | ((sample >> (seg + 1)) & 0xF);
        return (uval ^ mask);
    }

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

void lfsr32(uint32_t *seedptr){
/*
Taps from: https://docs.amd.com/v/u/en-US/xapp052
*/
    uint32_t bit = (((*seedptr >> 31) ^ (*seedptr >> 21) ^ (*seedptr >> 1) ^ (*seedptr >> 0)) & 1) ;
    *seedptr = (*seedptr >> 1) | (bit << 31);
}

//GEMINI's QUANTIZER FOR THE ENTIRE SAMPLE

//WE PROBABLY NEED TO FIX THE DITHER AMPLITUDE 
FIX_ME_IMPLEMENT_THIS(); 

void dither_quantize_fast(int16_t * restrict samples, size_t num_samples, int target_bits, uint32_t alpha_q16, uint32_t * restrict seed_ptr, int mulaw) 
{
    int shift_bits = 16 - target_bits;
    int32_t delta = 1 << shift_bits; 
    int32_t active_width = (alpha_q16 * delta) >> 16; 
    uint64_t threshold = (uint64_t)alpha_q16 << 16;

    // Process the array in small chunks to stay within the L1 cache
    for (size_t i = 0; i < num_samples; i += BLOCK_SIZE) {
        
        size_t current_block = (num_samples - i < BLOCK_SIZE) ? (num_samples - i) : BLOCK_SIZE;
        int32_t dither_buffer[BLOCK_SIZE] = {0};

        // Phase 1: Sequential Noise Generation should be isolated 
        for (size_t j = 0; j < current_block; j++) {
            lfsr32(seed_ptr);
            if (*seed_ptr <= threshold) {
                lfsr32(seed_ptr);
                uint32_t uniform_noise = ((uint64_t)(*seed_ptr) * active_width) >> 32;
                dither_buffer[j] = (int32_t)uniform_noise - (active_width >> 1);
            }
        }

        // Phase 2: Vectorizable Quantization Math
        // Because 'samples' is marked restrict, the compiler can use SIMD here
        for (size_t j = 0; j < current_block; j++) {
            //
            int dithered = samples[i + j];
            if(mulaw)
            {
                dithered = dither_buffer[j] + lineartomulaw(samples[i + j]);
            }
            else //directly quantized
            {
                dithered = (int32_t)samples[i + j] + dither_buffer[j];
            }
            
            // Branchless clamping (compiles to fast CMOV/CSEL instructions)
            dithered = dithered > 32767 ? 32767 : dithered;
            dithered = dithered < -32768 ? -32768 : dithered;
            
            int32_t floor_term = dithered >> shift_bits; 
            samples[i + j] = (int16_t)((floor_term << shift_bits) + (delta >> 1));


        }

        //
    }
}