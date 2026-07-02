#!/usr/bin/env python3
"""
Analyze a .glx stream to decide the best entropy-coding strategy.

Reports symbol/delta frequency maps and the *actual* cost (bits/symbol and
total bytes) of each candidate scheme, plus the entropy floors (what an
arithmetic coder could reach). Delta and context reset at each 160-symbol
packet boundary, matching the real-time / packet-independent codec design.

Usage: python analyze_glx.py [out.glx]
"""
import sys
import heapq
import math
from collections import Counter, defaultdict

PACKET_SYMBOLS = 160          # 10 ms @ 16 kHz output rate
HEADER_BYTES = 14


def read_glx(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"GLX1":
        raise SystemExit("not a GLX1 file")
    sample_rate = int.from_bytes(data[4:8], "little")
    bits_per_sym = data[8]
    alpha_idx = data[9]
    num_packets = int.from_bytes(data[10:14], "little")
    payload = data[HEADER_BYTES:]

    # unpack 3-bit symbols, MSB-first, 8 per 3 bytes (see sampling.c)
    symbols = []
    for i in range(0, len(payload) - 2, 3):
        v = (payload[i] << 16) | (payload[i + 1] << 8) | payload[i + 2]
        for k in range(8):
            symbols.append((v >> (21 - 3 * k)) & 0x7)

    meta = dict(sample_rate=sample_rate, bits_per_sym=bits_per_sym,
                alpha_idx=alpha_idx, num_packets=num_packets)
    return meta, symbols


def shannon_entropy(counts):
    """Zero-order entropy in bits/symbol (arithmetic-coding floor)."""
    total = sum(counts.values())
    if total == 0:
        return 0.0
    return -sum((c / total) * math.log2(c / total) for c in counts.values())


def huffman_avg_len(counts):
    """Average Huffman code length (bits/symbol) for these frequencies."""
    total = sum(counts.values())
    if total == 0:
        return 0.0
    freqs = [c for c in counts.values() if c > 0]
    if len(freqs) == 1:
        return 1.0                      # one symbol still needs 1 bit
    heap = list(freqs)
    heapq.heapify(heap)
    weighted_path = 0                    # sum of merged weights == total code length
    while len(heap) > 1:
        a = heapq.heappop(heap)
        b = heapq.heappop(heap)
        weighted_path += a + b
        heapq.heappush(heap, a + b)
    return weighted_path / total


def packets(symbols):
    for i in range(0, len(symbols), PACKET_SYMBOLS):
        yield symbols[i:i + PACKET_SYMBOLS]


def build_stats(symbols):
    sym_counts = Counter(symbols)

    delta_counts = Counter()            # per-packet-reset deltas (excl. first of packet)
    ctx_counts = defaultdict(Counter)   # order-1: prev symbol -> next symbol
    n_packet_firsts = 0
    for pkt in packets(symbols):
        if not pkt:
            continue
        n_packet_firsts += 1            # first symbol of packet stored raw
        prev = pkt[0]
        for s in pkt[1:]:
            delta_counts[s - prev] += 1
            ctx_counts[prev][s] += 1
            prev = s
    return sym_counts, delta_counts, ctx_counts, n_packet_firsts


def bar(p, width=40):
    return "#" * int(round(p * width))


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "out.glx"
    meta, symbols = read_glx(path)
    n = len(symbols)
    seconds = n / meta["sample_rate"]

    print(f"file            : {path}")
    print(f"sample rate     : {meta['sample_rate']} Hz")
    print(f"alpha index     : {meta['alpha_idx']}  (dither strength)")
    print(f"symbols         : {n}  ({seconds:.2f} s, {meta['num_packets']} full packets)")
    print()

    sym_counts, delta_counts, ctx_counts, n_firsts = build_stats(symbols)

    # ---- symbol frequency map ----
    print("SYMBOL FREQUENCY MAP")
    for s in range(8):
        c = sym_counts.get(s, 0)
        p = c / n
        print(f"  {s} : {p*100:6.2f}%  {bar(p)}")
    print()

    # ---- delta frequency map ----
    print("DELTA FREQUENCY MAP (per-packet reset)")
    dtot = sum(delta_counts.values())
    for d in range(-7, 8):
        c = delta_counts.get(d, 0)
        p = c / dtot if dtot else 0
        if c:
            print(f"  {d:+d} : {p*100:6.2f}%  {bar(p)}")
    print()

    # ---- entropy floors (arithmetic-coding potential) ----
    h0 = shannon_entropy(sym_counts)
    h0_delta = shannon_entropy(delta_counts)
    ctx_total = sum(sum(c.values()) for c in ctx_counts.values())
    h1 = sum(sum(c.values()) / ctx_total * shannon_entropy(c)
             for c in ctx_counts.values()) if ctx_total else 0.0

    # ---- practical Huffman costs (bits/symbol over whole stream) ----
    huff_sym = huffman_avg_len(sym_counts)                       # order-0 huffman, all symbols

    # delta + huffman: first-of-packet raw (3 bits), rest via delta huffman
    huff_delta = huffman_avg_len(delta_counts)
    delta_total_bits = n_firsts * 3 + sum(delta_counts.values()) * huff_delta

    # order-1 context huffman: first-of-packet raw, rest via context tables
    ctx_total_bits = n_firsts * 3
    for prev, c in ctx_counts.items():
        ctx_total_bits += sum(c.values()) * huffman_avg_len(c)

    raw_bits = n * 3
    huff_bits = n * huff_sym

    print("COST PER SCHEME")
    print(f"  {'scheme':<26}{'bits/sym':>10}{'bytes':>12}{'vs raw':>9}")
    def row(label, total_bits):
        print(f"  {label:<26}{total_bits/n:>10.3f}{math.ceil(total_bits/8):>12}"
              f"{raw_bits/total_bits:>8.2f}x")
    row("raw 3-bit (current)", raw_bits)
    row("huffman (order-0)", huff_bits)
    row("delta + huffman", delta_total_bits)
    row("order-1 context huffman", ctx_total_bits)
    print()
    print("ENTROPY FLOORS (arithmetic-coding potential, bits/sym)")
    print(f"  H0 symbols      : {h0:.3f}")
    print(f"  H0 deltas       : {h0_delta:.3f}")
    print(f"  H1 order-1      : {h1:.3f}")
    print()

    # ---- recommendation ----
    schemes = {
        "huffman (order-0)": huff_bits,
        "delta + huffman": delta_total_bits,
        "order-1 context huffman": ctx_total_bits,
    }
    best = min(schemes, key=schemes.get)
    print("RECOMMENDATION")
    print(f"  best practical scheme : {best}  "
          f"({raw_bits/schemes[best]:.2f}x smaller than raw 3-bit)")
    if huff_delta >= huff_sym:
        print("  note: delta does NOT help here — delta entropy >= symbol entropy")
    else:
        print("  note: delta helps (deltas are more skewed than raw symbols)")
    if h1 < h0 - 0.05:
        print("  note: strong order-1 correlation -> context modeling has real headroom")
        print("        (Huffman can't reach it; an arithmetic/range coder would)")


if __name__ == "__main__":
    main()
