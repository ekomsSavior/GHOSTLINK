#!/usr/bin/env python3
"""Run host protocol regressions and pair provisioning checks (no hardware)."""
import pathlib
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]

def main():
    compiler = shutil.which("g++")
    if not compiler:
        raise SystemExit("Install g++ to run host protocol tests")
    with tempfile.TemporaryDirectory(prefix="ghostlink-tests-") as directory:
        binary = str(pathlib.Path(directory) / "protocol")
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-DGHOST_HOST_TEST", "-I", str(ROOT / "include"), "-I", str(ROOT / "src"),
                        str(ROOT / "tests/protocol_test.cpp"), "-lcrypto", "-o", binary], check=True)
        subprocess.run([binary], check=True)
    import sys
    generator = [sys.executable, str(ROOT / "tools/make_pair.py")]
    def config(a, b):
        return subprocess.run(generator + ["--a", a, "--b", b], capture_output=True, text=True)
    good = config("02:00:00:00:00:01", "02:00:00:00:00:02")
    assert good.returncode == 0 and "#define GHOST_PAIR_CONFIGURED 1" in good.stdout
    again = config("02:00:00:00:00:01", "02:00:00:00:00:02")
    assert good.stdout != again.stdout, "Every generated pair must get fresh keys"
    for invalid in ["00:00:00:00:00:00", "FF:FF:FF:FF:FF:FF", "01:00:00:00:00:01",
                    "2:00:00:00:00:01", "GG:00:00:00:00:01", "02:00:00:00:00:02"]:
        result = config(invalid, "02:00:00:00:00:02")
        assert result.returncode != 0 and not result.stdout
    with tempfile.TemporaryDirectory(prefix="ghostlink-provision-") as directory:
        path = pathlib.Path(directory) / "pair.h"
        path.write_text(good.stdout)
        relay = subprocess.run([sys.executable, str(ROOT / "tools/make_relay.py"), str(path)], capture_output=True, text=True, check=True).stdout
        import re
        chat_key = re.search(r"LMK\[16\] = (.+);", good.stdout)[1]
        assert chat_key not in relay and "LMK[16] = {0}" in relay
        for name in ("HOP_KEY", "NODE_A_MAC", "NODE_B_MAC"):
            declaration = next(line for line in good.stdout.splitlines() if f" {name}[" in line)
            assert declaration in relay
        path.write_text("invalid")
        bad = subprocess.run([sys.executable, str(ROOT / "tools/make_relay.py"), str(path)], capture_output=True, text=True)
        assert bad.returncode and not bad.stdout
    print("PASS: provisioning uniqueness, MAC validation and relay export without chat key")

if __name__ == "__main__":
    main()
