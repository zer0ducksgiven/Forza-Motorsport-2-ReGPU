"""mkloops.py <xmadecode.exe> <game>/Media/Audio/Cars/Engines src/audio/seamless_loops.h

The seamless loops the game's XMA decoder gives engine sounds (src/audio/xma.cpp, PatchLoop).
Every bank is decoded as the game does (tools/xmadecode.cpp, bit-identical to its output). FMOD
plays a stream x of L samples as the loop x[S:L], S = trim_start - 128 (it skips the restart's
pre-roll). Per sound, blends of several lengths are tried at the points that best match the join
(level included): the loop's end blended into what precedes a point like its start (landing before
any silence the stream ends with), or its start blended in from what follows a point like its end.
Each is played as FMOD loops it and the join scored (spectral change and 20 ms loudness step, over
the loop's typical ones); the best goes in the table when it clearly beats the sound's own join.
A <header>.info next to it lists each sound's bank, name and scores.
"""
import glob, os, subprocess, sys, tempfile
import numpy as np

exe, banks, out = sys.argv[1:4]
MATCH_LENGTHS = (2048, 8192)  # samples compared around the join, tried per sound
M = MATCH_LENGTHS[0]


def ncorr(a, b):
    a = a - a.mean(); b = b - b.mean()
    return float(np.dot(a, b) / (np.sqrt(np.dot(a, a) * np.dot(b, b)) + 1e-9))


def best_point(x, S, n, L, pad):
    """k in [S+n+pad, L-pad-n-M) maximising the match of x[k:k+M] with x[S:S+M]: where the tail blends to."""
    return best_match(x, x[S:S + M], S + n + pad, L - pad - n - M)


def best_head(x, S, n, L):
    """k2 in [S+n+M, L-n-M] whose preceding M samples best match the loop's last M: where the start blends from."""
    i, score = best_match(x, x[L - M:L], S + n, L - n - 2 * M)
    return i + M, score


def best_match(x, ref, lo, hi):
    """i in [lo, hi) maximising the likeness 2ab / (a^2 + b^2) of x[i:i+M] and ref (FFT cross-correlation).
    Levels count, the slow offset too: some idle sounds sit far off zero where their loop restarts."""
    seg = x[lo:hi + M]
    size = 1 << int(np.ceil(np.log2(len(seg) + M)))
    num = np.fft.irfft(np.fft.rfft(seg, size) * np.conj(np.fft.rfft(ref, size)), size)[:hi - lo]
    c2 = np.cumsum(np.concatenate(([0.0], seg * seg)))
    s2 = c2[M:M + hi - lo] - c2[:hi - lo]
    score = 2 * num / (s2 + np.dot(ref, ref) + 1e-9)
    i = int(np.argmax(score))
    return lo + i, float(score[i])


BLENDS_MS = (60, 120, 200, 400, 800)  # tried per sound: the one whose join changes least is kept


def fixed(x, L, k, n, r, pad):
    """x with the game's crossfade (xma.cpp PatchLoop): raised cosine, level kept for correlation r,
    ending pad samples early (before the stream's trailing silence), which is filled with x[k-pad:k]."""
    y = x.copy()
    w = 0.5 - 0.5 * np.cos(np.pi * (np.arange(n) + 1) / (n + 1))
    g = 1 / np.sqrt((1 - w) ** 2 + w ** 2 + 2 * max(r, 0) * w * (1 - w))
    e = L - pad
    y[e - n:e] = (x[e - n:e] * (1 - w) + x[k - pad - n:k - pad] * w) * g
    y[e:L] = x[k - pad:k]
    return y


def fixed_head(x, S, k2, n, r):
    """x with the game's loop-start blend: x[S:S+n] faded in from x[k2:k2+n] (what follows the loop's end)."""
    y = x.copy()
    v = 0.5 - 0.5 * np.cos(np.pi * (np.arange(n) + 1) / (n + 1))
    g = 1 / np.sqrt((1 - v) ** 2 + v ** 2 + 2 * max(r, 0) * v * (1 - v))
    y[S:S + n] = (x[k2:k2 + n] * (1 - v) + x[S:S + n] * v) * g
    return y


FN, FH = 1024, 256
FWIN = np.hanning(FN)


def spec(seg):
    return np.array([np.log(np.abs(np.fft.rfft(seg[s:s + FN] * FWIN))[:256] + 1) for s in range(0, len(seg) - FN, FH)])


LW, LH = 960, 240  # loudness: 20 ms RMS every 5 ms


def level_steps(seg):
    db = 20 * np.log10(np.array([np.sqrt(np.mean(seg[i:i + LW] ** 2)) + 1 for i in range(0, len(seg) - LW, LH)]))
    return np.abs(np.diff(db))


def typical_change(x, S, L):
    """The loop's median frame-to-frame spectral change (from its middle) and its 95th-percentile
    20 ms loudness step (over the whole loop)."""
    m = (S + L) // 2
    body = spec(x[m - 12 * FH:m + 12 * FH + FN])
    return float(np.median(np.sqrt((np.diff(body, axis=0) ** 2).mean(1)))), float(np.percentile(level_steps(x[S:L]), 95)) + 0.1


def join_change(x, S, L, typ):
    """How much the join of x[S:L] played twice stands out from the loop: the larger of its spectral
    change and its loudness step, each over the loop's typical one (1 = like anywhere else)."""
    med, lvl = typ
    around = np.concatenate([x[L - 8 * FH - FN // 2:L], x[S:S + 8 * FH + FN // 2]])
    fl = np.sqrt((np.diff(spec(around), axis=0) ** 2).mean(1))
    span = np.concatenate([x[L - 6 * LW:L], x[S:S + 6 * LW]])
    return float(max(fl.max() / (med + 1e-9), level_steps(span).max() / lvl))


rows, seen = [], {}
tmp = tempfile.mkdtemp()
files = sorted(glob.glob(os.path.join(banks, "**", "*.fsb"), recursive=True))
for fi, f in enumerate(files):
    res = subprocess.run([exe, f, tmp], capture_output=True, text=True)
    for line in res.stdout.splitlines():
        name, h, rate, ch, frames, ts, te, L, ls, le = line.split()
        rate, ch, ts, L = int(rate), int(ch), int(ts), int(L)
        raw = os.path.join(tmp, h + ".raw")
        if h in seen or not os.path.exists(raw):  # the same sound again (in this bank or another)
            continue
        seen[h] = L
        x = np.fromfile(raw, np.int16).astype(float).reshape(-1, ch).mean(axis=1)
        os.remove(raw)
        S = max(ts - 128, 0)
        M = MATCH_LENGTHS[0]
        if L - S < 8 * M or np.sqrt(np.mean(x ** 2)) < 30:
            continue
        # Trailing silence (encoder padding the stream plays at every pass): the end blend lands before it.
        loud = np.nonzero(np.abs(x) > 4)[0]
        pad = L - 1 - int(loud[-1]) if len(loud) and L - 1 - int(loud[-1]) > 16 else 0
        med = typical_change(x, S, L - pad)
        own = join_change(x, S, L, med)
        best = None
        # Either the loop's end is blended into what precedes a point like its start, or (from the
        # second pass on, for sounds recorded with a burst at their start) its start is blended in
        # from what follows a point like its end. One or the other, never both.
        for M, bl in [(m, b) for m in MATCH_LENGTHS for b in BLENDS_MS]:
            n = min(int(rate * bl / 1000), (L - S) // 4)
            if L - pad - n - 2 * M <= S + n + pad + 1:
                continue
            k, match = best_point(x, S, n, L, pad)
            # The blend's gain is set by how alike the two pieces it mixes are (not the match after them).
            if match >= 0.5:
                r = max(ncorr(x[L - pad - n:L - pad], x[k - pad - n:k - pad]), 0.0)
                c = join_change(fixed(x, L, k, n, r, pad), S, L, med)
                if best is None or c < best[0]:
                    best = (c, (k, n, r, 0, 0, 0.0))
            k2, match2 = best_head(x, S, n, L)
            if match2 >= 0.5 and not pad:
                r2 = max(ncorr(x[S:S + n], x[k2:k2 + n]), 0.0)
                c = join_change(fixed_head(x, S, k2, n, r2), S, L, med)
                if best is None or c < best[0]:
                    best = (c, (0, 0, 0.0, k2, n, r2))
        # In the table when the join changes clearly less than it did.
        if best and best[0] < own * 0.9 and own > 1.05:
            rows.append((h, L) + best[1] + ((pad if best[1][1] else 0), own, best[0], os.path.basename(f), name))
    print("%d/%d %s: %d loops so far" % (fi + 1, len(files), os.path.basename(f), len(rows)), flush=True)
# Every engine sound is listed, with no blend when it needs none: the game decodes listed streams as
# the hardware does (the rest, menu sounds among them, as before).
fixed_count = len(rows)
done = {r[0] for r in rows}
rows += [(h, L, 0, 0, 0.0, 0, 0, 0.0, 0, 0.0, 0.0, "", "") for h, L in seen.items() if h not in done]
rows.sort(key=lambda r: int(r[0], 16))
with open(out, "w", newline="\n") as t:
    t.write("// Generated by tools/mkloops.py from the engine sound banks: %d sounds, %d with a seamless loop.\n" % (len(rows), fixed_count))
    t.write("#pragma once\n#include <cstdint>\n\n")
    t.write("// A stream's first packet hash, length, end blend (from, n, r), start blend (from2, n2, r2) and the\n")
    t.write("// silence it ends with (pad); see Decoder::PatchLoop in xma.cpp. Sorted by hash.\n")
    t.write("struct SeamlessLoop\n{\n    uint64_t hash;\n    uint32_t length, from, n;\n    float r;\n    uint32_t from2, n2;\n    float r2;\n    uint32_t pad;\n};\n\n")
    t.write("inline const SeamlessLoop kSeamlessLoops[] = {\n")
    t.write("".join("    { 0x%sull, %d, %d, %d, %.3ff, %d, %d, %.3ff, %d },\n" % ((r[0].upper(),) + r[1:9]) for r in rows))
    t.write("};\n")
with open(out + ".info", "w") as t:
    t.write("".join("%s %d %d %d %.3f %d %d %.3f %d join change x%.2f -> x%.2f %s %s\n" % r for r in rows if r[3] or r[6]))
print("%d sounds, %d seamless loops in %s" % (len(rows), fixed_count, out))
