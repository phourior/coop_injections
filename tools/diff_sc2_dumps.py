from __future__ import annotations

import argparse
import csv
import math
import os
import struct
from dataclasses import dataclass
from pathlib import Path


SC2_MODULE_HINTS = ("SC2_x64.exe",)
PAGE = 0x1000


@dataclass
class Region:
    file: str
    base: int
    allocation_base: int
    size: int
    bytes_read: int
    protect: str
    type: str


def parse_int(value: str) -> int:
    return int(value, 16) if value.lower().startswith("0x") else int(value)


def load_regions(root: Path) -> dict[int, Region]:
    out: dict[int, Region] = {}
    with (root / "regions.tsv").open("r", encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f, delimiter="\t"):
            r = Region(
                file=row["file"],
                base=parse_int(row["base"]),
                allocation_base=parse_int(row["allocation_base"]),
                size=parse_int(row["size"]),
                bytes_read=parse_int(row.get("bytes_read", row["size"])),
                protect=row["protect"],
                type=row["type"],
            )
            out[r.base] = r
    return out


def load_modules(root: Path) -> list[tuple[int, int, str, str]]:
    mods: list[tuple[int, int, str, str]] = []
    with (root / "modules.tsv").open("r", encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f, delimiter="\t"):
            try:
                mods.append((parse_int(row["base"]), parse_int(row["size"]), row["module"], row["path"]))
            except ValueError:
                pass
    return mods


def module_for(addr: int, modules: list[tuple[int, int, str, str]]) -> str:
    for base, size, name, _path in modules:
        if base <= addr < base + size:
            return name
    return ""


def entropy_sample(data: bytes) -> float:
    if not data:
        return 0.0
    counts = [0] * 256
    for b in data[: min(len(data), 0x20000)]:
        counts[b] += 1
    n = sum(counts)
    return -sum((c / n) * math.log2(c / n) for c in counts if c)


def score_u32(a: int, b: int) -> int:
    if a == b:
        return 0
    score = 0
    # Switch-ish values.
    if a in (0, 1) and b in (0, 1, 2, 3, 4, 5, 10):
        score += 20
    # Multipliers represented as Q12/fixed-point.
    for v in (0x1000, 0x2000, 0x3000, 0x4000, 1000, 10000):
        if a == v or b == v:
            score += 10
    # Avoid obvious pointers.
    if 0x10000 < a < 0x0000800000000000 or 0x10000 < b < 0x0000800000000000:
        score -= 5
    delta = abs(b - a)
    if delta in (1, 2, 3, 4, 5, 10, 100, 1000, 4096):
        score += 8
    if delta > 0x10000000:
        score -= 6
    return score


def score_float(a: float, b: float) -> int:
    if not (math.isfinite(a) and math.isfinite(b)):
        return 0
    if a == b:
        return 0
    if abs(a) > 100000 or abs(b) > 100000:
        return 0
    score = 0
    for v in (0.0, 1.0, 1.5, 2.0, 3.0, 5.0, 10.0):
        if abs(a - v) < 0.0001 or abs(b - v) < 0.0001:
            score += 10
    ratio = b / a if abs(a) > 0.0001 else 0.0
    if any(abs(ratio - v) < 0.001 for v in (1.5, 2.0, 3.0, 5.0, 10.0)):
        score += 15
    if abs(b - a) in (1.0, 2.0, 5.0, 10.0):
        score += 6
    return score


def main() -> None:
    parser = argparse.ArgumentParser(description="Diff two ProcDump SC2 memory dumps.")
    parser.add_argument("off", type=Path)
    parser.add_argument("on", type=Path)
    parser.add_argument("--out", type=Path, default=Path("sc2_diff"))
    parser.add_argument("--max-region-mb", type=int, default=256)
    args = parser.parse_args()

    off_regions = load_regions(args.off)
    on_regions = load_regions(args.on)
    modules = load_modules(args.off)
    args.out.mkdir(parents=True, exist_ok=True)

    common_bases = sorted(set(off_regions) & set(on_regions))
    region_rows: list[dict[str, object]] = []
    candidate_rows: list[dict[str, object]] = []

    for base in common_bases:
        ro = off_regions[base]
        rn = on_regions[base]
        if ro.size != rn.size or ro.size == 0:
            continue
        if ro.size > args.max_region_mb * 1024 * 1024:
            continue

        off_path = args.off / ro.file
        on_path = args.on / rn.file
        if not off_path.exists() or not on_path.exists():
            continue

        bo = off_path.read_bytes()
        bn = on_path.read_bytes()
        n = min(len(bo), len(bn), ro.size)
        if n == 0:
            continue

        diff_count = 0
        diff_pages = 0
        first_diff = None
        for page in range(0, n, PAGE):
            a = bo[page : page + PAGE]
            b = bn[page : page + PAGE]
            if a != b:
                diff_pages += 1
                if first_diff is None:
                    first_diff = page
                diff_count += sum(x != y for x, y in zip(a, b))

        if diff_count == 0:
            continue

        mod = module_for(base, modules)
        diff_ratio = diff_count / n
        ent = max(entropy_sample(bo), entropy_sample(bn))
        region_rows.append(
            {
                "base": f"0x{base:016x}",
                "size": f"0x{n:x}",
                "type": ro.type,
                "protect": ro.protect,
                "module": mod,
                "diff_pages": diff_pages,
                "diff_bytes": diff_count,
                "diff_ratio": f"{diff_ratio:.8f}",
                "first_diff": f"0x{base + (first_diff or 0):016x}",
                "entropy": f"{ent:.3f}",
                "file_off": ro.file,
                "file_on": rn.file,
            }
        )

        # Candidate scalar changes. Only inspect sparse-ish regions.
        if diff_ratio > 0.15:
            continue
        for off in range(0, n - 8, 4):
            uo = struct.unpack_from("<I", bo, off)[0]
            un = struct.unpack_from("<I", bn, off)[0]
            if uo != un:
                s = score_u32(uo, un)
                if s >= 12:
                    candidate_rows.append(
                        {
                            "score": s,
                            "kind": "u32",
                            "address": f"0x{base + off:016x}",
                            "off": uo,
                            "on": un,
                            "off_hex": f"0x{uo:08x}",
                            "on_hex": f"0x{un:08x}",
                            "module": mod,
                            "region_type": ro.type,
                            "protect": ro.protect,
                        }
                    )

            try:
                fo = struct.unpack_from("<f", bo, off)[0]
                fn = struct.unpack_from("<f", bn, off)[0]
            except struct.error:
                continue
            sf = score_float(fo, fn)
            if sf >= 15:
                candidate_rows.append(
                    {
                        "score": sf,
                        "kind": "float",
                        "address": f"0x{base + off:016x}",
                        "off": f"{fo:.6g}",
                        "on": f"{fn:.6g}",
                        "off_hex": f"0x{struct.unpack_from('<I', bo, off)[0]:08x}",
                        "on_hex": f"0x{struct.unpack_from('<I', bn, off)[0]:08x}",
                        "module": mod,
                        "region_type": ro.type,
                        "protect": ro.protect,
                    }
                )

    region_rows.sort(key=lambda r: (float(r["diff_ratio"]), int(r["diff_bytes"])))
    candidate_rows.sort(key=lambda r: (-int(r["score"]), r["address"]))

    with (args.out / "changed_regions.tsv").open("w", encoding="utf-8", newline="") as f:
        fields = ["base", "size", "type", "protect", "module", "diff_pages", "diff_bytes", "diff_ratio", "first_diff", "entropy", "file_off", "file_on"]
        writer = csv.DictWriter(f, fields, delimiter="\t")
        writer.writeheader()
        writer.writerows(region_rows)

    with (args.out / "scalar_candidates.tsv").open("w", encoding="utf-8", newline="") as f:
        fields = ["score", "kind", "address", "off", "on", "off_hex", "on_hex", "module", "region_type", "protect"]
        writer = csv.DictWriter(f, fields, delimiter="\t")
        writer.writeheader()
        writer.writerows(candidate_rows[:200000])

    print(f"common_regions={len(common_bases)}")
    print(f"changed_regions={len(region_rows)}")
    print(f"scalar_candidates={len(candidate_rows)}")
    print(f"out={args.out}")


if __name__ == "__main__":
    main()
