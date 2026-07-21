#!/usr/bin/env python3
"""
glx_batch.py — Python harness for the .glx codec (FableImplementation/our.exe).

The C encoder (`our.exe our.wav alpha seed bitdepth dither out.glx huff mulaw`)
accepts ONLY 48 kHz / 16-bit PCM WAV (mono or stereo). This wrapper uses ffprobe/ffmpeg to
condition an arbitrary pile of .wav files to meet that contract, then runs each
through the encoder:

    * 8-bit PCM        -> thrown out (per spec)
    * 16-bit PCM       -> kept (still normalized to 48 kHz if needed)
    * 24 / 32-bit int  -> quantized down to 16-bit by ffmpeg
    * 32 / 64-bit float-> quantized down to 16-bit by ffmpeg
    * rate != 48 kHz   -> resampled up/down to 48 kHz by ffmpeg
    * > 2 channels     -> downmixed to mono (codec handles mono/stereo itself)

Requires ffmpeg + ffprobe on PATH (or pass --ffmpeg / --ffprobe). Only the
Python standard library is used otherwise.

Usage:
    python glx_batch.py INPUT [INPUT ...] -o OUTDIR [options]

INPUT may be a .wav file, a directory (searched recursively), or a glob.

Options:
    -o, --outdir DIR   where .glx files are written        (default: ./glx_out)
    --alpha N          dither index 1->0.0 .. 11->1.0       (default: 6  = 0.5)
    --seed N           LFSR dither seed, must be nonzero    (default: 12345)
    --bits {1,2,3}     quantizer bits per sample            (default: 3)
    --dither {1,2}     1 = masked RPDF, 2 = spiked          (default: 1)
    --huff {0,1}       1 = Huffman-code deltas, 0 = raw     (default: 1)
    --mulaw {0,1}      1 = mu-law companding, 0 = linear    (default: 0)
    --exe PATH         path to encoder (default: our.exe next to this script)
    --ffmpeg PATH      ffmpeg binary                        (default: ffmpeg)
    --ffprobe PATH     ffprobe binary                       (default: ffprobe)
    --keep-wav         keep the conditioned 48k/16-bit WAVs
    --jobs N           parallel workers                     (default: cpu_count)
    -v, --verbose      print encoder stdout for each file
"""

import argparse
import glob as globmod
import json
import os
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor, as_completed

TARGET_RATE = "48000"


class Result:
    def __init__(self, src, status, detail=""):
        self.src = src
        self.status = status   # "ok" | "skip" | "error"
        self.detail = detail


# ── ffprobe inspection ──────────────────────────────────────────────────────

def probe(path, ffprobe):
    """Return the first audio stream's properties as a dict, or raise."""
    cmd = [ffprobe, "-v", "error", "-select_streams", "a:0",
           "-show_entries",
           "stream=codec_name,sample_fmt,sample_rate,channels,"
           "bits_per_sample,bits_per_raw_sample",
           "-of", "json", path]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError((proc.stderr or "ffprobe failed").strip().splitlines()[-1])
    streams = json.loads(proc.stdout or "{}").get("streams", [])
    if not streams:
        raise RuntimeError("no audio stream")
    return streams[0]


def stream_bits(info):
    """Best-effort bit depth from an ffprobe stream dict."""
    for key in ("bits_per_sample", "bits_per_raw_sample"):
        val = info.get(key)
        if val not in (None, "0", 0):
            return int(val)
    # fall back to the sample_fmt name (u8/s16/s32/flt/dbl, plus 'p' planar)
    return {"u8": 8, "s16": 16, "s32": 32,
            "flt": 32, "dbl": 64}.get((info.get("sample_fmt") or "").rstrip("p"), 0)


def is_8bit_pcm(info):
    codec = info.get("codec_name", "")
    fmt = (info.get("sample_fmt") or "").rstrip("p")
    return codec.startswith("pcm_") and (fmt == "u8" or stream_bits(info) == 8)


# ── per-file pipeline ───────────────────────────────────────────────────────

def process_one(src, args, tmpdir):
    base = os.path.splitext(os.path.basename(src))[0]

    try:
        info = probe(src, args.ffprobe)
    except (RuntimeError, json.JSONDecodeError) as e:
        return Result(src, "error", f"probe: {e}")

    # spec: throw out 8-bit PCM audio
    if is_8bit_pcm(info):
        return Result(src, "skip", "8-bit PCM discarded")

    try:
        in_rate = int(info.get("sample_rate") or 0)
        in_ch = int(info.get("channels") or 0)
    except ValueError:
        in_rate = in_ch = 0
    bits = stream_bits(info)

    # ffmpeg conditions: -> 48 kHz, 16-bit signed PCM, mono/stereo preserved
    # (codec downmixes stereo); anything with >2 channels is folded to mono.
    wav48 = os.path.join(args.outdir if args.keep_wav else tmpdir,
                         base + ".48k16.wav")
    ff = [args.ffmpeg, "-y", "-v", "error", "-i", src, "-map", "a:0",
          "-ar", TARGET_RATE, "-c:a", "pcm_s16le"]
    if in_ch > 2:
        ff += ["-ac", "1"]
    ff.append(wav48)

    proc = subprocess.run(ff, capture_output=True, text=True)
    if proc.returncode != 0 or not os.path.exists(wav48):
        msg = (proc.stderr or "ffmpeg failed").strip().splitlines()
        return Result(src, "error", f"ffmpeg: {msg[-1] if msg else 'no output'}")

    out_ch = 1 if in_ch > 2 else in_ch
    note = (f"{info.get('codec_name','?')}/{bits or '?'}bit "
            f"{in_rate or '?'}Hz {in_ch or '?'}ch "
            f"-> 48000/16 {out_ch or '?'}ch")

    # encode: our.exe wav alpha seed bitdepth dither out.glx huff mulaw
    out_glx = os.path.join(args.outdir, base + ".glx")
    cmd = [args.exe, wav48, str(args.alpha), str(args.seed),
           str(args.bits), str(args.dither), out_glx,
           str(args.huff), str(args.mulaw)]
    enc = subprocess.run(cmd, capture_output=True, text=True)

    if not args.keep_wav:
        try:
            os.remove(wav48)
        except OSError:
            pass

    if enc.returncode != 0 or not os.path.exists(out_glx):
        tail = (enc.stdout or "").strip().splitlines()
        return Result(src, "error", f"{note} | encoder rc={enc.returncode}: "
                                    f"{tail[-1] if tail else 'no output'}")

    size = os.path.getsize(out_glx)
    detail = f"{note} -> {os.path.basename(out_glx)} ({size} B)"
    if args.verbose:
        detail += "\n    " + (enc.stdout or "").strip().replace("\n", "\n    ")
    return Result(src, "ok", detail)


# ── input discovery + CLI ───────────────────────────────────────────────────

def gather_inputs(inputs):
    found = []
    for item in inputs:
        if os.path.isdir(item):
            for root, _dirs, files in os.walk(item):
                for name in files:
                    if name.lower().endswith(".wav"):
                        found.append(os.path.join(root, name))
        elif any(ch in item for ch in "*?[") and not os.path.exists(item):
            found.extend(globmod.glob(item, recursive=True))
        elif os.path.isfile(item):
            found.append(item)
        else:
            print(f"warning: no such input: {item}", file=sys.stderr)
    seen, out = set(), []
    for p in found:
        ap = os.path.abspath(p)
        if ap not in seen:
            seen.add(ap)
            out.append(p)
    return out


def resolve_tool(name, override):
    if override:
        return override
    found = shutil.which(name)
    return found or name


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Batch-encode WAV files through the .glx codec via ffmpeg.")
    ap.add_argument("inputs", nargs="+", help="wav files, directories, or globs")
    ap.add_argument("-o", "--outdir", default="glx_out", help="output directory")
    ap.add_argument("--alpha", type=int, default=6, help="dither index 1..11")
    ap.add_argument("--seed", type=int, default=12345, help="dither seed (nonzero)")
    ap.add_argument("--bits", type=int, choices=(1, 2, 3), default=3,
                    help="quantizer bits per sample")
    ap.add_argument("--dither", type=int, choices=(1, 2), default=1,
                    help="dither PDF: 1 = masked RPDF, 2 = spiked")
    ap.add_argument("--huff", type=int, choices=(0, 1), default=1,
                    help="1 = Huffman-code deltas (target bitrate), 0 = raw fixed-width")
    ap.add_argument("--mulaw", type=int, choices=(0, 1), default=0,
                    help="1 = mu-law companding, 0 = linear")
    ap.add_argument("--exe", default=None,
                    help="path to encoder (default: our.exe next to this script)")
    ap.add_argument("--ffmpeg", default=None)
    ap.add_argument("--ffprobe", default=None)
    ap.add_argument("--keep-wav", action="store_true",
                    help="keep the conditioned 48k/16-bit WAVs")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)

    if args.seed == 0:
        ap.error("--seed must be nonzero (the dither RNG breaks on 0)")

    if args.exe is None:
        here = os.path.dirname(os.path.abspath(__file__))
        args.exe = os.path.join(here, "our.exe")
    if not os.path.exists(args.exe):
        ap.error(f"encoder not found: {args.exe}")

    args.ffmpeg = resolve_tool("ffmpeg", args.ffmpeg)
    args.ffprobe = resolve_tool("ffprobe", args.ffprobe)
    for tool in (args.ffmpeg, args.ffprobe):
        if shutil.which(tool) is None and not os.path.exists(tool):
            ap.error(f"not found on PATH: {tool}")

    inputs = gather_inputs(args.inputs)
    if not inputs:
        ap.error("no .wav inputs found")

    os.makedirs(args.outdir, exist_ok=True)

    results = []
    with tempfile.TemporaryDirectory(prefix="glx_") as tmpdir:
        with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            futs = {pool.submit(process_one, src, args, tmpdir): src
                    for src in inputs}
            for fut in as_completed(futs):
                res = fut.result()
                results.append(res)
                tag = {"ok": "OK  ", "skip": "SKIP", "error": "ERR "}[res.status]
                print(f"[{tag}] {os.path.basename(res.src)}: {res.detail}")

    ok = sum(r.status == "ok" for r in results)
    skip = sum(r.status == "skip" for r in results)
    err = sum(r.status == "error" for r in results)
    print(f"\n{ok} encoded, {skip} skipped, {err} errored "
          f"(of {len(results)}) -> {os.path.abspath(args.outdir)}")
    return 1 if err else 0


if __name__ == "__main__":
    sys.exit(main())
