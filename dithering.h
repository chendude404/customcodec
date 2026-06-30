#ifndef ASR_CODEC_H
#define ASR_CODEC_H

#include <stdint.h>
#include <stddef.h>

uint32_t threshol_lut(int alphaIdx);

short lcg_rng(short prev, short a, short c, short m);

void lfsr32(uint32_t *seedptr);

int16_t dither_quantize(int16_t input_sample, int target_bits, uint32_t alpha_q16, uint32_t *seed_ptr);

void dither_quantize_fast(int16_t * restrict samples, size_t num_samples, int target_bits, uint32_t alpha_q16, uint32_t * restrict seed_ptr);
#define BLOCK_SIZE 64

#endif /* ASR_CODEC_H */