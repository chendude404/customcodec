#!/usr/bin/env python3
"""
huffman_bitdepth_sweep.py — simulate Huffman coding of the codec's quantizer-code
deltas across bit depths 0..8, and plot average bits/sample vs. the dither
amplitude alpha.

  X axis: alpha (class-2 dither amplitude, 0.0 .. 1.0)
  Y axis: expected Huffman bits per sample (= per delta symbol)
  one curve per bit depth (0 .. 8)

The residual pipeline mirrors the C encoder (mulaw.c / encoder.c) exactly and is
a generalization of build_huffman.py — which is hardwired to the 3-bit alphabet
[-7,+7]. Here the alphabet grows with depth: b-bit codes span [0, 2^b - 1], so
their first differences span [-(2^b - 1), +(2^b - 1)] -> 2^(b+1) - 1 symbols.

    normalize -> decimate 48->16 kHz -> [mu-law] -> x*headroom -> +class-2 dither
    -> b-bit mid-riser quantize -> first difference -> Huffman code lengths

Run (uses the Windows py launcher, which has numpy+matplotlib here):
    py huffman_bitdepth_sweep.py [IN.wav ...] [--no-mulaw] [--out PNG]

With no WAV args it falls back to the repo's speech files.
"""

import argparse
import heapq
import os
import sys
import wave

import numpy as np

BITDEPTHS = tuple(range(0, 9))          # 0..8, inclusive, per request
DECIMATION = 3                          # 48 kHz -> 16 kHz
MU = 255.0

# repo speech files to use when none are given on the command line
_HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_WAVS = [
    os.path.join(_HERE, "118426__acclivity__evacuationadvice_48k.wav"),  # resampled 44.1->48k
    os.path.join(_HERE, "33415__acclivity__easterreading.wav"),
    os.path.join(_HERE, "84579__acclivity__abcifa-talk.wav"),
    os.path.join(_HERE, "..", "harvard_48k.wav"),                        # resampled 44.1->48k
]


# ── WAV -> normalized mono float (from build_huffman.py) ─────────────────────

def read_wav_mono(path):
    with wave.open(path, "rb") as w:
        if w.getsampwidth() != 2:
            raise ValueError(f"{path}: not 16-bit PCM")
        ch = w.getnchannels()
        rate = w.getframerate()
        raw = w.readframes(w.getnframes())
    x = np.frombuffer(raw, dtype="<i2").astype(np.float64)
    if ch > 1:
        x = x.reshape(-1, ch).mean(axis=1)
    peak = np.max(np.abs(x))
    return (x / peak if peak > 0 else x), rate


def box_decimate(x, factor):
    n = (len(x) // factor) * factor
    return x[:n].reshape(-1, factor).mean(axis=1)


def uniforms(n, seed):
    """Deterministic vectorized U[0,1) via SplitMix64 (matches build_huffman.py)."""
    z = (np.arange(n, dtype=np.uint64) + np.uint64(seed & 0xFFFFFFFFFFFFFFFF)
         + np.uint64(0x9E3779B97F4A7C15))
    z = (z ^ (z >> np.uint64(30))) * np.uint64(0xBF58476D1CE4E5B9)
    z = (z ^ (z >> np.uint64(27))) * np.uint64(0x94D049BB133111EB)
    z = z ^ (z >> np.uint64(31))
    return (z >> np.uint64(11)).astype(np.float64) * (1.0 / 9007199254740992.0)


def class2_dither(delta, n, alpha, seed):
    """RPDF scaled by alpha, gated on an alpha-fraction of samples (glx_dither_next)."""
    if alpha <= 0.0:
        return np.zeros(n)
    R = alpha * delta * (uniforms(n, seed) - 0.5)
    mask = uniforms(n, seed ^ 0xABCDEF1234567) < alpha
    return R * mask


# ── residual PMF for one file at (alpha, mulaw, bitdepth) ────────────────────

def simulate_file_pmf(x, alpha, mulaw, bitdepth, seed, headroom=True):
    """Return (probs, bias) for the delta histogram. Alphabet is sized to the
    depth: idx = delta + bias, bias = nlev-1, length = 2*nlev-1."""
    if mulaw:
        x = np.sign(x) * np.log1p(MU * np.abs(x)) / np.log1p(MU)

    nlev = 1 << bitdepth
    delta = 2.0 / nlev
    hrf = (1.0 - alpha * delta * 0.5) if headroom else 1.0   # mulaw.c:52

    xd = x * hrf + class2_dither(delta, len(x), alpha, seed)
    code = np.floor((xd + 1.0) / delta).astype(np.int64)     # glx_quantize
    np.clip(code, 0, nlev - 1, out=code)

    deltas = np.empty_like(code)                             # glx_delta_encode
    deltas[0] = code[0]                                      # prev<0 -> absolute
    deltas[1:] = code[1:] - code[:-1]

    bias = nlev - 1
    nsym = 2 * nlev - 1
    idx = deltas + bias
    counts = np.bincount(idx, minlength=nsym).astype(np.float64)
    return counts / counts.sum(), bias


# ── Huffman code lengths over an arbitrary alphabet ──────────────────────────

def code_lengths(weights):
    """Standard Huffman lengths. Only symbols with nonzero weight get a leaf;
    a single occupied symbol costs 0 bits (no information to transmit)."""
    occupied = [s for s, w in enumerate(weights) if w > 0]
    if len(occupied) <= 1:
        return {s: 0 for s in occupied}         # degenerate source: 0 bits/sym

    counter = 0
    heap = []
    for s in occupied:
        heapq.heappush(heap, (float(weights[s]), counter, {s: 0}))
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
    return {s: max(1, depth) for s, depth in heap[0][2].items()}


def expected_bits(probs, mulaw, bitdepth, seed, headroom):
    lengths = code_lengths(probs)
    return float(sum(lengths.get(s, 0) * probs[s] for s in range(len(probs))))


# ── sweep + plot ─────────────────────────────────────────────────────────────

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="*", default=None,
                    help="16-bit PCM WAV files (default: repo speech files)")
    ap.add_argument("--no-mulaw", action="store_true", help="disable mu-law companding")
    ap.add_argument("--no-headroom", action="store_true",
                    help="skip the x*hrf headroom factor (matches build_huffman.py)")
    ap.add_argument("--alpha-steps", type=int, default=21, help="alpha grid points 0..1")
    ap.add_argument("--seed", type=int, default=12345)
    ap.add_argument("--out", default=os.path.join(_HERE, "huffman_bits_vs_alpha.png"))
    ap.add_argument("--csv", default=None, help="also write the sweep as CSV")
    args = ap.parse_args(argv)

    mulaw = not args.no_mulaw
    headroom = not args.no_headroom
    inputs = args.inputs or DEFAULT_WAVS

    # load + decimate each file once
    signals = []
    for path in inputs:
        try:
            x, rate = read_wav_mono(path)
        except (ValueError, OSError, wave.Error) as e:
            print(f"skip: {e}", file=sys.stderr)
            continue
        if rate != 48000:
            print(f"note: {os.path.basename(path)} is {rate} Hz, not 48k "
                  f"(decimating by {DECIMATION} anyway)", file=sys.stderr)
        signals.append((os.path.basename(path), box_decimate(x, DECIMATION)))
    if not signals:
        ap.error("no usable WAV inputs")
    print(f"using {len(signals)} file(s): {', '.join(n for n, _ in signals)}")

    alphas = np.linspace(0.0, 1.0, args.alpha_steps)
    curves = {b: np.zeros(len(alphas)) for b in BITDEPTHS}

    for b in BITDEPTHS:
        for j, alpha in enumerate(alphas):
            # average the per-file PMF (equal weight), then Huffman-code it —
            # same methodology as build_huffman.py
            pmfs = []
            for fi, (_, sig) in enumerate(signals):
                probs, _ = simulate_file_pmf(sig, alpha, mulaw, b,
                                             args.seed + fi * 0x1000, headroom)
                pmfs.append(probs)
            probs = np.mean(pmfs, axis=0)
            curves[b][j] = expected_bits(probs, mulaw, b, args.seed, headroom)
        print(f"  b={b}: {curves[b][0]:.3f} .. {curves[b][-1]:.3f} bits/sample "
              f"(alpha 0..1)")

    if args.csv:
        with open(args.csv, "w", newline="\n") as fp:
            fp.write("alpha," + ",".join(f"b{b}" for b in BITDEPTHS) + "\n")
            for j, a in enumerate(alphas):
                fp.write(f"{a:.4f}," +
                         ",".join(f"{curves[b][j]:.5f}" for b in BITDEPTHS) + "\n")
        print(f"wrote {args.csv}")

    # ── plot ──
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(figsize=(10, 6))
    cmap = plt.get_cmap("viridis")
    for i, b in enumerate(BITDEPTHS):
        color = cmap(i / (len(BITDEPTHS) - 1))
        ax.plot(alphas, curves[b], marker="o", ms=3, lw=1.8,
                color=color, label=f"{b}-bit")
        # label the exact value at the start (alpha=0) and end (alpha=1)
        ax.annotate(f"{curves[b][0]:.3f}", (alphas[0], curves[b][0]),
                    textcoords="offset points", xytext=(-6, 0),
                    ha="right", va="center", fontsize=7, color=color)
        ax.annotate(f"{curves[b][-1]:.3f}", (alphas[-1], curves[b][-1]),
                    textcoords="offset points", xytext=(6, 0),
                    ha="left", va="center", fontsize=7, color=color)
    ax.set_xlim(alphas[0] - 0.16, alphas[-1] + 0.16)   # room for endpoint labels
    ax.set_xlabel("alpha  (class-2 dither amplitude)")
    ax.set_ylabel("average Huffman bits / sample")
    comp = "mu-law" if mulaw else "linear"
    ax.set_title(f"Huffman-coded delta cost vs. alpha  ({comp}, "
                 f"{len(signals)} file(s))")
    ax.grid(True, alpha=0.3)
    ax.legend(title="quantizer depth", ncol=2, fontsize=9)
    fig.tight_layout()
    fig.savefig(args.out, dpi=130)
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
