#!/usr/bin/env python3
"""
build_huffman.py — compile the static Huffman code-length tables for the codec.

The compression stage emits one symbol per sample: the delta of the 3-bit
quantizer codes, spanning [-7, +7] (15 values) and clustering hard around 0.

PMF SOURCE — a Python port of `lutmu.mlx`'s residual simulation (NOT a byte-count
of pre-encoded .glx files). For each WAV it runs the modeled pipeline:

    normalize -> decimate 48->16 kHz -> [mu-law] -> class-2 dither
    -> 3-bit mid-riser quantize -> first difference -> histogram over [-7,+7]

Per-file PMFs are averaged with equal weight (as lutmu.mlx does). Two tables are
produced at alpha=0.5, one with mu-law companding and one without.

Only CODE LENGTHS are emitted. The canonical codes and decode tables are rebuilt
from the lengths by glx_huff_build() in compression.c — the same reconstruction
the decoder runs on the length block embedded in each .glx file, so encoder and
decoder are consistent by construction.

Usage:
    python build_huffman.py IN.wav [IN.wav ...] [-o glx_huff_tables.h] [options]

Options:
    -o, --out PATH     output header (default: glx_huff_tables.h next to script)
    --alpha FLOAT      class-2 dither amplitude (codec idx 6 = 0.5)   (default: 0.5)
    --seconds FLOAT    per-file seconds to simulate; 0 = whole file   (default: 0)
    --seed INT         RNG seed for the dither draw                   (default: 12345)
"""

import argparse
import heapq
import os
import sys
import wave

import numpy as np

# Alphabet: the delta values themselves. Differences of 3-bit codes span
# [-7, +7] (15 values); the first symbol is a raw code in [0, 7], also in range.
# Tables are indexed by (delta + BIAS) -> [0, 14].
BITDEPTH = 3
NSYM = 15
BIAS = 7
DECIMATION = 3
MU = 255.0


# ── WAV -> normalized mono float ─────────────────────────────────────────────

def read_wav_mono(path):
    with wave.open(path, "rb") as w:
        if w.getsampwidth() != 2:
            raise ValueError(f"{path}: not 16-bit PCM")
        ch = w.getnchannels()
        raw = w.readframes(w.getnframes())
    x = np.frombuffer(raw, dtype="<i2").astype(np.float64)
    if ch > 1:
        x = x.reshape(-1, ch).mean(axis=1)
    peak = np.max(np.abs(x))
    return x / peak if peak > 0 else x


# ── lutmu.mlx pipeline (per file) ────────────────────────────────────────────

def box_decimate(x, factor):
    """Codec's box-filter decimation: mean of each group of `factor` samples."""
    n = (len(x) // factor) * factor
    return x[:n].reshape(-1, factor).mean(axis=1)


def uniforms(n, seed):
    """Deterministic vectorized U[0,1) via SplitMix64 (numpy.random is broken in
    this MSYS2 build). Fine for dither: only the ~uniform marginal matters."""
    z = (np.arange(n, dtype=np.uint64) + np.uint64(seed & 0xFFFFFFFFFFFFFFFF)
         + np.uint64(0x9E3779B97F4A7C15))
    z = (z ^ (z >> np.uint64(30))) * np.uint64(0xBF58476D1CE4E5B9)
    z = (z ^ (z >> np.uint64(27))) * np.uint64(0x94D049BB133111EB)
    z = z ^ (z >> np.uint64(31))
    return (z >> np.uint64(11)).astype(np.float64) * (1.0 / 9007199254740992.0)


def class2_dither(delta, n, alpha, seed):
    """RPDF2 from lutmu.mlx: RPDF scaled by alpha, applied to an alpha-fraction
    of samples. The threshold mask matches glx_dither_next (mask = u < alpha)."""
    R = alpha * delta * (uniforms(n, seed) - 0.5)
    mask = uniforms(n, seed ^ 0xABCDEF1234567) < alpha
    return R * mask


def simulate_file_pmf(path, alpha, mulaw, seconds, seed):
    """Return the 15-bin delta PMF for one file via the lutmu.mlx model."""
    x = read_wav_mono(path)
    x = box_decimate(x, DECIMATION)                 # 48 -> 16 kHz
    if seconds and seconds > 0:
        x = x[:int(seconds * 16000)]
    if mulaw:
        x = np.sign(x) * np.log1p(MU * np.abs(x)) / np.log1p(MU)

    nlev = 1 << BITDEPTH
    delta = 2.0 / nlev
    xd = x + class2_dither(delta, len(x), alpha, seed)

    code = np.floor((xd + 1.0) / delta).astype(np.int64)   # matches glx_quantize
    np.clip(code, 0, nlev - 1, out=code)

    deltas = np.empty_like(code)                    # matches glx_delta_encode
    deltas[0] = code[0]
    deltas[1:] = code[1:] - code[:-1]

    idx = deltas + BIAS
    if idx.min() < 0 or idx.max() >= NSYM:
        raise ValueError(f"{path}: delta out of [-7,7]")
    counts = np.bincount(idx, minlength=NSYM).astype(np.float64)
    return counts / counts.sum()


# ── Huffman code lengths ─────────────────────────────────────────────────────

def code_lengths(weights):
    """Standard Huffman code lengths over the 15-symbol alphabet. Every symbol
    gets a leaf (hence a length >= 1), so any delta is always encodable."""
    counter = 0
    heap = []
    for sym in range(NSYM):
        heapq.heappush(heap, (float(weights[sym]), counter, {sym: 0}))
        counter += 1
    while len(heap) > 1:
        f1, _, d1 = heapq.heappop(heap)
        f2, _, d2 = heapq.heappop(heap)
        merged = {}
        for d in (d1, d2):
            for sym, depth in d.items():
                merged[sym] = depth + 1
        heapq.heappush(heap, (f1 + f2, counter, merged))
        counter += 1
    lengths = [0] * NSYM
    for sym, depth in heap[0][2].items():
        lengths[sym] = max(1, depth)
    return lengths


def expected_bits(lengths, probs):
    return float(sum(lengths[s] * probs[s] for s in range(NSYM)))


# ── header emission ──────────────────────────────────────────────────────────

def emit_tables(path, results, meta):
    """results: {mulaw: (probs, lengths)}."""
    def arr(vals):
        return ", ".join(str(v) for v in vals)

    eb1 = expected_bits(*reversed(results[1]))  # (lengths, probs)
    eb0 = expected_bits(*reversed(results[0]))

    L = []
    L.append("/* glx_huff_tables.h - GENERATED by build_huffman.py. Do not edit by hand. */")
    L.append("/*")
    L.append(" * Static Huffman code-LENGTH tables for the .glx delta alphabet")
    L.append(f" * ({NSYM} values in [-7,+7], symbol index = delta + {BIAS}). Canonical codes")
    L.append(" * are rebuilt from these lengths by glx_huff_build() in compression.c.")
    L.append(f" * Source: lutmu.mlx residual sim over {meta['nfiles']} file(s), alpha={meta['alpha']},")
    L.append(f" * {'whole-file' if not meta['seconds'] else str(meta['seconds'])+'s/file'}.")
    L.append(f" *   mu-law on : expected {eb1:.3f} bits/symbol")
    L.append(f" *   mu-law off: expected {eb0:.3f} bits/symbol")
    L.append(" */")
    L.append("#ifndef GLX_HUFF_TABLES_H")
    L.append("#define GLX_HUFF_TABLES_H")
    L.append("")
    L.append("#include <stdint.h>")
    L.append("")
    L.append("/* code length per symbol index (delta + 7); mu-law companded path */")
    L.append(f"static const uint8_t glx_huff_len_mu1[{NSYM}] = {{ {arr(results[1][1])} }};")
    L.append("")
    L.append("/* code length per symbol index (delta + 7); linear (no companding) path */")
    L.append(f"static const uint8_t glx_huff_len_mu0[{NSYM}] = {{ {arr(results[0][1])} }};")
    L.append("")
    L.append("#endif /* GLX_HUFF_TABLES_H */")
    L.append("")
    with open(path, "w", newline="\n", encoding="ascii") as fp:
        fp.write("\n".join(L))
    return eb1, eb0


# ── CLI ──────────────────────────────────────────────────────────────────────

def main(argv=None):
    ap = argparse.ArgumentParser(description="Compile Huffman length tables for the .glx codec.")
    ap.add_argument("inputs", nargs="+", help="WAV files to simulate the residual PMF over")
    ap.add_argument("-o", "--out", default=None, help="output header path")
    ap.add_argument("--alpha", type=float, default=0.5)
    ap.add_argument("--seconds", type=float, default=0.0)
    ap.add_argument("--seed", type=int, default=12345)
    args = ap.parse_args(argv)

    if args.out is None:
        here = os.path.dirname(os.path.abspath(__file__))
        args.out = os.path.join(here, "glx_huff_tables.h")

    results = {}
    nfiles = 0
    for mulaw in (1, 0):
        per_file = []
        for fi, path in enumerate(args.inputs):
            try:
                pmf = simulate_file_pmf(path, args.alpha, mulaw, args.seconds,
                                        args.seed + fi * 0x1000)
            except (ValueError, OSError, wave.Error) as e:
                print(f"skip: {e}", file=sys.stderr)
                continue
            per_file.append(pmf)
        if not per_file:
            ap.error("no usable WAV inputs")
        probs = np.mean(per_file, axis=0)
        results[mulaw] = (probs, code_lengths(probs))
        nfiles = len(per_file)

    meta = {"nfiles": nfiles, "alpha": args.alpha, "seconds": args.seconds}
    eb1, eb0 = emit_tables(args.out, results, meta)

    for mulaw, tag in ((1, "mu-law on "), (0, "mu-law off")):
        probs, lengths = results[mulaw]
        print(f"\n[{tag}] residual PMF (index -> delta -> P, code length):")
        for s in range(NSYM):
            print(f"  idx {s:2d} (d={s-BIAS:+d})  P={probs[s]:7.4f}  len={lengths[s]:2d}")
        print(f"  expected {expected_bits(lengths, probs):.3f} bits/symbol")
    print(f"\nwrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
