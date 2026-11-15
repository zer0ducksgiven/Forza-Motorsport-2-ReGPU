# layout_order.py <generated code folder> <seed> <order file> [<profile> [count|address]]: writes a linker
# /ORDER file that places every generated function in a random order fixed by <seed>. Each seed gives the
# same code a different layout, so an A/B can test several layouts per build instead of one lucky or unlucky one.
# With a profile ("<samples>\t<function>" lines, from FM2_SAMPLE_OUT) the functions it names come first, the
# most sampled first ("count") or in guest address order ("address"), and only the rest are shuffled.
import random, re, sys
from pathlib import Path

folder, seed, out = Path(sys.argv[1]), int(sys.argv[2]), Path(sys.argv[3])
profile = Path(sys.argv[4]) if len(sys.argv) > 4 else None
mode = sys.argv[5] if len(sys.argv) > 5 else "count"
names = set()
for f in sorted(folder.glob("*.cpp")):
    names.update(re.findall(r"\b(?:__imp__)?sub_[0-9A-F]{8}\b", f.read_text(encoding="utf-8", errors="replace")))
names = sorted(names)
random.Random(seed).shuffle(names)
# Only the bodies (__imp__sub_, extern "C") are kept: the sub_ entries are C++ names the linker can't match.
# They are dropped after the shuffle so each seed keeps the layout it had before.
names = [n for n in names if n.startswith("__imp__")]
hot = []
if profile:
    samples = {}
    for line in profile.read_text(encoding="utf-8", errors="replace").splitlines():
        count, _, name = line.partition("\t")
        m = re.fullmatch(r"(?:__imp__)?sub_([0-9A-F]{8})", name.strip())
        if m and count.isdigit():
            samples[m.group(1)] = samples.get(m.group(1), 0) + int(count)
    key = (lambda a: (-samples[a], a)) if mode == "count" else (lambda a: a)
    present = set(names)
    hot = [f"__imp__sub_{a}" for a in sorted(samples, key=key) if f"__imp__sub_{a}" in present]
    taken = set(hot)
    names = hot + [n for n in names if n not in taken]
text = "\n".join(names) + "\n"
if not out.exists() or out.read_text() != text:
    out.write_text(text)
print(f"layout_order: {len(names)} functions, seed {seed}" + (f", {len(hot)} hot first by {mode}" if profile else ""))
