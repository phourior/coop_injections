from __future__ import annotations

import argparse
import struct
from pathlib import Path


OEP_RVA = 0x96A0E6


def u16(data: bytes | bytearray, off: int) -> int:
    return struct.unpack_from("<H", data, off)[0]


def u32(data: bytes | bytearray, off: int) -> int:
    return struct.unpack_from("<I", data, off)[0]


def w32(data: bytearray, off: int, value: int) -> None:
    struct.pack_into("<I", data, off, value)


def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def main() -> None:
    parser = argparse.ArgumentParser(description="Build an IDA-friendly PE from a runtime full image dump.")
    parser.add_argument("original_pe", type=Path)
    parser.add_argument("full_image", type=Path)
    parser.add_argument("output_pe", type=Path)
    args = parser.parse_args()

    original = bytearray(args.original_pe.read_bytes())
    image = args.full_image.read_bytes()

    pe = u32(original, 0x3C)
    opt = pe + 0x18
    if original[pe : pe + 4] != b"PE\0\0":
        raise ValueError("not a PE")
    if u16(original, opt) != 0x20B:
        raise ValueError("expected PE32+")

    section_count = u16(original, pe + 6)
    opt_size = u16(original, pe + 0x14)
    file_alignment = u32(original, opt + 0x3C)
    section_table = opt + opt_size

    w32(original, opt + 0x10, OEP_RVA)

    rebuilt = bytearray(original)
    append_pos = align_up(len(rebuilt), file_alignment)
    if len(rebuilt) < append_pos:
        rebuilt.extend(b"\0" * (append_pos - len(rebuilt)))

    for i in range(section_count):
        sh = section_table + i * 0x28
        name = original[sh : sh + 8].rstrip(b"\0").decode("ascii", errors="replace")
        virtual_size = u32(original, sh + 8)
        virtual_address = u32(original, sh + 12)
        raw_size = u32(original, sh + 16)
        raw_pointer = u32(original, sh + 20)

        if name == ".themida" or raw_size == 0:
            raw_pointer = append_pos
            raw_size = align_up(virtual_size, file_alignment)
            chunk = image[virtual_address : virtual_address + virtual_size]
            rebuilt.extend(chunk)
            rebuilt.extend(b"\0" * (raw_size - len(chunk)))
            append_pos += raw_size
            w32(rebuilt, sh + 16, raw_size)
            w32(rebuilt, sh + 20, raw_pointer)
            continue

        size = min(raw_size, len(image) - virtual_address, len(rebuilt) - raw_pointer)
        if raw_pointer and size > 0:
            rebuilt[raw_pointer : raw_pointer + size] = image[virtual_address : virtual_address + size]

    args.output_pe.write_bytes(rebuilt)
    print(f"wrote {args.output_pe}")


if __name__ == "__main__":
    main()
