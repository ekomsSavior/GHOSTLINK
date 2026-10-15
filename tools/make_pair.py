#!/usr/bin/env python3
import argparse
import secrets


def parse_mac(s: str):
    parts = s.split(":")
    if len(parts) != 6 or any(len(part) != 2 for part in parts):
        raise argparse.ArgumentTypeError("MAC must look like AA:BB:CC:DD:EE:FF")
    try:
        b = bytes(int(x, 16) for x in parts)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("invalid hex in MAC") from exc
    if len(b) != 6:
        raise argparse.ArgumentTypeError("MAC must be 6 bytes")
    if b == bytes(6) or b[0] & 1:
        raise argparse.ArgumentTypeError("MAC must be a nonzero unicast station address")
    return b


def c_array(data: bytes):
    return ", ".join(f"0x{x:02X}" for x in data)


def main():
    p = argparse.ArgumentParser(description="Generate a pair-unique GHOSTLINK config")
    p.add_argument("--a", required=True, type=parse_mac, help="node A station MAC")
    p.add_argument("--b", required=True, type=parse_mac, help="node B station MAC")
    args = p.parse_args()
    if args.a == args.b:
        p.error("A and B must have different station MAC addresses")

    network_id = int.from_bytes(secrets.token_bytes(2), "big") or 1
    pmk = secrets.token_bytes(16)
    lmk = secrets.token_bytes(16)
    hop = secrets.token_bytes(32)

    print("#pragma once")
    print("#include <Arduino.h>")
    print("#define GHOST_PAIR_CONFIGURED 1")
    print("namespace ghostpair {")
    print(f"constexpr uint16_t NETWORK_ID = 0x{network_id:04X};")
    print(f"constexpr uint8_t NODE_A_MAC[6] = {{{c_array(args.a)}}};")
    print(f"constexpr uint8_t NODE_B_MAC[6] = {{{c_array(args.b)}}};")
    print(f"constexpr uint8_t PMK[16] = {{{c_array(pmk)}}};")
    print(f"constexpr uint8_t LMK[16] = {{{c_array(lmk)}}};")
    print(f"constexpr uint8_t HOP_KEY[32] = {{{c_array(hop)}}};")
    print("} // namespace ghostpair")


if __name__ == "__main__":
    main()
