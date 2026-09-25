#!/usr/bin/env python3
"""Build the Magisk ZIP from this repository's module files."""
import json
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

ROOT = Path(__file__).resolve().parent
OUTPUT = ROOT / 'qptp-magisk.zip'
FILES = ('module.prop', 'service.sh', 'customize.sh', 'bin/qpro_streamer')


def properties(path):
    result = {}
    for line in path.read_text(encoding='utf-8').splitlines():
        if line and not line.startswith('#'):
            key, separator, value = line.partition('=')
            if not separator:
                raise ValueError(f'Invalid module.prop line: {line!r}')
            result[key] = value
    return result


prop = properties(ROOT / 'module.prop')
manifest = json.loads((ROOT / 'update.json').read_text(encoding='utf-8'))
module_code = int(prop['versionCode'])
published_code = int(manifest['versionCode'])
if published_code > module_code:
    raise SystemExit('update.json advertises a newer version than module.prop')
if published_code == module_code and prop['version'] != manifest['version']:
    raise SystemExit('module.prop and update.json names differ at the same versionCode')
if prop.get('updateJson') != 'https://raw.githubusercontent.com/Lateir/qptp-module/main/update.json':
    raise SystemExit('module.prop updateJson does not match this repository')
if not (ROOT / 'bin/qpro_streamer').read_bytes().startswith(b'\x7fELF'):
    raise SystemExit('Missing aarch64 ELF binary in bin/qpro_streamer')

with ZipFile(OUTPUT, 'w', ZIP_DEFLATED) as archive:
    for name in FILES:
        data = (ROOT / name).read_bytes()
        if name.endswith(('.sh', '.prop')) and b'\r' in data:
            raise SystemExit(f'{name} must use LF line endings')
        info = ZipInfo(name)
        info.compress_type = ZIP_DEFLATED
        info.create_system = 3
        info.external_attr = ((0o755 if name.endswith(('.sh', 'qpro_streamer')) else 0o644) << 16)
        archive.writestr(info, data)
print(OUTPUT)
if published_code < module_code:
    print('Staged next version: publish the ZIP before updating update.json')
