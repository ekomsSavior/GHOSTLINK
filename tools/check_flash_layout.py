"""Reject builds whose serial upload addresses disagree with their boot table."""
import csv
from pathlib import Path


def read_partitions(path):
    rows = []
    for row in csv.reader(Path(path).read_text().splitlines()):
        if not row or row[0].lstrip().startswith('#'):
            continue
        name, kind, subtype, offset, size = (part.strip() for part in row[:5])
        rows.append((name, kind, subtype, int(offset, 0), int(size, 0)))
    return rows


def validate_layout(path, app_offset, extra_images):
    rows = read_partitions(path)
    previous_end = 0x9000
    for name, kind, subtype, offset, size in sorted(rows, key=lambda p: p[3]):
        alignment = 0x10000 if kind == 'app' else 0x1000
        if offset < previous_end or offset % alignment or size <= 0 or size % 0x1000:
            raise ValueError('Invalid/overlapping partition: ' + name)
        previous_end = offset + size
        if previous_end > 0x400000:
            raise ValueError('Partition exceeds the CYD 4 MB flash: ' + name)
    app = next(p for p in rows if p[1:3] == ('app', 'ota_0'))
    ota = next(p for p in rows if p[1:3] == ('data', 'ota'))
    if int(str(app_offset), 0) != app[3]:
        raise ValueError('App upload address disagrees with ota_0; firmware would not boot')
    initializers = [(int(str(offset), 0), Path(str(image))) for offset, image in extra_images
                    if Path(str(image)).name == 'boot_app0.bin']
    if len(initializers) != 1 or initializers[0][0] != ota[3] or ota[4] != 0x2000:
        raise ValueError('OTA initializer upload address disagrees with otadata')
    nvs = next(p for p in rows if p[0] == 'nvs')
    if nvs[4] < 0x17000:
        raise ValueError('GHOSTLINK durable queues need at least 92 KiB of NVS')


try:
    Import('env')  # Supplied by PlatformIO/SCons only.
except NameError:
    pass
else:
    validate_layout(
        Path(env.subst('$PROJECT_DIR')) / env.BoardConfig().get('build.partitions'),
        env.subst('$ESP32_APP_OFFSET'),
        [(env.subst(str(offset)), env.subst(str(image)))
         for offset, image in env.get('FLASH_EXTRA_IMAGES', [])],
    )
