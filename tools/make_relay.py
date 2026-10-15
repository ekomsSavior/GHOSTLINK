#!/usr/bin/env python3
"""Export mesh membership to a relay without exporting the endpoints' chat key."""
import argparse
import pathlib
import re
from make_pair import parse_mac


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pair", type=pathlib.Path, help="existing ghost_pair_private.h")
    args = parser.parse_args()
    source = args.pair.read_text()
    network = re.search(r"constexpr uint16_t NETWORK_ID = (0x[0-9a-fA-F]{4});", source)
    arrays = {}
    for name, size in (("NODE_A_MAC", 6), ("NODE_B_MAC", 6), ("HOP_KEY", 32)):
        match = re.search(rf"constexpr uint8_t {name}\[{size}\] = \{{([^}}]+)\}};", source)
        if not match:
            parser.error(f"Missing {name} in pair configuration")
        values = re.findall(r"0x[0-9A-Fa-f]{2}", match[1])
        if len(values) != size:
            parser.error(f"Invalid {name}")
        arrays[name] = values
    if not network or int(network[1], 16) == 0:
        parser.error("Missing/invalid network ID")
    try:
        a = parse_mac(":".join(value[2:] for value in arrays["NODE_A_MAC"]))
        b = parse_mac(":".join(value[2:] for value in arrays["NODE_B_MAC"]))
    except argparse.ArgumentTypeError as exc:
        parser.error(str(exc))
    if a == b or not any(int(value, 16) for value in arrays["HOP_KEY"]):
        parser.error("Distinct endpoint addresses and a nonzero membership key are required")
    print("#pragma once\n#include <Arduino.h>\n#define GHOST_PAIR_CONFIGURED 1\nnamespace ghostpair {")
    print(f"constexpr uint16_t NETWORK_ID = {network[1]};")
    for name, values in arrays.items():
        print(f"constexpr uint8_t {name}[{len(values)}] = {{{', '.join(values)}}};")
    print("// Relay membership only. No endpoint chat key is present.")
    print("constexpr uint8_t PMK[16] = {0};\nconstexpr uint8_t LMK[16] = {0};\n}")


if __name__ == "__main__":
    main()
