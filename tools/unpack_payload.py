from __future__ import annotations

import argparse
import struct
from pathlib import Path


IMAGE_BASE = 0x180000000
PACKER_ENTRY = 0x180C5F058
UNPACK_BASE_RVA = 0x650000
COMPRESSED_COUNT_RVA = 0xC5F248
OEP_RVA = 0x96A0E6


def u16(data: bytes | bytearray, off: int) -> int:
    return struct.unpack_from("<H", data, off)[0]


def u32(data: bytes | bytearray, off: int) -> int:
    return struct.unpack_from("<I", data, off)[0]


def w32(data: bytearray, off: int, value: int) -> None:
    struct.pack_into("<I", data, off, value)


def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def parse_pe(data: bytes) -> tuple[int, int, int, list[dict[str, int | str]]]:
    pe_off = u32(data, 0x3C)
    if data[pe_off : pe_off + 4] != b"PE\0\0":
        raise ValueError("not a PE file")

    number_of_sections = u16(data, pe_off + 6)
    opt_off = pe_off + 0x18
    magic = u16(data, opt_off)
    if magic != 0x20B:
        raise ValueError(f"expected PE32+, got optional header magic 0x{magic:x}")

    image_base = struct.unpack_from("<Q", data, opt_off + 0x18)[0]
    size_of_image = u32(data, opt_off + 0x38)
    file_alignment = u32(data, opt_off + 0x3C)
    address_of_entry_point_off = opt_off + 0x10
    section_table = opt_off + u16(data, pe_off + 0x14)

    sections: list[dict[str, int | str]] = []
    for i in range(number_of_sections):
        off = section_table + i * 0x28
        name = data[off : off + 8].rstrip(b"\0").decode("ascii", errors="replace")
        virtual_size = u32(data, off + 8)
        virtual_address = u32(data, off + 12)
        raw_size = u32(data, off + 16)
        raw_pointer = u32(data, off + 20)
        sections.append(
            {
                "name": name,
                "virtual_size": virtual_size,
                "virtual_address": virtual_address,
                "raw_size": raw_size,
                "raw_pointer": raw_pointer,
                "header_offset": off,
            }
        )

    return image_base, size_of_image, address_of_entry_point_off, file_alignment, sections


def build_memory_image(data: bytes, size_of_image: int, sections: list[dict[str, int | str]]) -> bytearray:
    image = bytearray(size_of_image)
    headers_size = min(len(data), min(int(s["raw_pointer"]) for s in sections if int(s["raw_pointer"]) > 0))
    image[:headers_size] = data[:headers_size]

    for section in sections:
        va = int(section["virtual_address"])
        raw_pointer = int(section["raw_pointer"])
        raw_size = int(section["raw_size"])
        virtual_size = int(section["virtual_size"])
        if raw_pointer == 0 or raw_size == 0:
            continue
        size = min(raw_size, virtual_size or raw_size, len(data) - raw_pointer, len(image) - va)
        if size > 0:
            image[va : va + size] = data[raw_pointer : raw_pointer + size]

    return image


class BitReader:
    def __init__(self, src: bytearray, pos: int):
        self.src = src
        self.pos = pos
        self.dl = 0x80

    def bit(self) -> int:
        carry = 1 if self.dl & 0x80 else 0
        self.dl = (self.dl << 1) & 0xFF
        if self.dl == 0:
            b = self.src[self.pos]
            self.pos += 1
            total = b + b + carry
            carry = 1 if total > 0xFF else 0
            self.dl = total & 0xFF
        return carry


def copy_back(buf: bytearray, out_pos: int, offset: int, length: int) -> int:
    if offset <= 0:
        raise ValueError(f"invalid back-reference offset {offset:#x}")
    src_pos = out_pos - offset
    if src_pos < 0:
        raise ValueError(f"back-reference before output: out={out_pos:#x} offset={offset:#x}")
    for _ in range(length):
        buf[out_pos] = buf[src_pos]
        out_pos += 1
        src_pos += 1
    return out_pos


def unpack_block(image: bytearray, src_pos: int, dst_pos: int) -> tuple[int, int]:
    reader = BitReader(image, src_pos)
    out = dst_pos
    last_offset = 0

    image[out] = image[reader.pos]
    reader.pos += 1
    out += 1
    ebx = 2

    while True:
        if reader.bit() == 0:
            image[out] = image[reader.pos]
            reader.pos += 1
            out += 1
            ebx = 2
            continue

        if reader.bit() == 0:
            eax = 1
            while True:
                eax = ((eax << 1) | reader.bit()) & 0xFFFFFFFF
                if reader.bit() == 0:
                    break

            eax = (eax - ebx) & 0xFFFFFFFF
            ebx = 1

            if eax != 0:
                eax = ((eax - 1) << 8) & 0xFFFFFFFF
                eax = (eax & 0xFFFFFF00) | image[reader.pos]
                reader.pos += 1
                last_offset = eax

                ecx = 1
                while True:
                    ecx = ((ecx << 1) | reader.bit()) & 0xFFFFFFFF
                    if reader.bit() == 0:
                        break

                if eax >= 0x7D00:
                    ecx += 2
                elif eax >= 0x500:
                    ecx += 1
                elif eax <= 0x7F:
                    ecx += 2

                out = copy_back(image, out, eax, ecx)
            else:
                if last_offset == 0:
                    raise ValueError("repeat back-reference before initial offset")

                ecx = 1
                while True:
                    ecx = ((ecx << 1) | reader.bit()) & 0xFFFFFFFF
                    if reader.bit() == 0:
                        break

                out = copy_back(image, out, last_offset, ecx)

            continue

        eax = 0
        if reader.bit() == 0:
            al = image[reader.pos]
            reader.pos += 1
            carry = al & 1
            eax = al >> 1
            if eax == 0:
                return reader.pos, out - dst_pos

            ecx = 2 + carry
            last_offset = eax
            out = copy_back(image, out, eax, ecx)
            ebx = 1
            continue

        for _ in range(4):
            eax = (eax << 1) | reader.bit()

        value = image[out - eax] if eax else 0
        image[out] = value
        out += 1
        ebx = 2


def write_rebuilt_pe(
    original: bytes,
    image: bytearray,
    address_of_entry_point_off: int,
    sections: list[dict[str, int | str]],
    output: Path,
) -> None:
    rebuilt = bytearray(original)
    w32(rebuilt, address_of_entry_point_off, OEP_RVA)

    for section in sections:
        va = int(section["virtual_address"])
        raw_pointer = int(section["raw_pointer"])
        raw_size = int(section["raw_size"])
        if raw_pointer == 0 or raw_size == 0:
            continue
        available = min(raw_size, len(rebuilt) - raw_pointer, len(image) - va)
        if available > 0:
            rebuilt[raw_pointer : raw_pointer + available] = image[va : va + available]

    output.write_bytes(rebuilt)


def write_ida_friendly_pe(
    original: bytes,
    image: bytearray,
    address_of_entry_point_off: int,
    file_alignment: int,
    sections: list[dict[str, int | str]],
    output: Path,
) -> None:
    rebuilt = bytearray(original)
    w32(rebuilt, address_of_entry_point_off, OEP_RVA)

    append_pos = align_up(len(rebuilt), file_alignment)
    if len(rebuilt) < append_pos:
        rebuilt.extend(b"\0" * (append_pos - len(rebuilt)))

    for section in sections:
        name = str(section["name"])
        va = int(section["virtual_address"])
        virtual_size = int(section["virtual_size"])
        raw_pointer = int(section["raw_pointer"])
        raw_size = int(section["raw_size"])
        header_offset = int(section["header_offset"])

        if name == ".themida":
            raw_pointer = append_pos
            raw_size = align_up(virtual_size, file_alignment)
            chunk = bytes(image[va : va + virtual_size])
            rebuilt.extend(chunk)
            rebuilt.extend(b"\0" * (raw_size - len(chunk)))
            append_pos += raw_size
            w32(rebuilt, header_offset + 16, raw_size)
            w32(rebuilt, header_offset + 20, raw_pointer)
            continue

        if raw_pointer == 0 or raw_size == 0:
            continue

        available = min(raw_size, len(rebuilt) - raw_pointer, len(image) - va)
        if available > 0:
            rebuilt[raw_pointer : raw_pointer + available] = image[va : va + available]

    output.write_bytes(rebuilt)


def main() -> None:
    parser = argparse.ArgumentParser(description="Statically unpack the payload.dll boot stub.")
    parser.add_argument("input", type=Path)
    parser.add_argument("--out-dir", type=Path, default=Path("unpacked"))
    args = parser.parse_args()

    data = args.input.read_bytes()
    image_base, size_of_image, ep_off, file_alignment, sections = parse_pe(data)
    if image_base != IMAGE_BASE:
        print(f"[!] image base is {image_base:#x}; script constants were derived for {IMAGE_BASE:#x}")

    image = build_memory_image(data, size_of_image, sections)

    count = image[COMPRESSED_COUNT_RVA]
    src = COMPRESSED_COUNT_RVA + 1
    dst = UNPACK_BASE_RVA
    print(f"[*] compressed block count: {count}")
    print(f"[*] source RVA: {src:#x}")
    print(f"[*] destination RVA: {dst:#x}")

    for index in range(count):
        src, written = unpack_block(image, src, dst)
        print(f"    block {index + 1:02d}: wrote {written:#x}, next src {src:#x}, next dst {dst + written:#x}")
        dst += written

    args.out_dir.mkdir(parents=True, exist_ok=True)
    full_image = args.out_dir / "payload.unpacked.full_image.bin"
    rebuilt_pe = args.out_dir / "payload.unpacked.dll"
    ida_pe = args.out_dir / "payload.unpacked.ida.dll"
    full_image.write_bytes(image)
    write_rebuilt_pe(data, image, ep_off, sections, rebuilt_pe)
    write_ida_friendly_pe(data, image, ep_off, file_alignment, sections, ida_pe)

    print(f"[*] OEP RVA: {OEP_RVA:#x} VA: {image_base + OEP_RVA:#x}")
    print(f"[*] full image: {full_image}")
    print(f"[*] rebuilt PE: {rebuilt_pe}")
    print(f"[*] IDA-friendly PE: {ida_pe}")


if __name__ == "__main__":
    main()
