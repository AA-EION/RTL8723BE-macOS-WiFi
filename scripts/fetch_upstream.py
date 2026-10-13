#!/usr/bin/env python3
"""Cache the exact source references used by this port, verifying every hash."""
# SPDX-License-Identifier: MIT
import concurrent.futures
import hashlib
import json
from pathlib import Path
import urllib.request

ROOT = Path(__file__).resolve().parent.parent


def fetch(reference):
    relative = Path(reference["project"]) / reference["path"]
    if relative.is_absolute() or ".." in relative.parts:
        raise ValueError(f"Unsafe reference path: {relative}")
    destination = ROOT / ".references" / relative
    expected = reference["sha256"]
    if destination.is_file() and hashlib.sha256(destination.read_bytes()).hexdigest() == expected:
        return f"Cached: {relative}"
    with urllib.request.urlopen(reference["url"], timeout=30) as response:
        contents = response.read()
    actual = hashlib.sha256(contents).hexdigest()
    if actual != expected:
        raise ValueError(f"Source changed: {relative}; expected {expected}, got {actual}. "
                         "Resolve the recorded upstream revision before using this file.")
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(contents)
    return f"Verified: {relative}"


def main():
    references = json.loads((ROOT / "docs/upstream-sources.json").read_text())
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        for result in pool.map(fetch, references):
            print(result)
    print(f"Verified {len(references)} upstream source files in {ROOT / '.references'}")


if __name__ == "__main__":
    main()
