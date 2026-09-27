#!/usr/bin/env python3
"""Validate ROMTag relocation contracts in cross-linked Amiga device files."""

from __future__ import annotations

import re
import struct
import subprocess
import sys
from pathlib import Path

MATCH_WORD = 0x4AFC
RESIDENT_SIZE = 26


def command(*args: str) -> str:
    return subprocess.run(args, check=True, text=True, capture_output=True).stdout


def text_bytes(path: Path) -> dict[int, int]:
    """Read objdump's .text bytes, preserving their link-time offsets."""
    output = command("m68k-amigaos-objdump", "-s", "-j", ".text", str(path))
    result: dict[int, int] = {}
    for line in output.splitlines():
        match = re.match(r"\s*([0-9a-fA-F]+)\s+((?:[0-9a-fA-F]{8}\s*)+)", line)
        if match is None:
            continue
        offset = int(match.group(1), 16)
        for chunk in match.group(2).split():
            for byte in bytes.fromhex(chunk):
                result[offset] = byte
                offset += 1
    return result


def longword(data: dict[int, int], offset: int) -> int:
    try:
        return struct.unpack(">I", bytes(data[offset + index] for index in range(4)))[0]
    except KeyError as exc:
        raise ValueError(f"missing .text data at {offset:#x}") from exc


def word(data: dict[int, int], offset: int) -> int:
    try:
        return struct.unpack(">H", bytes(data[offset + index] for index in range(2)))[0]
    except KeyError as exc:
        raise ValueError(f"missing .text data at {offset:#x}") from exc


def resident_offset(path: Path) -> int:
    output = command("m68k-amigaos-nm", "-n", str(path))
    for line in output.splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[1].upper() == "T" and fields[2] == "_device_resident":
            return int(fields[0], 16)
    raise ValueError("_device_resident is not a .text symbol")


def text_relocations(path: Path) -> set[int]:
    output = command("m68k-amigaos-objdump", "-r", str(path))
    in_text = False
    offsets: set[int] = set()
    for line in output.splitlines():
        if line.startswith("RELOCATION RECORDS FOR ["):
            in_text = line == "RELOCATION RECORDS FOR [.text]:"
            continue
        if in_text:
            match = re.match(r"^([0-9a-fA-F]+)\s+RELOC32\s+\.text$", line)
            if match:
                offsets.add(int(match.group(1), 16))
    return offsets


def check(path: Path, expected_name: str) -> None:
    tag = resident_offset(path)
    data = text_bytes(path)
    relocations = text_relocations(path)
    end_skip = tag + RESIDENT_SIZE

    if word(data, tag) != MATCH_WORD:
        raise ValueError(f"{expected_name}: ROMTag at {tag:#x} lacks RTC_MATCHWORD")
    if longword(data, tag + 2) != tag or tag + 2 not in relocations:
        raise ValueError(f"{expected_name}: rt_MatchTag is not a relocated self-match")
    if longword(data, tag + 6) != end_skip or tag + 6 not in relocations:
        raise ValueError(
            f"{expected_name}: rt_EndSkip is not relocated immediately past its ROMTag"
        )
    print(f"resident artifact OK: {expected_name} (ROMTag {tag:#x})")


def main(argv: list[str]) -> int:
    if not argv:
        print("usage: check-resident-artifacts.py DEVICE:NAME [...]", file=sys.stderr)
        return 2
    try:
        for argument in argv:
            path_text, separator, expected_name = argument.rpartition(":")
            if not separator or not path_text or not expected_name:
                raise ValueError(f"invalid artifact argument: {argument!r}")
            check(Path(path_text), expected_name)
    except (OSError, subprocess.CalledProcessError, ValueError) as exc:
        print(f"resident artifact check failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
