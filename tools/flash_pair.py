#!/usr/bin/env python3
"""Provision two connected classic CYDs and verify serial-observed RF delivery."""
import argparse
from contextlib import ExitStack
from datetime import datetime
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

import serial
from serial.tools import list_ports

ROOT = Path(__file__).resolve().parents[1]
MAC_RE = r'(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}'


def config_mac(text, name):
    found = re.search(name + r'\[6\]\s*=\s*\{([^}]+)\}', text)
    if not found:
        raise RuntimeError('Cannot read existing pairing addresses; keeping the private file unchanged')
    return ':'.join(f'{int(x.strip(), 0):02x}' for x in found.group(1).split(','))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--a-mac', required=True, help='Previously identified station MAC of board A')
    parser.add_argument('--port', action='append', help='Explicit port; supply twice, or auto-detect two CH340 CYDs')
    parser.add_argument('--reset-storage', action='store_true', help='Erase the message-storage partition on both boards for initial setup/recovery')
    parser.add_argument('--log-dir', type=Path, default=ROOT / '.pio' / 'bench')
    args = parser.parse_args()
    if not re.fullmatch(MAC_RE, args.a_mac):
        parser.error('--a-mac must contain six hexadecimal bytes')
    a_mac = args.a_mac.lower()
    ports = args.port or sorted(p.device for p in list_ports.comports() if (p.vid, p.pid) == (0x1A86, 0x7523))
    if len(ports) != 2 or len(set(ports)) != 2:
        raise RuntimeError('Connect both CYDs; expected exactly two serial ports, found ' + repr(ports))
    pio = shutil.which('pio') or str(Path(sys.executable).parent / 'pio')
    core = Path(os.environ.get('PLATFORMIO_CORE_DIR', Path.home() / '.platformio'))
    esptool = core / 'packages' / 'tool-esptoolpy' / 'esptool.py'
    if not esptool.exists():
        raise RuntimeError('Build once with PlatformIO to install its ESP32 flashing tools')
    args.log_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime('%Y%m%d-%H%M%S')
    log_path = args.log_dir / ('pair-' + stamp + '.log')
    with log_path.open('w') as log:
        def emit(text):
            print(text, end='', flush=True)
            log.write(text)
            log.flush()

        def run(command):
            proc = subprocess.Popen([str(x) for x in command], cwd=ROOT,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, errors='replace')
            output = ''
            for line in proc.stdout:
                output += line
                emit(line)
            if proc.wait():
                raise RuntimeError('Command failed; see ' + str(log_path))
            return output

        def esp(port, *command):
            return run([sys.executable, esptool, '--chip', 'esp32', '--port', port,
                        '--baud', '460800', '--after', 'hard_reset', *command])

        def identify(port):
            output = esp(port, 'read_mac')
            found = re.search(r'MAC:\s*(' + MAC_RE + ')', output)
            if not found:
                raise RuntimeError('Could not identify ' + port + '; no firmware written')
            return found.group(1).lower()

        identities = {port: identify(port) for port in ports}
        if len(set(identities.values())) != 2 or a_mac not in identities.values():
            raise RuntimeError('The original board A is not uniquely present; refusing to assign roles')
        role_ports = {'A': next(p for p, m in identities.items() if m == a_mac),
                      'B': next(p for p, m in identities.items() if m != a_mac)}
        b_mac = identities[role_ports['B']]
        emit(f'ROLE A: {a_mac} on {role_ports["A"]}\nROLE B: {b_mac} on {role_ports["B"]}\n')
        private = ROOT / 'include' / 'ghost_pair_private.h'
        if private.exists():
            text = private.read_text()
            if config_mac(text, 'NODE_A_MAC') != a_mac or config_mac(text, 'NODE_B_MAC') != b_mac:
                raise RuntimeError('Existing pairing is for other boards; keeping it unchanged')
            emit('Reusing the existing private pairing.\n')
        else:
            text = subprocess.check_output([sys.executable, str(ROOT / 'tools' / 'make_pair.py'),
                                            '--a', a_mac, '--b', b_mac], text=True)
            fd = os.open(private, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, 'w') as config:
                config.write(text)
            emit('Created one private pairing for both boards. Keys stay on this machine.\n')
        run([pio, 'run', '-e', 'ghost-a', '-e', 'ghost-b'])
        for role, port in role_ports.items():
            if identify(port) != identities[port]:
                raise RuntimeError('A serial device changed during the build; no write to this port')
            if args.reset_storage:
                from check_flash_layout import read_partitions
                nvs = next(p for p in read_partitions(ROOT / 'partitions.csv') if p[0] == 'nvs')
                emit(f'Initializing {role} message storage.\n')
                esp(port, 'erase_region', hex(nvs[3]), hex(nvs[4]))
            run([pio, 'run', '-e', 'ghost-' + role.lower(), '-t', 'upload', '--upload-port', port])
        with ExitStack() as stack:
            boards = {}
            buffers = {'A': '', 'B': ''}
            for role, port in role_ports.items():
                board = serial.Serial(port=None, baudrate=115200, timeout=0, write_timeout=2, exclusive=True)
                board.port = port
                board.dtr = False
                board.rts = False
                board.open()
                stack.callback(board.close)
                boards[role] = board
            for board in boards.values():
                board.rts = True
            time.sleep(0.15)
            for board in boards.values():
                board.rts = False

            def pump(seconds, until=None):
                deadline = time.monotonic() + seconds
                status_at = time.monotonic() + 4
                while time.monotonic() < deadline:
                    for role, board in boards.items():
                        data = board.read(8192).decode('utf-8', errors='replace')
                        if data:
                            buffers[role] += data
                            emit('[' + role + '] ' + data)
                    if time.monotonic() >= status_at:
                        for board in boards.values():
                            board.write(b'/status\n')
                        status_at += 8
                    if until and until():
                        return True
                    time.sleep(0.02)
                return False

            emit('Checking both configured boards after a restart...\n')
            pump(14)
            for role in boards:
                if f'role={role} ready=1 configured=1' not in buffers[role]:
                    raise RuntimeError(role + ' did not start correctly; keep both boards connected and share this log')
                if '[fatal]' in buffers[role] or 'Guru Meditation' in buffers[role] or buffers[role].count('rst:0x') > 2:
                    raise RuntimeError(role + ' reported an unstable startup; see log')
            buffers = {'A': '', 'B': ''}
            token = str(time.time_ns())[-10:]
            messages = {'A': 'GHOST A to B ' + token, 'B': 'GHOST B to A ' + token}
            for role, board in boards.items():
                board.write((messages[role] + '\n').encode())

            def delivered():
                for sender, receiver in [('A', 'B'), ('B', 'A')]:
                    queued = re.search(r'\[queued #(\d+)\] ' + re.escape(messages[sender]), buffers[sender])
                    received = re.search(r'\[received #\d+\] ' + re.escape(messages[sender]), buffers[receiver])
                    if not queued or not received or f'[delivered #{queued.group(1)}] peer accepted' not in buffers[sender]:
                        return False
                return True

            emit('Sending one test message each way; waiting up to 90 seconds for destination receipts.\n')
            if not pump(90, delivered):
                raise RuntimeError('Both boards were flashed, but two-way delivery did not pass within 90 seconds; keep both connected')
            emit('\nPASS: A to B and B to A received, with destination acknowledgments.\n')
            emit('Check both screens and touch keyboards next; this verifies radio delivery, not display/touch operation.\n')
        emit('Log saved to ' + str(log_path) + '\n')


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, OSError, serial.SerialException, subprocess.CalledProcessError) as error:
        print('\nSTOP: ' + str(error), file=sys.stderr)
        sys.exit(1)
