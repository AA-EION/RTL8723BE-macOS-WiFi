#!/usr/bin/env python3
"""Prepare a boot for manual post-login testing, without enabling EFI injection."""
import argparse
import copy
import datetime
import os
from pathlib import Path
import plistlib

APPLE_GUID = '7C436110-AB2A-4BBB-A880-FE41995C9F82'
FLAG = 'rtl8723be_experimental=1'


def configure(original, enable):
    result = copy.deepcopy(original)
    found = 0
    for section in ('Add', 'Force'):
        for entry in result.get('Kernel', {}).get(section, []):
            name = entry.get('BundlePath', '').replace('\\', '/').split('/')[-1]
            if name.lower() == 'rtl8723bewifi.kext':
                entry['Enabled'] = False
                found += 1
    if not found:
        raise ValueError('No RTL8723BEWiFi.kext entry found; refusing an unrelated config')
    nvram = result['NVRAM']['Add'][APPLE_GUID]
    args = nvram.get('boot-args', '')
    if not isinstance(args, str):
        raise ValueError('boot-args must be a string')
    args = [arg for arg in args.split() if not arg.startswith('rtl8723be_experimental=')]
    if enable:
        args.append(FLAG)
    nvram['boot-args'] = ' '.join(args)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, required=True, help='Explicit mounted EFI/OC/config.plist path')
    parser.add_argument('--disable', action='store_true', help='Remove experimental boot flag; keep driver disabled')
    parser.add_argument('--apply', action='store_true', help='Back up and write; otherwise preview only')
    opts = parser.parse_args()
    target = opts.config.resolve(strict=True)
    if target.name != 'config.plist' or target.parent.name != 'OC':
        parser.error('Expected an explicit EFI/OC/config.plist')
    before = target.read_bytes()
    config = plistlib.loads(before)
    updated = configure(config, not opts.disable)
    print('Target:', target)
    print('RTL8723BE EFI injection: DISABLED (manual load after login only)')
    print('Experimental boot flag:', 'removed' if opts.disable else 'enabled for next boot')
    if not opts.apply:
        print('Preview only. Add --apply to write after a successful normal internal-disk boot.')
        return
    if updated == config:
        print('Already configured; no write needed.')
        return
    suffix = datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    backup = target.with_name('config.before-rtl-test-' + suffix + '.plist')
    with backup.open('xb') as f:
        f.write(before)
        f.flush()
        os.fsync(f.fileno())
    payload = plistlib.dumps(updated, sort_keys=False)
    assert plistlib.loads(payload) == updated
    if target.read_bytes() != before:
        raise RuntimeError('Config changed during preparation; refusing to overwrite')
    temporary = target.with_name('config.rtl-test-' + suffix + '.tmp')
    with temporary.open('xb') as f:
        f.write(payload)
        f.flush()
        os.fsync(f.fileno())
    os.replace(temporary, target)
    assert plistlib.loads(target.read_bytes()) == updated
    print('Saved. Backup:', backup)
    print('Reboot to apply boot arguments. No driver was loaded or installed.')


if __name__ == '__main__':
    main()
