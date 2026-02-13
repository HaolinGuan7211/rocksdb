#!/usr/bin/env python3
from __future__ import annotations

import argparse
import struct
import sys


def make_key(index: int, key_size: int, key_format: str) -> str:
    if key_format == "legacy_k":
        key_digits = key_size - 1
        return f"k{index:0{key_digits}d}"

    if key_format == "db_bench_u64be_ascii0":
        if key_size < 8:
            raise ValueError("db_bench_u64be_ascii0 requires key_size >= 8")
        prefix = struct.pack(">Q", index)
        suffix_len = key_size - 8
        key = prefix + (b"0" * suffix_len)
        return "0x" + key.hex().upper()

    raise ValueError(f"unknown key format: {key_format}")


def make_value(value_size: int, value_hex: bool) -> str:
    if value_hex:
        return "0x" + (b"v" * value_size).hex().upper()
    return "v" * value_size


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate sorted 'key ==> value' lines for ldb write_extern_sst.")
    parser.add_argument("--start", type=int, required=True, help="Inclusive start key index.")
    parser.add_argument("--count", type=int, required=True, help="Number of keys to generate.")
    parser.add_argument("--key-size", type=int, default=16, help="Fixed key size.")
    parser.add_argument("--value-size", type=int, default=256, help="Fixed value size.")
    parser.add_argument(
        "--key-format",
        default="legacy_k",
        choices=["legacy_k", "db_bench_u64be_ascii0"],
        help="Key encoding format.",
    )
    parser.add_argument(
        "--value-hex",
        action="store_true",
        help="Emit value as hex form (0x...). Useful with ldb --hex.",
    )
    args = parser.parse_args()

    if args.start < 0 or args.count < 0:
        print("start/count must be non-negative", file=sys.stderr)
        return 2
    if args.key_size < 2:
        print("key-size must be >= 2", file=sys.stderr)
        return 2
    if args.value_size < 1:
        print("value-size must be >= 1", file=sys.stderr)
        return 2
    if args.key_format == "db_bench_u64be_ascii0" and args.key_size < 8:
        print("db_bench_u64be_ascii0 requires key-size >= 8", file=sys.stderr)
        return 2

    value = make_value(args.value_size, args.value_hex)

    out = sys.stdout
    end = args.start + args.count
    try:
        for i in range(args.start, end):
            key = make_key(i, args.key_size, args.key_format)
            out.write(key)
            out.write(" ==> ")
            out.write(value)
            out.write("\n")
        out.flush()
    except BrokenPipeError:
        return 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
