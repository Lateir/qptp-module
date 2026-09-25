#!/usr/bin/env python3
"""Validate a release tag and publish its Magisk update manifest."""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ASSET = 'qptp-magisk.zip'


def properties(path):
    return dict(line.split('=', 1) for line in path.read_text(encoding='utf-8').splitlines()
                if line and not line.startswith('#'))


def main(command, tag):
    if not re.fullmatch(r'v[0-9]+\.[0-9]+(?:\.[0-9]+)?', tag):
        raise SystemExit(f'Invalid release tag: {tag}')
    prop = properties(ROOT / 'module.prop')
    manifest_path = ROOT / 'update.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    if command == 'validate':
        if prop['version'] != tag:
            raise SystemExit('Tag and module.prop version differ')
        if int(prop['versionCode']) <= int(manifest['versionCode']):
            raise SystemExit('Release versionCode must exceed the published versionCode')
        return
    if command != 'update':
        raise SystemExit('Usage: release.py validate|update TAG')
    if int(prop['versionCode']) <= int(manifest['versionCode']):
        raise SystemExit('The released versionCode is no longer newer than update.json')
    if prop['version'] != tag:
        raise SystemExit('main/module.prop must still match the release tag')
    manifest['version'] = tag
    manifest['versionCode'] = int(prop['versionCode'])
    manifest['zipUrl'] = f'https://github.com/Lateir/qptp-module/releases/download/{tag}/{ASSET}'
    manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('Usage: release.py validate|update TAG')
    main(sys.argv[1], sys.argv[2])
