# FableImplementation — refactored .glx codec

A clean encoder/decoder pair refactored from the code in the parent folder.
Nothing in the parent folder was modified.

## Build

```sh
gcc -O2 -Wall -Wextra -o our.exe       encoder.c mulaw.c bitpack.c wav.c -lm
gcc -O2 -Wall -Wextra -o glxdecode.exe decoder.c mulaw.c bitpack.c wav.c -lm
```

## Usage

```sh
# encode: ./our.exe our.wav alpha seed mulaw out.glx
./our.exe input48k.wav 6 12345 1 out.glx   # alpha idx 6 (=0.5), seed 12345, mu-law on

# decode: ./glxdecode.exe in.glx out.wav
./glxdecode.exe out.glx out.wav
```

- `alpha` — dither index, same convention as the old `threshol_lut`:
  `1` → 0.0, `2` → 0.1, … `10` → 0.9, `11+` → 1.0
- `seed` — LFSR seed; the decoder regenerates the identical dither stream
  from the seed stored in the header (subtractive dithering)
- `mulaw` — `1` for µ-law companding, `0` for linear
- Input must be a **48 kHz** 16-bit PCM WAV (mono or stereo); this is a
  deliberate design limit so decimation to 16 kHz is a clean ÷3

## Pipeline

**Encoder**: read WAV → downmix stereo → box-filter decimate 48 kHz → 16 kHz
→ [µ-law compress if `mulaw`] → headroom scale → class-2 dither → 3-bit
midrise quantize → delta transform → bit-pack 4-bit signed deltas → `.glx`

**Decoder**: read `.glx` header → bit-unpack deltas → delta decode →
dequantize → subtract the same dither stream → undo headroom →
[µ-law expand if `mulaw`] → 16 kHz mono 16-bit WAV

## .glx header (19 bytes, little-endian — same layout as before)

| offset | field | type |
|---|---|---|
| 0 | magic `"GLX1"` | char[4] |
| 4 | sampleRate (16000) | u32 |
| 8 | bitsPerSym (3) | u8 |
| 9 | alphaIdx | u8 |
| 10 | mulaw | u8 |
| 11 | seed | u32 |
| 15 | numPackets | u32 |

A packet is 160 samples (10 ms at 16 kHz). Trailing samples that don't fill
a whole packet are dropped, as before. Payload is `numPackets * 160` 4-bit
symbols, MSB-first, zero-padded to a byte at the end.

## What came from where

| new file | refactored from |
|---|---|
| `mulaw.c` | `../mulaw.c` — float µ-law + LFSR dither + quantizer + delta transform, unchanged except `glx_encode`/`glx_decode` now take a `mulaw` flag so the linear path shares the same dither/headroom machinery |
| `bitpack.c` | packer from `../sampling.c`, unpacker from `../decompress.c`, merged |
| `wav.c` | chunk-walking reader from `../top.c` `readfile()` + `../claudius.c`; writer from `../glxdecode.c` |
| `encoder.c` | `../top.c` main flow (arg parsing, downmix, decimate, header write) |
| `decoder.c` | `../glxdecode.c` main flow |
| `glx.h` | `../essentials.h`, `../declarations.h`, `../bitpack.h`, `../mulaw.h`, `../dithering.h`, consolidated |

## Notes on the wire format

The delta transform maps 3-bit codes (0–7) to differences in [-7, +7],
stored as 4-bit two's complement. On its own this is 4 bits/sample vs the
raw 3; it's the precursor stage for the entropy coder (see
`../huffmanencoding.c`) — deltas of speech codes cluster tightly around 0,
which is what a Huffman/Rice stage exploits. Encoder and decoder keep one
`BitPacker`/`BitUnpacker` and one dither LFSR alive across the whole
stream, so symbol boundaries need not align to bytes or packets.

## Fixes relative to the old code

- `top.c` parsed `argc != 5` but the documented call has 5 args (`argc == 6`),
  compared the *pointer* `argv[4]` to int, and wrote the output to `argv[5]`
  while opening `argv[4]` — the new encoder parses all five correctly.
- `writeglxheader` stored `alpha * 255` (overflows for idx ≥ 2) while the
  decoder expected the raw index — the header now stores `alphaIdx` as-is.
- Encoder and decoder previously used *different* quantizers
  (`dithering.c` integer path vs `glxdecode.c` bin-centre reconstruction);
  both now run through the single float pipeline in `mulaw.c`, so the
  subtractive dither and headroom factor cancel exactly.
