#!/usr/bin/env python3
"""End-to-end check of /dev/mt7921_sdr: throughput at each rate, live retune, manual and auto
gain, and 802.11b beacon detection (11-chip Barker, 1 us symbols) as proof of real reception.

usage: sdr_check.py [--save DIR]      (needs numpy; SDR mode up, nothing else holding the device)
"""
import argparse
import fcntl
import os
import struct
import time

import numpy as np

START, STOP, INFO = 0x40085301, 0x5302, 0x80385303
GAIN, TUNE = 0x40105304, 0x40085305
DEV = '/dev/mt7921_sdr'


def info(fd):
    p, by, dr, sg, dg, bl, st, rate, freq = struct.unpack('<QQQ7I4x', fcntl.ioctl(fd, INFO, bytes(56)))[:9]
    return dict(packets=p, drops=dr, seq_gaps=sg, dev_gaps=dg, bad_len=bl, streaming=st)


def iq(raw):
    w = np.frombuffer(raw, '<u4')
    i = (((w >> 16) & 0x3fff) ^ 0x2000).astype(np.int32) - 0x2000
    q = ((w & 0x3fff) ^ 0x2000).astype(np.int32) - 0x2000
    return (i + 1j * q).astype(np.complex64)


def stream(fd, secs, keep=8 << 20):
    total, kept, t0 = 0, bytearray(), time.monotonic()
    while time.monotonic() - t0 < secs:
        b = os.read(fd, 4 << 20)
        total += len(b)
        if len(kept) < keep:
            kept += b[:keep - len(kept)]
    return total / (time.monotonic() - t0), bytes(kept)


def stop(fd):
    fcntl.ioctl(fd, STOP)
    while os.read(fd, 4 << 20):
        pass


def barker_channels(x, fs, lo_mhz):
    """Count 40 us windows holding DSSS, by channel; a working receiver finds beacons on real channels."""
    barker = np.array([1, -1, 1, 1, -1, 1, 1, 1, -1, -1, -1], float)
    T = int(round(fs / 1e6))
    tmpl = barker[np.minimum((np.arange(T) * 11e6 / fs).astype(int), 10)]
    x = x - x.mean()
    win = 40 * T
    p = np.add.reduceat(np.abs(x) ** 2, np.arange(0, len(x) - win, win))
    med = np.median(p)
    t = np.arange(win) / fs
    found = {}
    for wi in np.nonzero(p > 8 * med)[0][:400]:
        seg = x[wi * win:(wi + 1) * win]
        best = (0, None)
        for off in np.arange(-15, 15.1, 2.5):
            c = np.abs(np.convolve(seg * np.exp(-2j * np.pi * off * 1e6 * t), tmpl[::-1], 'valid'))
            m = len(c) // T * T
            fold = c[:m].reshape(-1, T).mean(0)
            best = max(best, (fold.max() / (np.median(fold) + 1e-9), off), key=lambda b: b[0])
        if best[0] > 3:
            ch = int(round((lo_mhz + best[1] - 2407) / 5))
            found[ch] = found.get(ch, 0) + 1
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--save', help='directory for the raw captures')
    a = ap.parse_args()
    fd = os.open(DEV, os.O_RDONLY)
    ok = True
    try:
        for rate in (40, 80, 160):
            fcntl.ioctl(fd, START, struct.pack('<II', rate, 2437))
            bps, raw = stream(fd, 2)
            stop(fd)
            st = info(fd)
            want = rate * 4e6
            good = st['seq_gaps'] == 0 and st['bad_len'] == 0 and (rate > 80 or bps > 0.97 * want)
            ok &= good
            print(f'{rate:3d} MSps: {bps / 1e6:6.1f} MB/s of {want / 1e6:.0f}, {st}  {"ok" if good else "FAIL"}')
            if a.save:
                open(os.path.join(a.save, f'mt7922_{rate}msps_2437.bin'), 'wb').write(raw)
            if rate == 40:
                found = barker_channels(iq(raw[:16 << 20])[:2_000_000], 40e6, 2437)
                print(f'         802.11b windows by channel: {dict(sorted(found.items())) or "none"}')

        # Live retune: the stream must keep running across TUNE.
        fcntl.ioctl(fd, START, struct.pack('<II', 40, 2412))
        for mhz in (2412, 2437, 2462, 2450):
            fcntl.ioctl(fd, TUNE, struct.pack('<Q', mhz * 1000000))
            bps, _ = stream(fd, 0.5)
        st = info(fd)
        good = st['streaming'] == 1 and st['seq_gaps'] == 0
        ok &= good
        print(f'retune 2412 -> 2437 -> 2462 -> 2450 while streaming: {st}  {"ok" if good else "FAIL"}')

        # Manual gain (stops and restarts the stream around it), then back to AGC.
        floors = []
        for g in ((1, 4, 9, 24), (1, 4, 9, 12), (0, 0, 0, 0)):
            fcntl.ioctl(fd, GAIN, struct.pack('<IIIi', *g))
            _, raw = stream(fd, 0.4, keep=4 << 20)
            x = iq(raw[1 << 20:])
            p = np.sort((np.abs(x[:len(x) // 1024 * 1024]) ** 2).reshape(-1, 1024).mean(1))
            floors.append(np.sqrt(p[:max(1, len(p) // 10)].mean()))
        stop(fd)
        db = 20 * np.log10(floors[0] / floors[1])
        good = 9 < db < 15 and floors[1] > 24     # 6+ LSB: step not set by quantization
        ok &= good
        print(f'gain: noise floor {floors[0]:.0f} at DIG +24, {floors[1]:.0f} at DIG +12 ({db:.1f} dB, want ~12), '
              f'{floors[2]:.0f} on AGC  {"ok" if good else "FAIL"}')
    finally:
        os.close(fd)
    print('PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    raise SystemExit(main())
