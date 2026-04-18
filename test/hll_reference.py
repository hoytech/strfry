#!/usr/bin/env python3
"""
HyperLogLog reference implementation — exact port of go-nostr nip45/hyperloglog.

Used as oracle for C++ unit tests and E2E test vector generation.
"""

import struct


def clz56(x: int) -> int:
    """Count leading zeros in the lower 56 bits of x (direct port of go-nostr helpers.go)."""
    c = 0
    m = 1 << 55
    while (m & x) == 0 and m != 0:
        c += 1
        m >>= 1
    return c


def hll_add(registers: bytearray, pubkey_bytes: bytes, offset: int):
    """Update HLL registers from a 32-byte pubkey at the given offset (port of AddBytes)."""
    x = pubkey_bytes[offset:offset + 8]
    j = x[0]  # register index
    w = struct.unpack('>Q', x)[0]  # big-endian uint64
    zero_bits = clz56(w) + 1
    if zero_bits > registers[j]:
        registers[j] = zero_bits


def hll_encode(registers: bytearray) -> str:
    """Encode 256 registers as 512-char lowercase hex string."""
    return registers.hex()


def compute_offset_from_hex(tag_value_hex: str) -> int:
    """Compute HLL offset from a 64-char hex tag value (e.g. for #e/#p filters)."""
    return int(tag_value_hex[32], 16) + 8


def compute_offset_from_bytes(raw_bytes: bytes) -> int:
    """Compute HLL offset from 32 raw bytes (strfry internal for #e/#p tags)."""
    return ((raw_bytes[16] >> 4) & 0xF) + 8


if __name__ == "__main__":
    # Generate test vectors for C++ unit test

    # Known pubkeys (32 bytes each)
    pubkeys_hex = [
        "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798",
        "c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5",
        "f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9",
        "e493dbf1c10d80f3581e4904930b1404cc6c13900ee0758474fa94abe8c4cd13",
    ]
    pubkeys = [bytes.fromhex(h) for h in pubkeys_hex]

    offset = 12  # arbitrary test offset

    print(f"Test offset: {offset}")
    print(f"Number of pubkeys: {len(pubkeys)}")
    print()

    registers = bytearray(256)
    for i, pk in enumerate(pubkeys):
        hll_add(registers, pk, offset)
        encoded = hll_encode(registers)
        print(f"After adding pubkey[{i}] ({pubkeys_hex[i][:16]}...):")
        print(f"  hex = {encoded}")

    print()
    print(f"Final hex ({len(hll_encode(registers))} chars): {hll_encode(registers)}")

    # Also test offset computation
    test_tag = "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
    off = compute_offset_from_hex(test_tag)
    raw = bytes.fromhex(test_tag)
    off2 = compute_offset_from_bytes(raw)
    print(f"\nOffset from hex tag '{test_tag[32]}' (pos 32): {off}")
    print(f"Offset from raw byte[16]=0x{raw[16]:02x}: {off2}")
    assert off == off2, "Offset mismatch!"
    print("Offset computation consistent.")
