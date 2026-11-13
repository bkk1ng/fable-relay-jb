#!/usr/bin/env python3
"""
filecrypt.py — AES-256-GCM streaming file encryptor / decryptor

Requires: cryptography >= 41.0
Install:  pip install cryptography

Usage:
  python filecrypt.py encrypt [-k KEYFILE | -p] [-f] INPUT OUTPUT
  python filecrypt.py decrypt [-k KEYFILE | -p] [-f] INPUT OUTPUT

If neither -k nor -p is given, passphrase mode is assumed.

File format on disk:
  MAGIC       8 bytes   b'FILCRYPT'
  VERSION     1 byte    0x01
  SALT        32 bytes  random, for scrypt key derivation
  BASE_NONCE  12 bytes  random, used to derive per-chunk nonces
  CHUNK...    variable  one or more chunks (see below)

Each chunk:
  CHUNK_LEN   4 bytes   uint32 big-endian, length of ciphertext+tag
  CT+TAG      variable  AES-256-GCM ciphertext with 16-byte tag appended

Per-chunk nonce:
  BASE_NONCE XOR chunk_index — ensures nonce uniqueness across all chunks
  that share the same key while still fitting in 12 bytes.

Per-chunk AAD:
  MAGIC || VERSION || SALT || BASE_NONCE || chunk_index (uint64 BE) || is_last (1 byte)

  Binding the header fields prevents a ciphertext block from being transplanted
  between files.  Binding the sequence number prevents chunk reordering.
  Binding is_last prevents silent truncation: the final chunk is authenticated
  as the final chunk, so stripping trailing chunks breaks verification.
"""

from __future__ import annotations

import argparse
import getpass
import os
import struct
import sys
from pathlib import Path

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.backends import default_backend
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.scrypt import Scrypt

# ── format constants ────────────────────────────────────────────────────────

MAGIC = b"FILCRYPT"       # 8 bytes
VERSION = b"\x01"         # 1 byte
SALT_LEN = 32             # bytes
NONCE_LEN = 12            # bytes (AES-GCM standard)
TAG_LEN = 16              # bytes (GCM tag, appended by AESGCM.encrypt)
CHUNK_SIZE = 65_536       # 64 KiB plaintext per chunk

# scrypt work factors — intentionally expensive for file encryption
SCRYPT_N = 1 << 17        # 131 072 — increase to 1 << 18 for more margin
SCRYPT_R = 8
SCRYPT_P = 1
KEY_LEN = 32              # 256-bit key


# ── cryptographic helpers ────────────────────────────────────────────────────

def _derive_key(secret: bytes, salt: bytes) -> bytes:
    """Derive a 256-bit key from raw secret bytes and a random salt via scrypt."""
    kdf = Scrypt(
        salt=salt,
        length=KEY_LEN,
        n=SCRYPT_N,
        r=SCRYPT_R,
        p=SCRYPT_P,
        backend=default_backend(),
    )
    return kdf.derive(secret)


def _chunk_nonce(base_nonce: bytes, index: int) -> bytes:
    """Return a 12-byte nonce unique to this chunk by XOR-ing the counter in."""
    nonce_int = int.from_bytes(base_nonce, "big")
    nonce_int ^= index & ((1 << 96) - 1)
    return nonce_int.to_bytes(NONCE_LEN, "big")


def _chunk_aad(
    salt: bytes,
    base_nonce: bytes,
    index: int,
    is_last: bool,
) -> bytes:
    """Build additional authenticated data for one chunk.

    Binds the chunk to: file identity (magic+version), key derivation parameters
    (salt), the specific base nonce, the chunk's sequence position, and whether
    it is the final chunk.
    """
    return (
        MAGIC
        + VERSION
        + salt
        + base_nonce
        + struct.pack(">Q", index)
        + (b"\x01" if is_last else b"\x00")
    )


# ── key-material acquisition ─────────────────────────────────────────────────

def _get_secret(args: argparse.Namespace) -> bytes:
    """Return raw bytes to feed into scrypt — from keyfile or passphrase prompt."""
    if args.keyfile:
        path = Path(args.keyfile)
        if not path.is_file():
            sys.exit(f"error: keyfile not found: {args.keyfile}")
        data = path.read_bytes()
        if not data:
            sys.exit("error: keyfile is empty")
        return data

    # Passphrase mode
    if args.subcommand == "encrypt":
        pw = getpass.getpass("Passphrase: ")
        if not pw:
            sys.exit("error: passphrase must not be empty")
        confirm = getpass.getpass("Confirm passphrase: ")
        if pw != confirm:
            sys.exit("error: passphrases do not match")
        return pw.encode("utf-8")

    pw = getpass.getpass("Passphrase: ")
    return pw.encode("utf-8")


# ── output-file guard ────────────────────────────────────────────────────────

def _guard_output(path: Path, force: bool) -> None:
    if path.exists() and not force:
        sys.exit(
            f"error: output file already exists: {path}\n"
            "       pass --force / -f to overwrite"
        )


# ── chunk I/O helpers ────────────────────────────────────────────────────────

def _write_chunk(fout, ciphertext: bytes) -> None:
    fout.write(struct.pack(">I", len(ciphertext)))
    fout.write(ciphertext)


def _read_chunk(fin):
    """Read the next chunk from fin.  Returns bytes, or None at EOF."""
    raw_len = fin.read(4)
    if not raw_len:
        return None
    if len(raw_len) < 4:
        sys.exit("error: truncated chunk-length field — file is corrupt")
    ct_len = struct.unpack(">I", raw_len)[0]
    ct = fin.read(ct_len)
    if len(ct) < ct_len:
        sys.exit("error: truncated chunk data — file is corrupt")
    return ct


# ── subcommands ──────────────────────────────────────────────────────────────

def cmd_encrypt(args: argparse.Namespace) -> None:
    input_path = Path(args.input)
    output_path = Path(args.output)

    if not input_path.is_file():
        sys.exit(f"error: input file not found: {input_path}")
    _guard_output(output_path, args.force)

    secret = _get_secret(args)
    salt = os.urandom(SALT_LEN)
    base_nonce = os.urandom(NONCE_LEN)

    print("Deriving key (this may take a moment)…", file=sys.stderr)
    key = _derive_key(secret, salt)
    aes = AESGCM(key)

    with (
        input_path.open("rb") as fin,
        output_path.open("wb") as fout,
    ):
        # Write file header
        fout.write(MAGIC)
        fout.write(VERSION)
        fout.write(salt)
        fout.write(base_nonce)

        # Stream chunks with a one-chunk lookahead so we can mark the last one
        chunk_index = 0
        current = fin.read(CHUNK_SIZE)

        while current:
            lookahead = fin.read(CHUNK_SIZE)
            is_last = len(lookahead) == 0

            nonce = _chunk_nonce(base_nonce, chunk_index)
            aad = _chunk_aad(salt, base_nonce, chunk_index, is_last)
            ct = aes.encrypt(nonce, current, aad)
            _write_chunk(fout, ct)

            chunk_index += 1
            current = lookahead

        # Empty input: write a single empty last chunk so the format is valid
        if chunk_index == 0:
            nonce = _chunk_nonce(base_nonce, 0)
            aad = _chunk_aad(salt, base_nonce, 0, True)
            ct = aes.encrypt(nonce, b"", aad)
            _write_chunk(fout, ct)

    print(f"Encrypted  {input_path}  →  {output_path}", file=sys.stderr)


def cmd_decrypt(args: argparse.Namespace) -> None:
    input_path = Path(args.input)
    output_path = Path(args.output)

    if not input_path.is_file():
        sys.exit(f"error: input file not found: {input_path}")
    _guard_output(output_path, args.force)

    secret = _get_secret(args)

    # Write plaintext to a sibling temp file so a failed authentication
    # never leaves partial plaintext at the final destination.
    tmp_path = output_path.with_name(output_path.name + ".fctmp")
    if tmp_path.exists():
        tmp_path.unlink()

    try:
        with input_path.open("rb") as fin:
            # ── Validate header ──────────────────────────────────────────────
            magic = fin.read(len(MAGIC))
            if magic != MAGIC:
                sys.exit("error: not a filecrypt file (bad magic bytes)")

            version = fin.read(1)
            if version != VERSION:
                sys.exit(f"error: unsupported file version: {version.hex()}")

            salt = fin.read(SALT_LEN)
            if len(salt) != SALT_LEN:
                sys.exit("error: truncated header — salt field is incomplete")

            base_nonce = fin.read(NONCE_LEN)
            if len(base_nonce) != NONCE_LEN:
                sys.exit("error: truncated header — nonce field is incomplete")

            # ── Derive key ───────────────────────────────────────────────────
            print("Deriving key (this may take a moment)…", file=sys.stderr)
            key = _derive_key(secret, salt)
            aes = AESGCM(key)

            # ── Streaming decrypt with one-chunk lookahead ───────────────────
            with tmp_path.open("wb") as fout:
                chunk_index = 0
                current = _read_chunk(fin)
                if current is None:
                    sys.exit("error: file contains no chunks — it may be corrupt")

                while current is not None:
                    lookahead = _read_chunk(fin)
                    is_last = lookahead is None

                    nonce = _chunk_nonce(base_nonce, chunk_index)
                    aad = _chunk_aad(salt, base_nonce, chunk_index, is_last)
                    try:
                        plaintext = aes.decrypt(nonce, current, aad)
                    except InvalidTag:
                        sys.exit(
                            f"error: authentication failed on chunk {chunk_index} — "
                            "wrong passphrase / keyfile, or file is corrupted / tampered"
                        )
                    fout.write(plaintext)

                    chunk_index += 1
                    current = lookahead

        # Atomic replace on success
        tmp_path.replace(output_path)

    except SystemExit:
        tmp_path.unlink(missing_ok=True)
        raise
    except BaseException:
        tmp_path.unlink(missing_ok=True)
        raise

    print(f"Decrypted  {input_path}  →  {output_path}", file=sys.stderr)


# ── argument parser ──────────────────────────────────────────────────────────

def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="filecrypt",
        description="AES-256-GCM streaming file encryptor / decryptor.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "examples:\n"
            "  python filecrypt.py encrypt secret.txt secret.enc\n"
            "  python filecrypt.py decrypt secret.enc recovered.txt\n"
            "  python filecrypt.py encrypt -k mykey.bin data.zip data.zip.enc\n"
            "  python filecrypt.py decrypt -k mykey.bin -f data.zip.enc data.zip\n"
        ),
    )

    sub = parser.add_subparsers(dest="subcommand", required=True)

    for name, help_str in (
        ("encrypt", "encrypt INPUT and write to OUTPUT"),
        ("decrypt", "decrypt INPUT and write to OUTPUT"),
    ):
        p = sub.add_parser(name, help=help_str)

        key_src = p.add_mutually_exclusive_group()
        key_src.add_argument(
            "-p", "--passphrase",
            action="store_true",
            default=False,
            help="derive key from an interactively prompted passphrase (default)",
        )
        key_src.add_argument(
            "-k", "--keyfile",
            metavar="FILE",
            help="derive key from the raw bytes of FILE instead of a passphrase",
        )

        p.add_argument(
            "-f", "--force",
            action="store_true",
            default=False,
            help="overwrite OUTPUT if it already exists",
        )
        p.add_argument("input", help="source file path")
        p.add_argument("output", help="destination file path")

    return parser


# ── entry point ──────────────────────────────────────────────────────────────

def main() -> None:
    parser = build_parser()
    args = parser.parse_args()

    # Default to passphrase when neither -p nor -k was supplied
    if not args.keyfile:
        args.passphrase = True

    if args.subcommand == "encrypt":
        cmd_encrypt(args)
    else:
        cmd_decrypt(args)


if __name__ == "__main__":
    main()
