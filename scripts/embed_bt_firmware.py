#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate firmware storage; the checked-in binary is redistributed unmodified."""
import hashlib
from pathlib import Path
import sys

source, destination = map(Path, sys.argv[1:3])
data = source.read_bytes()
expected = 'c9c8a9f6ebd90b7449a7e4d38ee39952dc7cc47d6c1d054e42cfbb5e4d818853'
if hashlib.sha256(data).hexdigest() != expected:
    raise SystemExit('Unexpected RTL8723B firmware digest; review provenance before updating.')
lines = ['// Firmware: LicenseRef-rtl-bt-firmware; generated without modification.',
         '#include "firmware.hpp"', 'const uint8_t rtl8723b_bt_firmware[] = {']
lines += [','.join(f'0x{x:02x}' for x in data[i:i + 16]) + ',' for i in range(0, len(data), 16)]
lines += ['};', 'const size_t rtl8723b_bt_firmware_size = sizeof(rtl8723b_bt_firmware);']
destination.parent.mkdir(parents=True, exist_ok=True)
destination.write_text('\n'.join(lines) + '\n')
