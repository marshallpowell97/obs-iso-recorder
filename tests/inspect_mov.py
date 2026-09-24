#!/usr/bin/env python3
"""Inspect an ISO Recorder MOV (growing or finished).

Prints the top-level layout, whether the timecode track is present in the
moov, the stored start frame, and fragment statistics. Exit code 1 if the
timecode track is missing.
"""
import struct
import sys

CONTAINERS = {b"moov", b"trak", b"mdia", b"minf", b"stbl", b"moof", b"traf", b"mvex",
              b"edts", b"udta", b"dinf", b"tref", b"gmhd"}


def boxes(data, start, end):
    pos = start
    while pos + 8 <= end:
        size, typ = struct.unpack(">I4s", data[pos:pos + 8])
        hdr = 8
        if size == 1:
            size = struct.unpack(">Q", data[pos + 8:pos + 16])[0]
            hdr = 16
        elif size == 0:
            size = end - pos
        if size < 8 or pos + size > end:
            yield typ, pos, hdr, None  # incomplete (still being written)
            return
        yield typ, pos, hdr, size
        pos += size


def children(data, pos, hdr, size):
    return list(boxes(data, pos + hdr, pos + size))


def find(data, pos, hdr, size, path):
    """Find first box along a path of fourccs."""
    for typ, p, h, s in children(data, pos, hdr, size):
        if s is None:
            continue
        if typ == path[0]:
            if len(path) == 1:
                return p, h, s
            return find(data, p, h, s, path[1:])
    return None


def fps_label(num, den, df, n):
    nominal = (num + den // 2) // den
    if df:
        drop = nominal // 15
        fp10 = nominal * 600 - drop * 9
        fpm = nominal * 60 - drop
        d, m = divmod(n, fp10)
        n += drop * 9 * d + (drop * ((m - drop) // fpm) if m > drop else 0)
    ff = n % nominal
    ss = n // nominal % 60
    mm = n // (nominal * 60) % 60
    hh = n // (nominal * 3600) % 24
    return "%02d:%02d:%02d%s%02d" % (hh, mm, ss, ";" if df else ":", ff)


def main(path):
    data = open(path, "rb").read()
    top = list(boxes(data, 0, len(data)))
    layout = [t.decode("latin1") for t, _, _, s in top[:6]]
    n_moof = sum(1 for t, *_ in top if t == b"moof")
    incomplete = top and top[-1][3] is None
    print(f"file: {path}")
    print(f"  size {len(data)/1e6:.1f} MB, first boxes {layout}, moof count {n_moof}"
          + (" (last box still being written)" if incomplete else ""))

    moov = next(((p, h, s) for t, p, h, s in top if t == b"moov" and s), None)
    if not moov:
        print("  no moov yet")
        return 1

    tracks = []
    for typ, p, h, s in children(data, *moov):
        if typ != b"trak" or s is None:
            continue
        hdlr = find(data, p, h, s, [b"mdia", b"hdlr"])
        handler = data[hdlr[0] + 16:hdlr[0] + 20].decode("latin1") if hdlr else "?"
        tkhd = find(data, p, h, s, [b"tkhd"])
        version = data[tkhd[0] + 8]
        track_id = struct.unpack(">I", data[tkhd[0] + (28 if version else 20):tkhd[0] + (32 if version else 24)])[0]
        tref = find(data, p, h, s, [b"tref"])
        refs = [c[0].decode() for c in children(data, *tref)] if tref else []
        tracks.append((track_id, handler, refs, (p, h, s)))

    print("  tracks:", ", ".join(f"#{i} {hd}{' ->' + '/'.join(r) if r else ''}" for i, hd, r, _ in tracks))
    mvex = find(data, *moov, [b"mvex"])
    if mvex:
        print(f"  mvex trex entries: {sum(1 for c in children(data, *mvex) if c[0] == b'trex')}")

    tc = next((t for t in tracks if t[1] == "tmcd"), None)
    if not tc:
        print("  NO TIMECODE TRACK")
        return 1

    stsd = find(data, *tc[3], [b"mdia", b"minf", b"stbl", b"stsd"])
    entry = stsd[0] + 16
    flags, timescale, frame_dur, nb = struct.unpack(">IIIB", data[entry + 20:entry + 33])
    df = bool(flags & 1)
    print(f"  tmcd: timescale {timescale}, frame duration {frame_dur}, {nb} fps nominal, "
          f"{'drop-frame' if df else 'non-drop'}")

    # Timecode sample: final file -> stco of tmcd track; growing -> first moof.
    stco = find(data, *tc[3], [b"mdia", b"minf", b"stbl", b"stco"])
    count = struct.unpack(">I", data[stco[0] + 12:stco[0] + 16])[0] if stco else 0
    if count:
        off = struct.unpack(">I", data[stco[0] + 16:stco[0] + 20])[0]
        where = "final moov"
    else:
        moof = next(((p, h, s) for t, p, h, s in top if t == b"moof" and s), None)
        off = None
        for typ, p, h, s in children(data, *moof):
            if typ != b"traf":
                continue
            tfhd = find(data, p, h, s, [b"tfhd"])
            tid = struct.unpack(">I", data[tfhd[0] + 12:tfhd[0] + 16])[0]
            if tid == tc[0]:
                base = struct.unpack(">Q", data[tfhd[0] + 16:tfhd[0] + 24])[0]
                trun = find(data, p, h, s, [b"trun"])
                data_off = struct.unpack(">i", data[trun[0] + 16:trun[0] + 20])[0]
                off = base + data_off
        where = "first fragment"
    if off is None:
        print("  timecode sample not found")
        return 1
    start = struct.unpack(">I", data[off:off + 4])[0]
    print(f"  start timecode {fps_label(timescale, frame_dur, df, start)} (frame {start}, from {where})")
    return 0


if __name__ == "__main__":
    rc = 0
    for p in sys.argv[1:]:
        rc |= main(p)
    sys.exit(rc)
