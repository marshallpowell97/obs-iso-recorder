#!/usr/bin/env python3
"""Checks the output of tests/mac/headless-record-mac (see tests/mac/e2e.sh).

usage: check_e2e.py <out dir>   (expects <out dir>/log.txt and <out dir>/snap)
Exit code 1 if any check fails.
"""
import json
import os
import re
import subprocess
import sys

OUT = sys.argv[1]
failures = 0


def check(ok, msg):
    global failures
    print(("  ok    " if ok else "  FAIL  ") + msg)
    if not ok:
        failures += 1


def probe(path):
    r = subprocess.run(["ffprobe", "-v", "error", "-show_streams", "-show_format", "-of", "json", path],
                       capture_output=True, text=True)
    return json.loads(r.stdout or "{}")


def video(info):
    return next((s for s in info.get("streams", []) if s["codec_type"] == "video"), None)


def timecode(info):
    for s in info.get("streams", []):
        tc = s.get("tags", {}).get("timecode")
        if tc:
            return tc
    return None


def df_frames(tc):
    """59.94 drop-frame label -> real frame count."""
    h, m, s, f = map(int, re.split(r"[:;]", tc))
    minutes = h * 60 + m
    return ((h * 60 + m) * 60 + s) * 60 + f - 4 * (minutes - minutes // 10)


def psnr(a, b, start_a, start_b, n=30):
    fc = (f"[0:v]trim=start_frame={start_a}:end_frame={start_a + n},setpts=PTS-STARTPTS[a];"
          f"[1:v]trim=start_frame={start_b}:end_frame={start_b + n},setpts=PTS-STARTPTS[b];"
          "[a][b]psnr=stats_file=/dev/stdout")
    r = subprocess.run(["ffmpeg", "-v", "error", "-nostats", "-i", a, "-i", b, "-filter_complex", fc,
                        "-f", "null", "-"], capture_output=True, text=True)
    vals = [float(v) for v in re.findall(r"psnr_avg:(\S+)", r.stdout) if v != "inf"]
    if not vals and "psnr_avg:inf" in r.stdout:
        return float("inf")
    return sum(vals) / len(vals) if vals else 0.0


# Which files each phase recorded, from the plugin's own log lines.
phase = 0
files = {1: [], 2: [], 3: []}
log = open(os.path.join(OUT, "log.txt"), errors="replace").read()
for line in log.splitlines():
    m = re.match(r"PHASE (\d)$", line)
    if m:
        phase = int(m.group(1))
    m = re.search(r"\[iso-recorder: '(.+?)'\] Recording to '(.+?)'", line)
    if m and phase:
        files[phase].append((m.group(1), m.group(2)))

print("Phase 1: timecode, growing files, sync")
p1 = dict(files[1])
check(set(p1) == {"ISO A", "ISO B", "ISO C"}, f"three ISOs recorded ({', '.join(p1)})")
for name, path in sorted(p1.items()):
    final = probe(path)
    snap = probe(os.path.join(OUT, "snap", os.path.basename(path)))
    fv, sv = video(final), video(snap)
    check(fv is not None and int(fv.get("nb_frames", 0)) > 0, f"{name}: finalised file has video")
    check(timecode(final) is not None, f"{name}: finalised timecode {timecode(final)}")
    check(timecode(snap) == timecode(final), f"{name}: growing file timecode {timecode(snap)} matches")
    check(fv is not None and fv.get("has_b_frames", 0) == 0, f"{name}: no B-frames")
    check(fv is not None and fv["width"] % 4 == 0, f"{name}: width {fv and fv['width']} is a multiple of 4")
    for label, v in (("finalised", fv), ("growing", sv)):
        st = float(v.get("start_time", "nan")) if v else float("nan")
        check(abs(st) < 0.001, f"{name}: {label} video starts at {st:.4f} s (want 0)")

if {"ISO A", "ISO B"} <= set(p1):
    a, b = p1["ISO A"], p1["ISO B"]
    off = df_frames(timecode(probe(b))) - df_frames(timecode(probe(a)))
    scores = {o: psnr(a, b, o, 0) for o in range(off - 2, off + 3)}
    best = max(scores, key=scores.get)
    detail = ", ".join(f"{o}:{s:.1f}" for o, s in scores.items())
    check(best == off, f"ISO B frame 0 matches ISO A frame {off} predicted by timecode (dB {detail})")
    snaps = [os.path.join(OUT, "snap", os.path.basename(p)) for p in (a, b)]
    scores = {o: psnr(*snaps, o, 0) for o in range(off - 1, off + 2)}
    check(max(scores, key=scores.get) == off, "same match in the growing files")

print("Phase 2: stop then immediate restart")
check(len(files[2]) == 6, f"{len(files[2])} recordings started (want 3 + 3 after restart)")
for name, path in files[2]:
    v = video(probe(path))
    dur = float(v.get("duration", 0)) if v else 0
    check(2.0 < dur < 4.0, f"{name}: {os.path.basename(path)} finalised, {dur:.2f} s")
m = re.search(r"PHASE 2 restart, active=(\d+)", log)
check(m and m.group(1) == "3", f"active count right after restart = {m.group(1) if m else '?'} (want 3)")

print("Phase 3: filter removed while recording")
check(len(files[3]) == 1, "one recording started")
for name, path in files[3]:
    v = video(probe(path))
    dur = float(v.get("duration", 0)) if v else 0
    check(v is not None and 1.0 < dur < 4.0 and "nb_frames" in v, f"{name}: finalised, {dur:.2f} s")
m = re.search(r"PHASE 3 done, active=(\d+)", log)
check(m and m.group(1) == "0", "nothing left recording")

print("Shutdown")
m = re.search(r"DONE leaks=(-?\d+)", log)
check(m is not None, "clean shutdown" + (f", {m.group(1)} leaked allocations" if m else ""))

print(f"\n{failures} failure(s)" if failures else "\nall end-to-end checks passed")
sys.exit(1 if failures else 0)
