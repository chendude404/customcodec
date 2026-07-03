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

// Decompression LUT: 3-bit mu-law for MU = 255.0
const float MU_LAW_EXPAND_3BIT[8] = {
    -1.000000f,  // Level 0
    -0.198424f,  // Level 1
    -0.038311f,  // Level 2
    -0.005110f,  // Level 3
     0.005110f,  // Level 4
     0.038311f,  // Level 5
     0.198424f,  // Level 6
     1.000000f   // Level 7
};

uint32_t headroom_lut(int target_bits){
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

int16_t mulaw_remap(int16_t input_sample, int target_bits)
{
/*
G.711-style segmented mu-law (mu=255), integer only.
Output lands in [-safe_max, +safe_max] where safe_max reserves exactly
worst-case-dither headroom, so dither_quantize_fast can never overflow int16.
Call on each sample BEFORE dither_quantize_fast.
*/
    int32_t safe_max = 32767 - (int32_t)headroom_lut(target_bits);

    int neg = input_sample < 0;
    int32_t mag = neg ? -(int32_t)input_sample : (int32_t)input_sample; // <= 32768, fits int32

    mag >>= 2;                  // 16-bit -> 14-bit magnitude domain
    mag += 33;                  // standard mu-law bias
    if (mag > 8191) mag = 8191;

    int seg = 0;                // segment = MSB position above bit 5
    for (int32_t t = mag >> 6; t != 0; t >>= 1) seg++;   // 0..7

    int32_t mant = (mag >> (seg + 1)) & 0x0F;
    int32_t code = (seg << 4) | mant;    // 7-bit companded magnitude code, 0..127

    // spread the code linearly across the headroom-safe output range
    int32_t out = (code * safe_max) / 127;   // <= 127*32767 < 2^22, no overflow
    return (int16_t)(neg ? -out : out);
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
void dither_quantize_fast(int16_t * restrict samples, size_t num_samples, int target_bits, uint32_t alpha_q16, uint32_t * restrict seed_ptr) 
{
    int shift_bits = 16 - target_bits;
    int32_t delta = 1 << shift_bits; 
    int32_t active_width = (alpha_q16 * delta) >> 16; 
    uint64_t threshold = (uint64_t)alpha_q16 << 16;

    // Process the array in small chunks to stay within the L1 cache
    for (size_t i = 0; i < num_samples; i += BLOCK_SIZE) {
        
        size_t current_block = (num_samples - i < BLOCK_SIZE) ? (num_samples - i) : BLOCK_SIZE;
        int32_t dither_buffer[BLOCK_SIZE] = {0};

        // Phase 1: Sequential Noise Generation
        // This loop is purely stateful but isolated from the heavy math
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
            int32_t dithered = (int32_t)samples[i + j] + dither_buffer[j];

            // Branchless clamping (compiles to fast CMOV/CSEL instructions)
            dithered = dithered > 32767 ? 32767 : dithered;
            dithered = dithered < -32768 ? -32768 : dithered;
            
            int32_t floor_term = dithered >> shift_bits; 
            samples[i + j] = (int16_t)((floor_term << shift_bits) + (delta >> 1));
        }
    }
}