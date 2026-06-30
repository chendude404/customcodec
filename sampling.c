#include "declarations.h"
#include "dithering.h"
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


STEP 1 — COMPUTE DECIMATION FACTOR
====================================

    int decimation_factor = input_sample_rate / 16000
    // e.g. 48000 / 16000 = 3  (keep every 3rd sample)
    // e.g. 32000 / 16000 = 2
    //
    // If input_sample_rate is not an integer multiple of 16000
    // (e.g. 44100), a polyphase resampler is needed instead.
    // For now we assume a clean multiple.
*/
void resamplepackets(int16_t * databus, FILE * fp, int16_t numsamples, int alphaval, uint32_t * seed)
{
    int decimation_factor = 3; // 48kHz -> 16kHz
    int out_count = numsamples / decimation_factor;

    // Step 1: box-filter + decimate (average 3 input samples per output sample)
    int16_t downsampled[out_count];
    for(int i = 0; i < out_count; i++)
        downsampled[i] = ((int32_t)databus[i*3] + databus[i*3+1] + databus[i*3+2]) / 3;

    // Step 2: dither and quantize the decimated samples to 3 bits
    dither_quantize_fast(downsampled, out_count, 3, threshol_lut(alphaval), seed);

    // Step 3: bit-pack 8 x 3-bit symbols into 3 bytes, write each group
    //CHECK THIS THOROUGHLY
    int counter = 0;
    char bitpack[3] = {0, 0, 0};
    for(int i = 0; i < out_count; i++)
    {
        uint8_t s = downsampled[i] & 0x07;

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

//void bitpacker() for later

/*
STEP 2 — ANTI-ALIASING LOW-PASS FILTER (before decimating)
============================================================

    Before discarding samples we must low-pass filter to below
    the Nyquist frequency of the OUTPUT rate (8kHz), otherwise
    high-frequency content aliases back into the signal.

    Simple FIR approach (pseudocode):

        int16_t lpf(int16_t * window, int window_len, float * coeffs)
            sum = 0
            for i in 0 .. window_len:
                sum += window[i] * coeffs[i]
            return clamp(sum, -32768, 32767)

    The coefficients are a pre-computed windowed-sinc kernel
    with cutoff at 8kHz. Window length of 32–64 taps is enough.

    In practice, for a 3:1 decimation a simple 3-point box filter
    (average of 3 consecutive samples) is a fast approximation:

        filtered = (samples[i] + samples[i+1] + samples[i+2]) / 3


STEP 3 — DECIMATE
==================

    Consume `decimation_factor` input samples, output 1 filtered sample.

    int output_index = 0
    for i = 0; i < num_input_samples; i += decimation_factor:
        int16_t filtered = lpf(samples + i, decimation_factor)
        downsampled[output_index++] = filtered

    output frame count = num_input_samples / decimation_factor
    // e.g. 480 input frames (10ms @ 48kHz) -> 160 output frames (10ms @ 16kHz)


STEP 4 — 3-BIT QUANTIZATION
=============================

    3 bits = 8 quantization levels
    Input is int16_t: range [-32768, 32767]
    Each quantization step = 65536 / 8 = 8192

    Use dither_quantize() from dithering.c with target_bits = 3:

        int16_t q = dither_quantize(sample, 3, alpha_q16, &seed)

    dither_quantize returns a 16-bit value snapped to one of 8 levels.
    To convert to a 3-bit symbol in [0, 7]:

        uint8_t symbol = (uint8_t)((q + 32768) >> 13)
        // shifts the signed 16-bit value into [0, 65535] then takes top 3 bits

    Full loop:

        for i in 0 .. num_downsampled_frames:
            int16_t q = dither_quantize(downsampled[i], 3, alpha_q16, &seed)
            symbols[i] = (uint8_t)((q + 32768) >> 13)


STEP 5 — BIT PACKING (optional, for compact output)
=====================================================

    Each symbol is 3 bits. Pack multiple symbols into bytes to avoid
    wasting 5 bits per byte.

    8 symbols = 24 bits = 3 bytes exactly (no padding needed).

    void pack_3bit_symbols(uint8_t * symbols, int count, uint8_t * out)
        bit_buffer = 0
        bits_in_buffer = 0
        out_index = 0

        for each symbol in symbols[0..count]:
            bit_buffer = (bit_buffer << 3) | (symbol & 0x7)
            bits_in_buffer += 3
            if bits_in_buffer >= 8:
                out[out_index++] = (bit_buffer >> (bits_in_buffer - 8)) & 0xFF
                bits_in_buffer -= 8

        if bits_in_buffer > 0:        // flush remaining bits
            out[out_index++] = (bit_buffer << (8 - bits_in_buffer)) & 0xFF


WIRING INTO top.c
==================

    Replace //PROCESS PACKET HERE with:

        int out_frames = framesperpacket / decimation_factor    // e.g. 160
        int16_t downsampled[out_frames]
        downsample(databus, framesperpacket, decimation_factor, downsampled)

        uint8_t symbols[out_frames]
        quantize_to_3bit(downsampled, out_frames, alpha_q16, &seed, symbols)

        uint8_t packed[ (out_frames * 3 + 7) / 8 ]
        pack_3bit_symbols(symbols, out_frames, packed)

        // pass packed to huffman encoder
*/
