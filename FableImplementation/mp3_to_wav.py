#!/usr/bin/env python3
"""
mp3_to_wav.py — convert MP3 files to WAV for the .glx codec.

The .glx encoder accepts ONLY 48 kHz / 16-bit PCM WAV (mono or stereo), so by
default this script decodes each MP3 straight into that contract via ffmpeg.
The output can be handed directly to glx_batch.py / our.exe.

Requires ffmpeg on PATH (or pass --ffmpeg). Only the Python standard library is
used otherwise.

Usage:
    python mp3_to_wav.py INPUT [INPUT ...] [-o OUTDIR] [options]

INPUT may be an .mp3 file, a directory (searched recursively), or a glob.

Options:
    -o, --outdir DIR   where .wav files are written        (default: alongside src)
    --rate N           output sample rate in Hz            (default: 48000)
    --channels {1,2}   force mono/stereo (default: keep source layout)
    --ffmpeg PATH      ffmpeg binary                       (default: ffmpeg)
    --jobs N           parallel workers                    (default: cpu_count)
    --overwrite        overwrite existing .wav outputs
    -v, --verbose      print ffmpeg stderr for each file
"""

import argparse
import glob as globmod
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed


class Result:
    def __init__(self, src, status, detail=""):
        self.src = src
        self.status = status   # "ok" | "skip" | "error"
        self.detail = detail


# ── per-file conversion ─────────────────────────────────────────────────────

def convert_one(src, args):
    base = os.path.splitext(os.path.basename(src))[0]
    outdir = args.outdir if args.outdir else os.path.dirname(os.path.abspath(src))
    dst = os.path.join(outdir, base + ".wav")

    if os.path.exists(dst) and not args.overwrite:
        return Result(src, "skip", f"exists: {os.path.basename(dst)} (use --overwrite)")

    # decode -> target rate, 16-bit signed PCM; keep channel layout unless forced
    ff = [args.ffmpeg, "-y", "-v", "error", "-i", src, "-map", "a:0",
          "-ar", str(args.rate), "-c:a", "pcm_s16le"]
    if args.channels:
        ff += ["-ac", str(args.channels)]
    ff.append(dst)

    proc = subprocess.run(ff, capture_output=True, text=True)
    if proc.returncode != 0 or not os.path.exists(dst):
        msg = (proc.stderr or "ffmpeg failed").strip().splitlines()
        return Result(src, "error", f"ffmpeg: {msg[-1] if msg else 'no output'}")

    size = os.path.getsize(dst)
    detail = f"-> {os.path.basename(dst)} ({size} B, {args.rate} Hz/16-bit)"
    if args.verbose and proc.stderr.strip():
        detail += "\n    " + proc.stderr.strip().replace("\n", "\n    ")
    return Result(src, "ok", detail)


# ── input discovery + CLI ───────────────────────────────────────────────────

def gather_inputs(inputs):
    found = []
    for item in inputs:
        if os.path.isdir(item):
            for root, _dirs, files in os.walk(item):
                for name in files:
                    if name.lower().endswith(".mp3"):
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
        description="Convert MP3 files to 48 kHz/16-bit WAV for the .glx codec.")
    ap.add_argument("inputs", nargs="+", help="mp3 files, directories, or globs")
    ap.add_argument("-o", "--outdir", default=None,
                    help="output directory (default: alongside each source)")
    ap.add_argument("--rate", type=int, default=48000,
                    help="output sample rate in Hz (default: 48000)")
    ap.add_argument("--channels", type=int, choices=(1, 2), default=None,
                    help="force mono/stereo (default: keep source layout)")
    ap.add_argument("--ffmpeg", default=None)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--overwrite", action="store_true",
                    help="overwrite existing .wav outputs")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)

    args.ffmpeg = resolve_tool("ffmpeg", args.ffmpeg)
    if shutil.which(args.ffmpeg) is None and not os.path.exists(args.ffmpeg):
        ap.error(f"not found on PATH: {args.ffmpeg}")

    inputs = gather_inputs(args.inputs)
    if not inputs:
        ap.error("no .mp3 inputs found")

    if args.outdir:
        os.makedirs(args.outdir, exist_ok=True)

    results = []
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        futs = {pool.submit(convert_one, src, args): src for src in inputs}
        for fut in as_completed(futs):
            res = fut.result()
            results.append(res)
            tag = {"ok": "OK  ", "skip": "SKIP", "error": "ERR "}[res.status]
            print(f"[{tag}] {os.path.basename(res.src)}: {res.detail}")

    ok = sum(r.status == "ok" for r in results)
    skip = sum(r.status == "skip" for r in results)
    err = sum(r.status == "error" for r in results)
    print(f"\n{ok} converted, {skip} skipped, {err} errored (of {len(results)})")
    return 1 if err else 0


if __name__ == "__main__":
    sys.exit(main())
