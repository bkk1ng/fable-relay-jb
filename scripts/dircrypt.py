#!/usr/bin/env python3
"""
dircrypt.py — AES-256-CTR directory encryptor/decryptor
language: Python 3.10+, file: dircrypt.py
dep: cryptography>=42

File format (encrypted):
  magic    4 B  b'DCR1'
  salt    32 B  random per-file, scrypt input
  nonce   16 B  random per-file, CTR start
  mac     32 B  HMAC-SHA256(salt || nonce || ciphertext)  — keyed with mac_key
  ct       N B  AES-256-CTR(plaintext)                    — keyed with enc_key

Key derivation: scrypt(password, salt) → 64 bytes
  enc_key = bytes[0:32]   AES-256
  mac_key = bytes[32:64]  HMAC-SHA256
"""

import argparse
import hmac as _hmac
import os
import sys
from pathlib import Path

from cryptography.hazmat.primitives import hashes, hmac
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.kdf.scrypt import Scrypt

MAGIC = b"DCR1"
SALT_LEN = 32
NONCE_LEN = 16   # AES block size — CTR counter starts here
MAC_LEN = 32
HEADER_LEN = len(MAGIC) + SALT_LEN + NONCE_LEN + MAC_LEN  # 84 bytes

SCRYPT_N = 1 << 20  # 2^20 — tune down to 1<<14 on low-memory systems
SCRYPT_R = 8
SCRYPT_P = 1


def derive_keys(password: bytes, salt: bytes) -> tuple[bytes, bytes]:
    """Return (enc_key, mac_key) from scrypt(password, salt)."""
    kdf = Scrypt(salt=salt, length=64, n=SCRYPT_N, r=SCRYPT_R, p=SCRYPT_P)
    material = kdf.derive(password)
    return material[:32], material[32:]


def compute_mac(mac_key: bytes, data: bytes) -> bytes:
    h = hmac.HMAC(mac_key, hashes.SHA256())
    h.update(data)
    return h.finalize()


def aes_ctr(enc_key: bytes, nonce: bytes, data: bytes) -> bytes:
    cipher = Cipher(algorithms.AES(enc_key), modes.CTR(nonce))
    op = cipher.encryptor()          # CTR encrypt == decrypt
    return op.update(data) + op.finalize()


def encrypt_file(src: Path, dst: Path, password: bytes) -> None:
    plaintext = src.read_bytes()
    salt = os.urandom(SALT_LEN)
    nonce = os.urandom(NONCE_LEN)
    enc_key, mac_key = derive_keys(password, salt)
    ct = aes_ctr(enc_key, nonce, plaintext)
    mac = compute_mac(mac_key, salt + nonce + ct)   # encrypt-then-MAC
    dst.parent.mkdir(parents=True, exist_ok=True)
    part = dst.with_suffix(dst.suffix + ".part")
    part.write_bytes(MAGIC + salt + nonce + mac + ct)
    part.replace(dst)   # atomic on POSIX


def decrypt_file(src: Path, dst: Path, password: bytes) -> None:
    raw = src.read_bytes()
    if len(raw) < HEADER_LEN:
        raise ValueError("file too short to be DCR1")
    if raw[:4] != MAGIC:
        raise ValueError("not a DCR1 file")
    salt = raw[4:36]
    nonce = raw[36:52]
    stored_mac = raw[52:84]
    ct = raw[84:]
    enc_key, mac_key = derive_keys(password, salt)
    expected_mac = compute_mac(mac_key, salt + nonce + ct)
    if not _hmac.compare_digest(stored_mac, expected_mac):
        raise ValueError("HMAC mismatch — wrong password or corrupted file")
    plaintext = aes_ctr(enc_key, nonce, ct)
    dst.parent.mkdir(parents=True, exist_ok=True)
    part = dst.with_suffix(dst.suffix + ".part")
    part.write_bytes(plaintext)
    part.replace(dst)


def walk(src_root: Path, dst_root: Path, password: bytes, mode: str) -> None:
    fn = encrypt_file if mode == "encrypt" else decrypt_file
    errors: list[Path] = []

    for dirpath, dirnames, filenames in os.walk(src_root):
        cur = Path(dirpath)
        # skip symlinked subdirectories in-place
        dirnames[:] = [d for d in dirnames if not (cur / d).is_symlink()]

        for name in filenames:
            src = cur / name
            if src.is_symlink():
                continue
            rel = src.relative_to(src_root)
            dst = dst_root / rel
            try:
                fn(src, dst, password)
                print(f"  {mode}ed  {rel}")
            except Exception as exc:
                print(f"  ERROR    {rel}: {exc}", file=sys.stderr)
                errors.append(rel)

    if errors:
        print(f"\n{len(errors)} file(s) failed.", file=sys.stderr)
        sys.exit(1)


def get_password(confirm: bool) -> bytes:
    import getpass
    pw = getpass.getpass("Password: ")
    if confirm:
        pw2 = getpass.getpass("Confirm: ")
        if pw != pw2:
            sys.exit("Passwords do not match.")
    if not pw:
        sys.exit("Password must not be empty.")
    return pw.encode()


def main() -> None:
    ap = argparse.ArgumentParser(
        description="AES-256-CTR directory encrypt/decrypt — scrypt KDF, HMAC-SHA256 auth"
    )
    ap.add_argument("mode", choices=["encrypt", "decrypt"])
    ap.add_argument("src", help="source directory")
    ap.add_argument("dst", help="destination directory (mirrored tree, created if absent)")
    args = ap.parse_args()

    src_root = Path(args.src).resolve()
    dst_root = Path(args.dst).resolve()

    if not src_root.is_dir():
        sys.exit(f"not a directory: {src_root}")
    if dst_root == src_root:
        sys.exit("src and dst must differ")

    password = get_password(confirm=(args.mode == "encrypt"))
    walk(src_root, dst_root, password, args.mode)
    print("done.")


if __name__ == "__main__":
    main()
