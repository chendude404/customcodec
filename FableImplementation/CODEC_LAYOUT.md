# GLX Codec — Optimal Block Layout

Target design for the `.glx` speech codec: **48 kHz → 16 kHz, µ-law (optional) → dithered
quantizer → delta → tANS entropy coder**. This document captures the *optimal* pipeline —
i.e. the current design with the entropy stage moved from canonical Huffman to **tANS** — and
pins down the stage ordering and the invariants that keep encode/decode exactly reversible.

The key correctness idea: the pipeline is two **independent domains** stacked in series.
The **sample domain** (companding, headroom, dither, quantize) and the **symbol domain**
(delta, entropy code) never overlap. The entropy coder never sees dither; the dither logic
never sees an entropy symbol.

---

## 1. Encode path

```mermaid
flowchart TD
    A["WAV in<br/>48 kHz · 16-bit PCM · mono"] --> B["Packetize<br/>480 samples = 10 ms"]
    B --> C["Box-filter decimate ÷3<br/>48 kHz → 16 kHz<br/>160 samples/packet"]

    subgraph SD["SAMPLE DOMAIN — glx_encode(), per sample"]
        direction TB
        C --> D["pcm16 → float [-1, 1)"]
        D --> E["µ-law compress<br/>log1pf · FLOPs<br/>(skipped if mulaw=0)"]
        E --> F["× headroom factor<br/>hrf = 1 − α·Δ/2"]
        F --> G["+ dither d<br/>LFSR(seed), α, ditherType<br/>peak ±α·Δ/2"]
        G --> H["quantize → code<br/>0 … nlev−1, nlev = 2^bitdepth"]
    end

    H --> I["glx_delta_encode<br/>δ = code − prev<br/>δ ∈ [-7, +7], prev chained"]

    subgraph YD["SYMBOL DOMAIN — entropy stage"]
        direction TB
        I --> J["bias to symbol<br/>s = δ + 7  →  0 … 14"]
        J --> K["tANS encode, REVERSED<br/>(LIFO: encode last→first)<br/>renorm bit I/O keeps state in [L, b·L)"]
    end

    K --> L["Write:<br/>header · normalized-freq table · final state · bitstream"]
    L --> M["out.glx"]
```

### Why the stage order is what it is
- **µ-law before headroom/dither** — companding must act on the true signal; the quantizer
  operates in the companded domain.
- **Headroom before dither** — `hrf` reserves exactly `±α·Δ/2` so the *dithered, full-scale*
  sample lands on the quantizer rail without saturating (`hrf = 1 − α·Δ/2`). For `α ≤ 0.5`
  this maps the decode round-trip exactly into `[-1, 1]` with no clip.
- **Delta before entropy** — the delta transform (`prev` chained across packets) is
  independent of entropy-coding order, so it stays forward even though tANS encodes reversed.

---

## 2. Decode path (mirror image)

```mermaid
flowchart TD
    A["in.glx"] --> B["Read header + freq table + final state"]

    subgraph YD["SYMBOL DOMAIN — entropy stage"]
        direction TB
        B --> C["tANS decode, FORWARD<br/>state → symbol, then renorm-read bits<br/>(forward because encode was reversed)"]
        C --> D["unbias<br/>δ = s − 7"]
    end

    D --> E["glx_delta_decode<br/>code = prev + δ, prev chained"]

    subgraph SD["SAMPLE DOMAIN — glx_decode(), per sample"]
        direction TB
        E --> F["dequantize → level center<br/>q = −1 + (code+0.5)·Δ"]
        F --> G["− dither d<br/>same LFSR(seed) REPLAY → cancels"]
        G --> H["÷ headroom factor<br/>c = (q − d) / hrf"]
        H --> I["µ-law expand<br/>expm1f · FLOPs<br/>(skipped if mulaw=0)"]
        I --> J["float → pcm16<br/>clamp to ±full-scale"]
    end

    J --> K["WAV out<br/>16 kHz · 16-bit PCM"]
```

### The reversibility contract
| Encode step | Decode step | Cancels because |
|---|---|---|
| `+ dither d` | `− dither d` | identical LFSR seed + fixed draw order → same `d` on both sides |
| `× hrf` | `÷ hrf` | `hrf` derived from same `α`, `bitdepth` |
| µ-law compress | µ-law expand | exact analytic inverse (`log1pf` ↔ `expm1f`) |
| quantize | dequantize | lossy — the *only* intended information loss |

---

## 3. Entropy stage detail — tANS (the "optimal" change)

Replaces canonical Huffman. Motivated by the **peaky** delta distribution: `δ = 0` dominates,
so Huffman's integer-length floor wastes a full bit where the ideal is `−log₂P(0) < 1` bit.
tANS recovers those fractional bits.

```mermaid
flowchart LR
    subgraph BUILD["Offline (build step)"]
        A1["designed distribution<br/>per (bitdepth, mulaw)"] --> A2["normalize freqs<br/>Σf = M = 2^k"]
        A2 --> A3["emit freq table<br/>→ embedded in .glx header"]
    end

    subgraph ENC["Encoder (per packet, reversed)"]
        B1["state x ∈ [L, b·L)"] --> B2["renorm: emit low bits of x<br/>until x fits symbol range"]
        B2 --> B3["x ← C(s, x) table step"]
    end

    subgraph DEC["Decoder (per packet, forward)"]
        C1["state x"] --> C2["(s, x') ← D(x) table step"]
        C2 --> C3["renorm: read bits → lift x' to [L, b·L)"]
    end

    A3 -.-> ENC
    A3 -.-> DEC
```

**Design choices that make tANS cheap here**
- **Alphabet = 15 symbols** (δ ∈ [-7,7]) → tables at `M = 1024–4096` are tiny and cache-resident.
- **Static, designed distributions** — same input as today's `build_huffman.py`, emitted as
  normalized frequencies instead of code lengths. No adaptive modeling.
- **Self-describing** — the freq table rides in the header (≈23–30 bytes), so the decoder
  needs no compiled-in table, exactly like today's 8-byte length block.

**The one structural cost: ANS is LIFO.** Decode recovers symbols in reverse of encode order,
so the encoder buffers a packet's 160 deltas and encodes them **last→first**; the decoder then
reads **forward**. Recommended granularity: **per-packet ANS blocks** (flush final state each
10 ms packet) to preserve the streaming packet model and truncation recovery.

---

## 4. Wire format (`.glx`)

```
┌──────────────────────────────────────────────────────────────┐
│ Header (21 B, little-endian)                                   │
│   magic "GLX1" · sampleRate(16000) · bitsPerSym · alphaIdx     │
│   · mulaw · huff/entropy-mode · ditherType · seed · numPackets │
├──────────────────────────────────────────────────────────────┤
│ Entropy table block (self-describing)                          │
│   Huffman:  8 B packed code-lengths     (current)              │
│   tANS:    ~23–30 B normalized freqs    (optimal)             │
├──────────────────────────────────────────────────────────────┤
│ Payload                                                        │
│   per-packet entropy-coded delta symbols (+ ANS final state)   │
└──────────────────────────────────────────────────────────────┘
```

---

## 5. Fixed design parameters (from `glx.h`)

| Constant | Value | Meaning |
|---|---|---|
| `GLX_IN_RATE` | 48000 | input rate — **48 kHz only** by design |
| `GLX_OUT_RATE` | 16000 | output rate after decimation |
| `GLX_DECIMATION` | 3 | 48 → 16 kHz box-filter decimate |
| `GLX_FRAMES_PER_PACKET` | 160 | 10 ms packet at 16 kHz |
| `GLX_BITS_MIN … MAX` | 1 … 3 | quantizer bit depth |
| `GLX_MU` | 255 | µ-law µ |
| `GLX_HUFF_NSYM` | 15 | delta alphabet size (δ ∈ [-7,7]) |
| `hrf` | `1 − α·Δ/2` | headroom; no round-trip clip when `α ≤ 0.5` |

> **Note:** "normalization" (Σf = M) and "renormalization" (state bit-I/O) are both entirely
> inside the **symbol-domain** entropy stage. They are upstream of — and independent from — the
> sample-domain dither subtraction in `glx_decode`. The entropy coder never sees dither.
