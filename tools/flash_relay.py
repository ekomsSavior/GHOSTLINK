#!/usr/bin/env python3
"""Build, back up, and flash a CYD relay using an existing endpoint pairing."""
import argparse
import codecs
from contextlib import ExitStack, redirect_stdout, redirect_stderr
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import site
import struct
import subprocess
import sys
import time
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
# Loaded after PlatformIO has installed the pinned flashing tools.
esptool = None
SerialException = OSError


def load_flash_tools(core):
    global esptool, SerialException
    tool = core / 'packages/tool-esptoolpy'
    site.addsitedir(str(tool / '_contrib'))
    sys.path.insert(0, str(tool))
    import esptool as module
    from serial import SerialException as serial_error
    esptool, SerialException = module, serial_error
    if not esptool.__version__.startswith('4.'):
        raise RuntimeError('This helper requires the esptool 4.x bundled with the pinned PlatformIO platform')


class Tee:
    def __init__(self, *streams):
        self.streams = streams
    def write(self, text):
        for stream in self.streams:
            stream.write(text)
            stream.flush()
        return len(text)
    def flush(self):
        for stream in self.streams:
            stream.flush()
    def isatty(self):
        return False


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def config_mac(text, name):
    found = re.search(name + r'\[6\]\s*=\s*\{([^}]+)\}', text)
    if not found:
        raise RuntimeError('Cannot read endpoint identity from existing pairing')
    values = [int(x.strip(), 0) for x in found.group(1).split(',')]
    if len(values) != 6 or any(not 0 <= x <= 255 for x in values):
        raise RuntimeError('Invalid endpoint address')
    return ':'.join(f'{x:02x}' for x in values)


def prepare_build(display, root=ROOT):
    pair = root / 'include/ghost_pair_private.h'
    if not pair.is_file():
        raise RuntimeError('Configure A and B first; include/ghost_pair_private.h is required')
    original_pair = pair.read_bytes()
    membership = subprocess.check_output([sys.executable, str(root / 'tools/make_relay.py'), str(pair)])
    save_membership(membership, root)
    environment = 'ghost-relay-st7789' if display == 'st7789' else 'ghost-relay'
    pio = shutil.which('pio') or str(Path(sys.executable).parent / 'pio')
    core = Path(os.environ.get('PLATFORMIO_CORE_DIR', Path.home() / '.platformio')).expanduser().resolve()
    subprocess.run([pio, 'run', '-e', environment], cwd=root, check=True)
    if pair.read_bytes() != original_pair or (root / 'include/ghost_relay_private.h').read_bytes() != membership:
        raise RuntimeError('Provisioning changed during the build; rebuild before flashing')
    build = root / '.pio/build' / environment
    package = root / '.pio/relay-flash' / datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    package.mkdir(parents=True, mode=0o700)
    sources = [
        ('bootloader.bin', 0x1000, build / 'bootloader.bin'),
        ('partitions.bin', 0x8000, build / 'partitions.bin'),
        ('boot_app0.bin', 0xE000, core / 'packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin'),
        ('firmware.bin', 0x10000, build / 'firmware.bin'),
    ]
    images = []
    for name, offset, source in sources:
        data = source.read_bytes()
        (package / name).write_bytes(data)
        images.append(dict(name=name, offset=offset, size=len(data), sha256=sha256(data)))
    (package / 'ghost_relay_private.h').write_bytes(membership)
    from check_flash_layout import read_partitions, validate_layout
    validate_layout(root / 'partitions.csv', 0x10000, [(0xE000, 'boot_app0.bin')])
    nvs = next(part for part in read_partitions(root / 'partitions.csv') if part[0] == 'nvs')
    manifest = dict(display=display, pair_sha256=sha256(original_pair),
                    membership_sha256=sha256(membership), flash_bytes=0x400000,
                    nvs_offset=nvs[3], nvs_size=nvs[4], images=images)
    (package / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return package, core


def checked_package(package, root=ROOT):
    manifest = json.loads((package / 'manifest.json').read_text())
    pair = (root / 'include/ghost_pair_private.h').read_bytes()
    if sha256(pair) != manifest['pair_sha256']:
        raise RuntimeError('The endpoint pairing changed; rebuild the relay before flashing')
    addresses = {config_mac(pair.decode(), name) for name in ('NODE_A_MAC', 'NODE_B_MAC')}
    if len(addresses) != 2:
        raise RuntimeError('The existing pairing does not have two distinct endpoints')
    expected = [('bootloader.bin', 0x1000), ('partitions.bin', 0x8000),
                ('boot_app0.bin', 0xE000), ('firmware.bin', 0x10000)]
    if [(x['name'], x['offset']) for x in manifest['images']] != expected:
        raise RuntimeError('Unexpected flash image layout')
    if (manifest['flash_bytes'], manifest['nvs_offset'], manifest['nvs_size']) != (0x400000, 0x3B0000, 0x20000):
        raise RuntimeError('Unexpected storage layout')
    previous_end = 0
    for asset in manifest['images']:
        data = (package / asset['name']).read_bytes()
        if len(data) != asset['size'] or sha256(data) != asset['sha256']:
            raise RuntimeError('Firmware checksum failed: ' + asset['name'])
        if asset['offset'] < previous_end:
            raise RuntimeError('Overlapping flash images')
        previous_end = asset['offset'] + len(data)
    if previous_end > 0x1E0000:
        raise RuntimeError('Application exceeds its partition')
    # Decode the generated binary, not just the CSV or a manifest assertion.
    partitions = (package / 'partitions.bin').read_bytes()
    decoded = {}
    for offset in range(0, len(partitions) - 31, 32):
        magic, kind, subtype, start, size, label, flags = struct.unpack_from('<HBBII16sI', partitions, offset)
        if magic == 0x50AA:
            decoded[label.split(b'\0', 1)[0].decode()] = (kind, subtype, start, size)
    if decoded.get('app0') != (0, 0x10, 0x10000, 0x1D0000) or decoded.get('otadata') != (1, 0, 0xE000, 0x2000) or decoded.get('nvs') != (1, 2, 0x3B0000, 0x20000):
        raise RuntimeError('Generated partition binary does not match the supported flash layout')
    membership = (package / 'ghost_relay_private.h').read_bytes()
    if sha256(membership) != manifest['membership_sha256']:
        raise RuntimeError('Relay configuration checksum failed')
    for key in ('PMK', 'LMK'):
        if not re.search(r'constexpr uint8_t ' + key + r'\[16\] = \{0\};', membership.decode()):
            raise RuntimeError('Unexpected endpoint key in relay configuration')
    return manifest, addresses, membership


def grant_serial_access(ports):
    if not sys.platform.startswith("linux"):
        return
    import pwd
    blocked = [port for port in ports if not os.access(port, os.R_OK | os.W_OK)]
    if blocked:
        if not shutil.which('setfacl'):
            raise RuntimeError('setfacl is unavailable; serial access needs fixing')
        username = pwd.getpwuid(os.getuid()).pw_name
        print('Granting your user temporary serial access to: ' + ', '.join(blocked), flush=True)
        subprocess.run(['sudo', 'setfacl', '-m', 'u:' + username + ':rw', *blocked], check=True)
        if any(not os.access(port, os.R_OK | os.W_OK) for port in ports):
            raise RuntimeError('Serial access is still unavailable')


def mac_of(esp):
    return ':'.join(f'{x:02x}' for x in esp.read_mac())


def select_relay(identities, endpoints):
    if len(set(identities.values())) != len(identities):
        raise RuntimeError('Duplicate device identities; refusing to choose a target')
    candidates = [(port, mac) for port, mac in identities.items() if mac not in endpoints]
    if len(candidates) != 1:
        raise RuntimeError('Expected exactly one CYD besides A/B; found ' + str(len(candidates)))
    return candidates[0]


def check_target(esp, endpoints, expected_mac):
    mac = mac_of(esp)
    if mac in endpoints:
        raise RuntimeError('Refusing to erase or flash an endpoint: ' + mac)
    if mac != expected_mac:
        raise RuntimeError('Device identity changed; no write allowed')
    if esp.CHIP_NAME != 'ESP32':
        raise RuntimeError('This build requires the original ESP32 CYD')
    if esp.secure_download_mode or esp.get_secure_boot_enabled() or esp.get_flash_encryption_enabled():
        raise RuntimeError('This board has security settings incompatible with this plain firmware image')


def save_membership(membership, root=ROOT):
    destination = root / 'include/ghost_relay_private.h'
    if destination.exists():
        if destination.read_bytes() != membership:
            raise RuntimeError('Existing relay membership differs; leaving it unchanged')
    else:
        fd = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'wb') as file:
            file.write(membership)
            file.flush()
            os.fsync(file.fileno())
    print('Relay membership is saved locally; endpoint pairing is unchanged.')


def read_verified_backup(esp, endpoints, expected_mac, manifest):
    """Retry only reads, restarting the loader on the same open USB handle."""
    port = esp._port
    for attempt, baud in enumerate((115200, 57600), start=1):
        try:
            if attempt > 1:
                # A fresh protocol reader discards the broken SLIP state. Passing the
                # serial object retains the selected device instead of reopening a path.
                esp = esptool.ESP32ROM(port=port, baud=115200)
                esp.connect(mode='default_reset', attempts=3)
            check_target(esp, endpoints, expected_mac)
            stub = esp.run_stub()
            if baud != 115200:
                stub.change_baud(baud)
            size = esptool.cmds.detect_flash_size(stub)
            if size != '4MB':
                raise RuntimeError('Expected 4 MB flash, detected ' + str(size))
            stub.flash_set_parameters(manifest['flash_bytes'])
            print(f'Backup attempt {attempt}/2 at {baud} baud; reading in 64 KiB blocks.')
            print('The 4 MB backup takes several minutes; progress is printed below.', flush=True)
            data = bytearray()
            for offset in range(0, manifest['flash_bytes'], 0x10000):
                length = min(0x10000, manifest['flash_bytes'] - offset)
                # read_flash verifies the digest of each individual block.
                block = stub.read_flash(offset, length)
                if len(block) != length:
                    raise esptool.FatalError('Incomplete backup block')
                data.extend(block)
                print(f'Backup: {len(data) * 100 // manifest["flash_bytes"]}% '
                      f'({len(data)} / {manifest["flash_bytes"]} bytes)', flush=True)
            device_md5 = stub.flash_md5sum(0, len(data))
            if isinstance(device_md5, bytes):
                device_md5 = device_md5.decode()
            if hashlib.md5(data).hexdigest() != device_md5.lower():
                raise esptool.FatalError('Full backup verification failed')
            return stub, bytes(data)
        except (esptool.FatalError, SerialException, OSError) as error:
            if attempt == 2:
                raise RuntimeError('Backup still failed; no erase or firmware write was performed. '
                                   'Try a different USB data cable or direct USB port, then rerun. '
                                   'Last error: ' + str(error)) from error
            print('Backup transfer failed before any erase/write: ' + str(error))
            print('Restarting the bootloader on the same device and retrying at 57600 baud.', flush=True)
    raise RuntimeError('Backup did not complete')


def program_relay(esp, endpoints, expected_mac, manifest, backup_dir, package, reset_storage=False):
    # No target flash operation occurs before this identity gate and verified backup.
    check_target(esp, endpoints, expected_mac)
    stub, data = read_verified_backup(esp, endpoints, expected_mac, manifest)
    backup_dir.mkdir(mode=0o700, parents=True, exist_ok=False)
    backup_file = backup_dir / 'original-flash.bin'
    with backup_file.open('xb') as file:
        file.write(data)
        file.flush()
        os.fsync(file.fileno())
    if sha256(backup_file.read_bytes()) != sha256(data):
        raise RuntimeError('Saved backup verification failed; no flash writes performed')
    record = {'mac': expected_mac, 'flash_bytes': len(data), 'sha256': sha256(data),
              'description': 'Full flash contents before this relay upload'}
    (backup_dir / 'backup.json').write_text(json.dumps(record, indent=2) + '\n')
    print('Verified original flash backup: ' + str(backup_file))
    # The same open serial connection is retained from identification through write.
    check_target(stub, endpoints, expected_mac)
    partition_image = (package / 'partitions.bin').read_bytes()
    if reset_storage or data[0x8000:0x8000 + len(partition_image)] != partition_image:
        print('Initializing relay message storage for the selected partition layout...')
        esptool.cmds.erase_region(stub, SimpleNamespace(address=manifest['nvs_offset'],
                                                       size=manifest['nvs_size'], force=False))
    else:
        print('Existing GHOSTLINK partition layout: preserving relay message storage.')
    check_target(stub, endpoints, expected_mac)
    with ExitStack() as files:
        images = [(image['offset'], files.enter_context((package / image['name']).open('rb')))
                  for image in manifest['images']]
        esptool.cmds.write_flash(stub, SimpleNamespace(
            addr_filename=images, compress=True, no_compress=False, no_stub=False,
            encrypt=False, encrypt_files=None, erase_all=False, force=False,
            ignore_flash_encryption_efuse_setting=False, flash_mode='keep',
            flash_freq='keep', flash_size='keep'))
    print('Relay firmware written and verified. Restarting for the radio check...')
    return stub


def monitor_relay(stub, expected_mac, endpoints, serial_log, display):
    port = stub._port
    port.baudrate = 115200
    port.timeout = 0
    port.write_timeout = 2
    port.dtr = False
    port.reset_input_buffer()
    stub.hard_reset()
    decoder = codecs.getincrementaldecoder('utf-8')('replace')
    text = ''
    pending = ''
    started = time.monotonic()
    status_at = started + 4
    with serial_log.open('w') as log:
        while time.monotonic() - started < 70:
            data = decoder.decode(port.read(8192))
            if data:
                text += data
                log.write(data)
                log.flush()
                pending += data
                while '\n' in pending:
                    line, pending = pending.split('\n', 1)
                    print('[R] ' + line.rstrip('\r'), flush=True)
                if any(marker in text for marker in ('[fatal]', 'Guru Meditation', 'abort() was called', 'missing send callback')) or text.count('rst:0x') > 1:
                    raise RuntimeError('Relay reported an unstable startup; see ' + str(serial_log))
            if time.monotonic() >= status_at:
                port.write(b'/status\n/neighbors\n')
                status_at += 6
            statuses = re.findall(r'\[status\][^\r\n]+', text)
            ready = bool(statuses and 'role=R ready=1 configured=1' in statuses[-1])
            neighbors = set(mac.lower() for mac in re.findall(r'\[(?:mesh|neighbor)\] (?:neighbor )?((?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2})', text))
            live = bool(statuses and re.search(r'neighbors=[1-9]\d*', statuses[-1]))
            identity = '[radio] local STA MAC: ' + expected_mac.upper() in text
            screen = '[display] GHOSTLINK CYD ' + display.upper() + ' 320x240 rotation=1' in text
            ble = '[ble] rendezvous ready' in text
            if time.monotonic() - started >= 20 and ready and identity and screen and ble and live and neighbors & endpoints:
                print('\nPASS: Relay R started and discovered a configured endpoint over the radio.')
                print('Check the relay screen. A/B firmware was not changed.')
                print('Place the relay between the endpoints for the next forwarding/range test.')
                print('Serial log: ' + str(serial_log))
                return
            time.sleep(0.02)
    raise RuntimeError('Relay was flashed, but startup/neighbor checks did not all pass; keep A and B powered nearby and share ' + str(serial_log))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', help='Optional explicit relay port; hardware identity is still checked')
    parser.add_argument('--display', choices=('ili9341', 'st7789'), default='ili9341', help='Panel controller; dual-USB CYDs commonly use st7789')
    parser.add_argument('--log-dir', type=Path, default=ROOT / '.pio/bench')
    parser.add_argument('--build-only', action='store_true', help='Prepare and verify firmware without opening serial ports')
    parser.add_argument('--reset-storage', action='store_true', help='Clear this relay’s stored packets even if its partition layout is unchanged')
    args = parser.parse_args()
    if hasattr(os, 'geteuid') and os.geteuid() == 0:
        raise RuntimeError('Run as your normal user; sudo is requested only if a serial-port ACL is needed')
    os.umask(0o077)
    package, core = prepare_build(args.display)
    manifest, endpoints, membership = checked_package(package)
    if args.build_only:
        print('PASS: relay firmware built and verified in ' + str(package))
        return
    load_flash_tools(core)
    from serial.tools import list_ports
    ports = [args.port] if args.port else sorted(p.device for p in list_ports.comports()
        if (p.vid, p.pid) in ((0x1A86, 0x7523), (0x1A86, 0x55D4), (0x10C4, 0xEA60)))
    if not 1 <= len(ports) <= 3:
        raise RuntimeError('Connect a relay CYD or specify --port; found ' + repr(ports))
    grant_serial_access(ports)
    stamp = datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    connections = {}
    identities = {}
    target_port = None
    monitoring_started = False
    args.log_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
    with (args.log_dir / ('relay-flash-' + stamp + '.log')).open('w') as log:
        tee = Tee(sys.stdout, log)
        with redirect_stdout(tee), redirect_stderr(tee):
            try:
                for port in ports:
                    esp = esptool.detect_chip(port=port, baud=115200, connect_attempts=3)
                    connections[port] = esp
                    esp._port.exclusive = True
                    if esp.secure_download_mode:
                        raise RuntimeError('Cannot identify a device in secure download mode')
                    identities[port] = mac_of(esp)
                    label = 'protected endpoint' if identities[port] in endpoints else 'relay candidate'
                    print(port + ': ' + identities[port] + ' (' + label + ')')
                    if identities[port] in endpoints:
                        esp.hard_reset()
                        esp._port.close()
                target_port, target_mac = select_relay(identities, endpoints)
                print('Selected relay only: ' + target_mac + ' on ' + target_port)
                save_membership(membership)
                backup_dir = args.log_dir / 'relay-backups' / (target_mac.replace(':', '') + '-' + stamp)
                stub = program_relay(connections[target_port], endpoints, target_mac, manifest, backup_dir, package, args.reset_storage)
                connections[target_port] = stub
                monitoring_started = True
                monitor_relay(stub, target_mac, endpoints, args.log_dir / ('board-relay-' + stamp + '.log'), args.display)
            except BaseException as error:
                print('\nSTOP: ' + str(error), flush=True)
                raise
            finally:
                for port, esp in connections.items():
                    if esp._port.is_open:
                        if port != target_port or not monitoring_started:
                            try:
                                esp.hard_reset()
                            except Exception:
                                pass
                        esp._port.close()


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        sys.exit('Interrupted. Any saved flash backup remains in the selected log directory.')
    except Exception as error:
        sys.exit('Relay flash stopped: ' + str(error))
