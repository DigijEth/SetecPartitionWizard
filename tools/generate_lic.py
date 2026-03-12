#!/usr/bin/env python3
"""
Generate the .key unlock file for Setec Partition Wizard.

Usage:
  python generate_lic.py
  Then use the resulting unlock.key in Tools > Unlock Features
"""
import base64
import hashlib
import os

UNLOCK_TEXT = "Snake Says Unlock!"
EXPECTED_HASH = "f2cd6920ba4b09c79c105810f9eff9d73beb1f689b8f67099c1a39e5634059c5"

script_dir = os.path.dirname(os.path.abspath(__file__))
key_path = os.path.join(script_dir, "unlock.key")

encoded = base64.b64encode(UNLOCK_TEXT.encode("utf-8"))

sha = hashlib.sha256(encoded + b"\n").hexdigest()
print(f"SHA-256: {sha}")
print(f"Expected: {EXPECTED_HASH}")

if sha == EXPECTED_HASH:
    print("Hash matches!")
else:
    print("WARNING: Hash mismatch.")

with open(key_path, "wb") as f:
    f.write(encoded + b"\n")

print(f"Written: {key_path} ({len(encoded) + 1} bytes)")
