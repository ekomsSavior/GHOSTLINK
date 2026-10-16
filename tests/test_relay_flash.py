import json
import struct
import sys
from types import SimpleNamespace
import hashlib
import unittest
from unittest.mock import Mock, patch
from pathlib import Path
from tempfile import TemporaryDirectory

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import flash_relay as relay

class FlashError(Exception):
    pass

relay.esptool = SimpleNamespace(FatalError=FlashError, ESP32ROM=Mock(),
    cmds=SimpleNamespace(detect_flash_size=Mock(), erase_region=Mock(), write_flash=Mock()))
M = vars(relay)
A = '02:00:00:00:00:01'
B = '02:00:00:00:00:02'
R = '24:6f:28:11:22:33'
ENDPOINTS = {A, B}


def device(mac, chip='ESP32'):
    esp = Mock()
    esp.read_mac.return_value = tuple(int(x, 16) for x in mac.split(':'))
    esp.CHIP_NAME = chip
    esp.secure_download_mode = False
    esp.get_secure_boot_enabled.return_value = False
    esp.get_flash_encryption_enabled.return_value = False
    return esp



def package_fixture(directory):
    root, package = Path(directory) / 'repo', Path(directory) / 'package'
    (root / 'include').mkdir(parents=True)
    package.mkdir()
    pair = b'constexpr uint8_t NODE_A_MAC[6] = {2,0,0,0,0,1};\nconstexpr uint8_t NODE_B_MAC[6] = {2,0,0,0,0,2};\n'
    (root / 'include/ghost_pair_private.h').write_bytes(pair)
    membership = b'constexpr uint8_t PMK[16] = {0};\nconstexpr uint8_t LMK[16] = {0};\n'
    (package / 'ghost_relay_private.h').write_bytes(membership)
    partitions = b''.join(struct.pack('<HBBII16sI', 0x50AA, kind, subtype, offset, size, label, 0)
        for kind, subtype, offset, size, label in [(0, 0x10, 0x10000, 0x1D0000, b'app0'),
            (1, 0, 0xE000, 0x2000, b'otadata'), (1, 2, 0x3B0000, 0x20000, b'nvs')])
    assets = []
    for name, offset in [('bootloader.bin',0x1000), ('partitions.bin',0x8000), ('boot_app0.bin',0xE000), ('firmware.bin',0x10000)]:
        data = partitions if name == 'partitions.bin' else bytes(32)
        (package / name).write_bytes(data)
        assets.append(dict(name=name, offset=offset, size=len(data), sha256=M['sha256'](data)))
    manifest = dict(display='st7789', pair_sha256=M['sha256'](pair), membership_sha256=M['sha256'](membership),
                    flash_bytes=0x400000, nvs_offset=0x3B0000, nvs_size=0x20000, images=assets)
    (package / 'manifest.json').write_text(json.dumps(manifest))
    return root, package, manifest


class RelayFlashChecks(unittest.TestCase):
    def test_select_third_board_ignores_port_order(self):
        self.assertEqual(M['select_relay']({'/dev/ttyUSB0': R, '/dev/ttyUSB1': B}, ENDPOINTS), ('/dev/ttyUSB0', R))
        self.assertEqual(M['select_relay']({'/dev/ttyUSB0': B, '/dev/ttyUSB1': R}, ENDPOINTS), ('/dev/ttyUSB1', R))

    def test_endpoints_only_are_never_targets(self):
        for identities in ({'p': B}, {'a': A, 'b': B}):
            with self.assertRaises(RuntimeError):
                M['select_relay'](identities, ENDPOINTS)

    def test_two_unknown_devices_are_ambiguous(self):
        with self.assertRaises(RuntimeError):
            M['select_relay']({'r1': R, 'r2': '24:6f:28:11:22:44'}, ENDPOINTS)

    def test_duplicate_identity_is_rejected(self):
        with self.assertRaises(RuntimeError):
            M['select_relay']({'r1': R, 'r2': R}, ENDPOINTS)

    def test_each_endpoint_is_rejected_before_any_flash_operation(self):
        for mac in ENDPOINTS:
            esp = device(mac)
            with self.assertRaises(RuntimeError):
                M['program_relay'](esp, ENDPOINTS, mac, {}, Path('/unused'), Path('/unused'))
            esp.run_stub.assert_not_called()

    def test_target_swap_is_rejected_before_backup_or_erase(self):
        esp = device('24:6f:28:11:22:44')
        with self.assertRaises(RuntimeError):
            M['program_relay'](esp, ENDPOINTS, R, {}, Path('/unused'), Path('/unused'))
        esp.run_stub.assert_not_called()

    def test_wrong_chip_and_security_settings_are_rejected(self):
        esp = device(R, 'ESP32-S3')
        with self.assertRaises(RuntimeError):
            M['check_target'](esp, ENDPOINTS, R)
        for attribute in ('get_secure_boot_enabled', 'get_flash_encryption_enabled'):
            esp = device(R)
            getattr(esp, attribute).return_value = True
            with self.assertRaises(RuntimeError):
                M['check_target'](esp, ENDPOINTS, R)

    def test_bad_backup_blocks_all_writes(self):
        esp, retried = device(R), device(R)
        stub, retry_stub = device(R), device(R)
        esp.run_stub.return_value, retried.run_stub.return_value = stub, retry_stub
        for current in (stub, retry_stub):
            current.read_flash.return_value = bytes(16)
            current.flash_md5sum.return_value = 'incorrect'
        with TemporaryDirectory() as temp, patch.object(M['esptool'], 'ESP32ROM', return_value=retried), patch.object(M['esptool'].cmds, 'detect_flash_size', return_value='4MB'), patch.object(M['esptool'].cmds, 'erase_region') as erase, patch.object(M['esptool'].cmds, 'write_flash') as write:
            with self.assertRaisesRegex(RuntimeError, 'no erase or firmware write'):
                M['program_relay'](esp, ENDPOINTS, R, {'flash_bytes': 16}, Path(temp) / 'backup', Path('/unused'))
            erase.assert_not_called()
            write.assert_not_called()

    def test_serial_failure_restarts_same_handle_and_checks_identity(self):
        esp, retried = device(R), device(R)
        stub, retry_stub = device(R), device(R)
        esp.run_stub.return_value, retried.run_stub.return_value = stub, retry_stub
        stub.read_flash.side_effect = M['esptool'].FatalError('Invalid head of packet (0xFF)')
        payload = bytes(range(16))
        retry_stub.read_flash.return_value = payload
        retry_stub.flash_md5sum.return_value = hashlib.md5(payload).hexdigest()
        with patch.object(M['esptool'], 'ESP32ROM', return_value=retried) as reconnect, patch.object(M['esptool'].cmds, 'detect_flash_size', return_value='4MB'):
            result_stub, data = M['read_verified_backup'](esp, ENDPOINTS, R, {'flash_bytes': len(payload)})
        self.assertIs(result_stub, retry_stub)
        self.assertEqual(data, payload)
        reconnect.assert_called_once_with(port=esp._port, baud=115200)
        retried.connect.assert_called_once_with(mode='default_reset', attempts=3)
        retried.read_mac.assert_called_once()
        stub.change_baud.assert_not_called()
        retry_stub.change_baud.assert_called_once_with(57600)

    def test_retry_rejects_endpoint_identity_before_uploading_stub(self):
        esp, endpoint = device(R), device(B)
        stub = device(R)
        esp.run_stub.return_value = stub
        stub.read_flash.side_effect = M['esptool'].FatalError('serial noise')
        with patch.object(M['esptool'], 'ESP32ROM', return_value=endpoint), patch.object(M['esptool'].cmds, 'detect_flash_size', return_value='4MB'):
            with self.assertRaisesRegex(RuntimeError, 'Refusing to erase or flash an endpoint'):
                M['read_verified_backup'](esp, ENDPOINTS, R, {'flash_bytes': 16})
        endpoint.run_stub.assert_not_called()

    def test_chunked_backup_covers_all_addresses_and_matches_full_digest(self):
        esp, stub = device(R), device(R)
        esp.run_stub.return_value = stub
        payload = bytes(range(256)) * 513
        stub.read_flash.side_effect = lambda offset, length: payload[offset:offset + length]
        stub.flash_md5sum.return_value = hashlib.md5(payload).hexdigest()
        with patch.object(M['esptool'].cmds, 'detect_flash_size', return_value='4MB'):
            result_stub, data = M['read_verified_backup'](esp, ENDPOINTS, R, {'flash_bytes': len(payload)})
        self.assertEqual(data, payload)
        self.assertEqual([call.args for call in stub.read_flash.call_args_list], [(0, 65536), (65536, 65536), (131072, 256)])
        stub.flash_md5sum.assert_called_once_with(0, len(payload))
        stub.change_baud.assert_not_called()

    def test_package_checksums_pairing_and_partition_binary(self):
        with TemporaryDirectory() as temp:
            root, package, expected = package_fixture(temp)
            manifest, endpoints, membership = M['checked_package'](package, root)
            self.assertEqual(endpoints, ENDPOINTS)
            self.assertEqual(manifest, expected)
            self.assertIn(b'constexpr uint8_t LMK[16] = {0};', membership)
            (package / 'firmware.bin').write_bytes(b'corrupt')
            with self.assertRaisesRegex(RuntimeError, 'Firmware checksum failed'):
                M['checked_package'](package, root)

    def test_changed_pairing_and_invalid_partitions_are_rejected(self):
        with TemporaryDirectory() as temp:
            root, package, manifest = package_fixture(temp)
            pair = root / 'include/ghost_pair_private.h'
            original = pair.read_bytes()
            pair.write_bytes(original + b'changed')
            with self.assertRaisesRegex(RuntimeError, 'pairing changed'):
                M['checked_package'](package, root)
            pair.write_bytes(original)
            partition_file = package / 'partitions.bin'
            data = bytearray(partition_file.read_bytes())
            struct.pack_into('<I', data, 4, 0x30000)
            partition_file.write_bytes(data)
            manifest['images'][1]['sha256'] = M['sha256'](data)
            (package / 'manifest.json').write_text(json.dumps(manifest))
            with self.assertRaisesRegex(RuntimeError, 'partition binary'):
                M['checked_package'](package, root)

    def test_existing_relay_storage_is_preserved_and_migrations_initialized(self):
        for matching, force_reset, should_erase in [(True, False, False), (False, False, True), (True, True, True)]:
            with self.subTest(matching=matching, force_reset=force_reset), TemporaryDirectory() as temp:
                root, package, manifest = package_fixture(temp)
                backup = bytearray(0x400000)
                if matching:
                    partitions = (package / 'partitions.bin').read_bytes()
                    backup[0x8000:0x8000 + len(partitions)] = partitions
                esp, stub = device(R), device(R)
                with patch.object(relay, 'read_verified_backup', return_value=(stub, bytes(backup))), patch.object(relay.esptool.cmds, 'erase_region') as erase, patch.object(relay.esptool.cmds, 'write_flash') as write:
                    M['program_relay'](esp, ENDPOINTS, R, manifest, Path(temp) / 'backup', package, force_reset)
                    self.assertEqual(erase.called, should_erase)
                    if should_erase:
                        self.assertEqual(erase.call_args.args[1].address, 0x3B0000)
                        self.assertEqual(erase.call_args.args[1].size, 0x20000)
                    write.assert_called_once()
                    self.assertEqual((Path(temp) / 'backup/original-flash.bin').read_bytes(), backup)



if __name__ == '__main__':
    unittest.main(verbosity=2)
