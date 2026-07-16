# FableImplementation — refactored .glx codec

A clean encoder/decoder pair refactored from the code in the parent folder.
Nothing in the parent folder was modified.

## Build

```sh
gcc -O2 -Wall -Wextra -o our.exe       encoder.c mulaw.c bitpack.c wav.c compression.c -lm
gcc -O2 -Wall -Wextra -o glxdecode.exe decoder.c mulaw.c bitpack.c wav.c compression.c -lm
```

## Usage

```sh
# encode: ./our.exe our.wav alpha seed bitdepth dither out.glx [huff]
./our.exe input48k.wav 6 12345 3 1 out.glx     # alpha idx 6 (=0.5), seed 12345, 3-bit, masked dither, Huffman on
./our.exe input48k.wav 6 12345 1 1 out.glx     # bitdepth=1: 1-bit quantizer
./our.exe input48k.wav 6 12345 3 2 out.glx     # dither=2: spiked dither PDF
./our.exe input48k.wav 6 12345 3 1 out.glx 0   # huff=0: raw fixed-width deltas (no entropy coding)

# decode: ./glxdecode.exe in.glx out.wav   (reads bitdepth + huff + dither flags from the header)
./glxdecode.exe out.glx out.wav
```

- `alpha` — dither index, same convention as the old `threshol_lut`:
  `1` → 0.0, `2` → 0.1, … `10` → 0.9, `11+` → 1.0
- `seed` — LFSR seed; the decoder regenerates the identical dither stream
  from the seed stored in the header (subtractive dithering)
- `bitdepth` — quantizer bits per sample, `1`–`3`. µ-law companding is
  **always on** (the old `mulaw` CLI flag was removed; the header still
  carries the byte so the decoder stays format-driven)
- `dither` — selects the dither PDF (stored in the header so the decoder
  regenerates and subtracts the exact same stream):
  - `1` (masked RPDF): `f(v) = α·Π_αΔ(v) + (1−α)·δ(v)` — with probability α
    the dither is uniform over ±αΔ/2, otherwise it is 0
  - `2` (spiked): `f(v) = α·Π_αΔ(v) + ((1−α)/2)·[δ(v − αΔ/2) + δ(v + αΔ/2)]`
    — same uniform part, but the remaining (1−α) mass is split into two
    equal spikes at ±αΔ/2 instead of sitting at 0
  - both PDFs peak at ±αΔ/2, so the headroom factor is identical, and both
    consume exactly two LFSR draws per sample, so `dither=1` output is
    bit-identical to the previous format (apart from the new header byte)
- `huff` (optional, default `1`) — `1` Huffman-codes the delta stream,
  `0` stores raw `(bitdepth+1)`-bit deltas. The decoder picks the path from
  the header.
- Input must be a **48 kHz** 16-bit PCM WAV (mono or stereo); this is a
  deliberate design limit so decimation to 16 kHz is a clean ÷3

## Pipeline

**Encoder**: read WAV → downmix stereo → box-filter decimate 48 kHz → 16 kHz
→ µ-law compress → headroom scale → dither (type 1 or 2) → `bitdepth`-bit
midrise quantize → delta transform → [Huffman-code deltas if `huff`, else
bit-pack `(bitdepth+1)`-bit signed deltas] → `.glx`

**Decoder**: read `.glx` header → [Huffman-decode if `huff`, else bit-unpack
fixed-width deltas] → delta decode → dequantize → subtract the same dither
stream → undo headroom → [µ-law expand if `mulaw`] → 16 kHz mono 16-bit WAV

### Buffered 10 ms packet processing

Both sides run the stream **one 10 ms packet at a time** (480 samples in /
160 samples out per packet) through fixed stack buffers — the encoder pulls
WAV samples incrementally (`wav_reader_*`), the decoder pulls the payload
through a 512-byte file buffer and appends to the WAV incrementally
(`wav_writer_*`). Neither side ever holds the whole file in RAM (~1 KB of
working buffers total), so the codec fits devices with little memory and is
the stepping stone toward real-time encoding. The dither LFSR, the delta
predictor, and the bit packer/unpacker all **carry across packet
boundaries**, so the packet loop produces byte-identical output to the old
whole-file pass (verified against the previous binaries) — except that a
trailing partial packet is now zero-padded and kept, where the old encoder
dropped it, so files that aren't an exact multiple of 10 ms gain one final
packet.

## .glx layout (little-endian)

**Fixed 21-byte header:**

| offset | field | type |
|---|---|---|
| 0 | magic `"GLX1"` | char[4] |
| 4 | sampleRate (16000) | u32 |
| 8 | bitsPerSym (1–3) | u8 |
| 9 | alphaIdx | u8 |
| 10 | mulaw (always 1 from this encoder) | u8 |
| 11 | huff (1 = Huffman, 0 = raw fixed-width) | u8 |
| 12 | ditherType (1 = masked RPDF, 2 = spiked) | u8 |
| 13 | seed | u32 |
| 17 | numPackets | u32 |

(The `ditherType` byte was inserted at offset 12; `.glx` files written before
this change have a 20-byte header and must be re-encoded.)

**Embedded Huffman table (8 bytes, only when `huff=1`):** the 15 per-symbol
code lengths, packed as nibbles (MSB-first, symbol index = `delta + 7`). This
makes the file **self-describing** — the decoder rebuilds the canonical table
from these lengths and needs no compiled-in table.

**Payload:** `numPackets * 160` symbols. When `huff=0`, each is a
`(bitsPerSym+1)`-bit signed delta (4-bit at depth 3, as before); when
`huff=1`, each is a canonical Huffman code. MSB-first, zero-padded to a byte
at the end. A packet is 160 samples (10 ms at 16 kHz);
`numPackets = ceil(numSamples / 160)` — a trailing partial packet is
**zero-padded to a full 10 ms** rather than dropped, so the decoded file can
end with up to 10 ms of silence but never loses speech.

## Entropy coding (`compression.c` / `.h`, `build_huffman.py`)

The delta transform maps quantizer codes to differences that cluster hard
around 0 for speech. The alphabet is fixed at `[-7, +7]` (15 values, sized by
the max 3-bit depth) for **every** bit depth — lower depths simply never use
the outer symbols — so the embedded table block is always 8 bytes.
`compression.c` holds all of the entropy coding: a canonical Huffman coder
over those 15 symbols (index = `delta + 7`) that replaces the flat
fixed-width field, dropping the payload to **~1.3–1.6 bits/sample** —
bit-exact losslessly.

Only **code lengths** define a canonical table, so both sides reconstruct
codes from lengths via `glx_huff_build()`. That is what lets the table ride in
the file: the encoder embeds the 8-byte length block, the decoder rebuilds from
it. The encoder's *designed* lengths come from six static tables selected by
`glx_huff_lengths_for(bitdepth, mulaw)` — one per (bit depth, companding)
pair:

| table | mode | expected |
|---|---|---|
| `glx_huff_len_b1_mu1` / `_mu0` | 1-bit | ~1.34 / ~1.54 bits/sym |
| `glx_huff_len_b2_mu1` / `_mu0` | 2-bit | ~1.43 / ~1.49 bits/sym |
| `glx_huff_len_b3_mu1` / `_mu0` | 3-bit | ~1.62 / ~1.46 bits/sym |

All live in `glx_huff_tables.h`, generated by `build_huffman.py` — a Python
port of `lutmu.mlx`'s residual simulation. It runs each WAV through the modeled
pipeline (`[mu-law] → class-2 dither → b-bit quantize → first difference`),
averages the per-file residual PMFs (equal weight, as the `.mlx` does) at
`alpha=0.5`, and emits all six length tables. Regenerate with:

```sh
python build_huffman.py *.wav -o glx_huff_tables.h   # then rebuild the binaries
```

Every symbol gets a code, so any delta is always encodable at any settings.

## What came from where

| new file | refactored from |
|---|---|
| `mulaw.c` | `../mulaw.c` — float µ-law + LFSR dither + quantizer + delta transform; `glx_encode`/`glx_decode` take `mulaw` and `ditherType` flags, plus caller-owned dither/predictor state so they can be fed one 10 ms packet at a time |
| `bitpack.c` | packer from `../sampling.c`, unpacker from `../decompress.c`, merged |
| `wav.c` | chunk-walking reader from `../top.c` `readfile()` + `../claudius.c`; writer from `../glxdecode.c`; both converted to streaming open/read-or-write/close so no whole-file buffers are needed |
| `encoder.c` | `../top.c` main flow (arg parsing, downmix, decimate, header write) |
| `decoder.c` | `../glxdecode.c` main flow |
| `glx.h` | `../essentials.h`, `../declarations.h`, `../bitpack.h`, `../mulaw.h`, `../dithering.h`, consolidated |

## Notes on the wire format

The delta transform maps b-bit codes to differences in ±(2^b − 1). On their
own, stored as (b+1)-bit two's complement (`huff=0`), that's one bit/sample
more than the raw codes — but the point is that speech deltas cluster tightly
around 0, which the Huffman stage (`huff=1`, default) exploits to reach
~1.3–1.6 bits/sample (see the *Entropy coding* section above; supersedes the
earlier `../huffmanencoding.c` sketch). Encoder and decoder keep one
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
