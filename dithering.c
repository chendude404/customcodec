#include <stdint.h>

/*
THE asr codec in C -  
.glz
Input 16bit 48 kHz 
*/
 
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
    default: return 65636; // alpha=1
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

void lfsr32(uint32_t *seedptr){
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

int16_t dither_quantize(int16_t input_sample, int target_bits, uint32_t alpha_q16, uint32_t *seed_ptr) 
{
    int shift_bits = 16 - target_bits;
    int32_t delta = 1 << shift_bits; 

    int32_t active_width = (alpha_q16 * delta) >> 16; 
    int32_t dither = 0;

    lfsr32(seed_ptr);
    if (*seed_ptr <= (alpha_q16 << 16)) {
        lfsr32(seed_ptr);
        uint32_t rand_val = *seed_ptr;
        
        uint32_t uniform_noise = ((uint64_t)rand_val * active_width) >> 32;
        dither = (int32_t)uniform_noise - (active_width >> 1);
    }

    int32_t dithered_sample = (int32_t)input_sample + dither;

    if (dithered_sample > 32767)  dithered_sample = 32767;
    if (dithered_sample < -32768) dithered_sample = -32768;
    //D * floor( dithered_sample / D ) + D/2
    int32_t floor_term = dithered_sample >> shift_bits; 
    int32_t quantized_output = (floor_term << shift_bits) + (delta >> 1);

    return (int16_t)quantized_output;
}