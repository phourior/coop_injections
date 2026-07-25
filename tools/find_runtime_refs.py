from __future__ import annotations

import argparse
import struct
from pathlib import Path


def find_ascii_strings(data: bytes, start: int, end: int, minimum: int = 4):
    offset = start
    while offset < end:
        tail = offset
        while tail < end and 0x20 <= data[tail] < 0x7F:
            tail += 1
        if tail - offset >= minimum:
            yield offset, data[offset:tail].decode("ascii", errors="replace")
        offset = max(offset + 1, tail + 1)


def find_rip_refs(data: bytes, target: int):
    # Common x64 RIP-relative LEA/MOV forms used to load string addresses.
    for offset in range(0, len(data) - 7):
        rex = data[offset]
        opcode = data[offset + 1]
        modrm = data[offset + 2]
        if rex not in range(0x40, 0x50) or opcode not in (0x8B, 0x8D):
            continue
        if (modrm & 0xC7) != 0x05:
            continue
        displacement = struct.unpack_from("<i", data, offset + 3)[0]
        if offset + 7 + displacement == target:
            yield offset, data[offset : offset + 7]


def main() -> None:
    parser = argparse.ArgumentParser(description="Find feature strings and RIP-relative references in a runtime image.")
    parser.add_argument("image", type=Path)
    parser.add_argument("--start", type=lambda value: int(value, 0), default=0)
    parser.add_argument("--end", type=lambda value: int(value, 0))
    parser.add_argument("--match", action="append", default=[])
    args = parser.parse_args()

    data = args.image.read_bytes()
    end = min(args.end if args.end is not None else len(data), len(data))
    matches = tuple(value.lower() for value in args.match)

    for offset, text in find_ascii_strings(data, args.start, end):
        if matches and not any(value in text.lower() for value in matches):
            continue
        references = list(find_rip_refs(data, offset))
        refs = ", ".join(f"0x{ref:x}:{raw.hex(' ')}" for ref, raw in references) or "none"
        print(f"0x{offset:x}\t{text}\trefs={refs}")


if __name__ == "__main__":
    main()