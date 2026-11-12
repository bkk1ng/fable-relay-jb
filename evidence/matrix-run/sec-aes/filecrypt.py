#!/usr/bin/env python3
"""
filecrypt.py — AES-256-GCM streaming file encryptor / decryptor

Requires: cryptography >= 41.0  (pip install cryptography)

File format
-----------
Offset  Size  Field
     0     6  Magic: b"FCRYPT"
     6     2  Version: b"\\x01\\x00"  (major=1, minor=0)
     8    32  Salt — random bytes fed to scrypt
    40    12  Base nonce — random, per-file; each chunk derives its own nonce from this
    52     ∞  Encrypted chunks (see below)

Each chunk on disk
------------------
  [4 bytes big-endian uint32]  length of the ciphertext+tag block that follows
  [N bytes]                    AES-256-GCM ciphertext + 16-byte authentication tag

Chunk nonce derivation
----------------------
  nonce_i = (int.from_bytes(base_nonce, "big") XOR i).to_bytes(12, "big")

  Counter wrapping is not a concern: a 96-bit nonce space supports 2^96 chunks per file,
  which exceeds any realistic file size.

Chunk AAD (additional authenticated data)
-----------------------------------------
  HEADER_MAGIC (8) || salt (32) || base_nonce (12) || chunk_index (8 bytes, big-endian)

  Binding every chunk to the full header and to its position prevents:
    * chunk substitution  — different salt/nonce would invalidate AAD
    * chunk reordering    — index is part of the authenticated data
    * truncation attacks  — a missing final chunk will be noticed by the caller

Key derivation
--------------
  scrypt(password, salt, n=2**17, r=8, p=1, dkLen=32)

  For --keyfile the raw file bytes are used as the password input.
  For --passphrase / interactive prompt the UTF-8-encoded string is used.
"""

import argparse
import os
import struct
import sys
from getpass import getpass
from pathlib import Path

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.scrypt import Scrypt


# ── constants ──────────────────────────────────────────────────────────────────

MAGIC        = b"FCRYPT"
VERSION      = b"\x01\x00"
HEADER_MAGIC = MAGIC + VERSION  # 8 bytes total

SALT_LEN     = 32               # bytes
NONCE_LEN    = 12               # bytes (AES-GCM standard)
KEY_LEN      = 32               # bytes (AES-256)
TAG_LEN      = 16               # bytes (AES-GCM tag appended by cryptography lib)
CHUNK_SIZE   = 64 * 1024        # 64 KiB plaintext per chunk

SCRYPT_N     = 2 ** 17          # CPU/memory cost (tune down to 2**14 for faster tests)
SCRYPT_R     = 8
SCRYPT_P     = 1


# ── header helpers ─────────────────────────────────────────────────────────────

def _write_header(fout, salt: bytes, base_nonce: bytes) -> None:
    fout.write(HEADER_MAGIC)
    fout.write(salt)
    fout.write(base_nonce)


def _read_header(fin) -> tuple[bytes, bytes]:
    """Read and validate the file header.  Returns (salt, base_nonce)."""
    raw = fin.read(len(HEADER_MAGIC))
    if len(raw) < len(HEADER_MAGIC):
        raise ValueError("File is too short to be a filecrypt archive")
    if raw[:6] != MAGIC:
        raise ValueError("Not a filecrypt file (bad magic bytes)")
    if raw[6:8] != VERSION:
        raise ValueError(f"Unsupported file version {raw[6]}.{raw[7]}")

    salt = fin.read(SALT_LEN)
    if len(salt) != SALT_LEN:
        raise ValueError("Truncated header: missing salt")

    base_nonce = fin.read(NONCE_LEN)
    if len(base_nonce) != NONCE_LEN:
        raise ValueError("Truncated header: missing base nonce")

    return salt, base_nonce


# ── key derivation ─────────────────────────────────────────────────────────────

def _derive_key(password: bytes, salt: bytes) -> bytes:
    kdf = Scrypt(salt=salt, length=KEY_LEN, n=SCRYPT_N, r=SCRYPT_R, p=SCRYPT_P)
    return kdf.derive(password)


# ── per-chunk nonce and AAD ────────────────────────────────────────────────────

def _chunk_nonce(base_nonce: bytes, index: int) -> bytes:
    return (int.from_bytes(base_nonce, "big") ^ index).to_bytes(NONCE_LEN, "big")


def _chunk_aad(salt: bytes, base_nonce: bytes, index: int) -> bytes:
    # Authenticates: file identity (magic+version), KDF salt, base nonce, and
    # chunk position.  Any mismatch causes GCM tag verification to fail.
    return HEADER_MAGIC + salt + base_nonce + index.to_bytes(8, "big")


# ── streaming encrypt / decrypt ────────────────────────────────────────────────

def encrypt_stream(fin, fout, password: bytes) -> None:
    """Read plaintext from *fin*, write encrypted archive to *fout*."""
    salt       = os.urandom(SALT_LEN)
    base_nonce = os.urandom(NONCE_LEN)
    key        = _derive_key(password, salt)
    aesgcm     = AESGCM(key)

    _write_header(fout, salt, base_nonce)

    index = 0
    while True:
        plaintext = fin.read(CHUNK_SIZE)
        if not plaintext:
            break
        nonce      = _chunk_nonce(base_nonce, index)
        aad        = _chunk_aad(salt, base_nonce, index)
        ciphertext = aesgcm.encrypt(nonce, plaintext, aad)  # ciphertext + tag
        fout.write(struct.pack(">I", len(ciphertext)))
        fout.write(ciphertext)
        index += 1


def decrypt_stream(fin, fout, password: bytes) -> None:
    """Read encrypted archive from *fin*, write plaintext to *fout*."""
    salt, base_nonce = _read_header(fin)
    key              = _derive_key(password, salt)
    aesgcm           = AESGCM(key)

    index = 0
    while True:
        length_field = fin.read(4)
        if not length_field:
            break                                   # clean EOF
        if len(length_field) != 4:
            raise ValueError(f"Chunk {index}: truncated length field")

        (chunk_len,) = struct.unpack(">I", length_field)
        if chunk_len < TAG_LEN:
            raise ValueError(
                f"Chunk {index}: implausible length {chunk_len} "
                f"(minimum is {TAG_LEN} for the GCM tag alone)"
            )

        ciphertext = fin.read(chunk_len)
        if len(ciphertext) != chunk_len:
            raise ValueError(f"Chunk {index}: expected {chunk_len} bytes, got {len(ciphertext)}")

        nonce = _chunk_nonce(base_nonce, index)
        aad   = _chunk_aad(salt, base_nonce, index)
        try:
            plaintext = aesgcm.decrypt(nonce, ciphertext, aad)
        except InvalidTag:
            raise ValueError(
                f"Chunk {index}: authentication tag mismatch — "
                "wrong passphrase / keyfile, or the file is corrupted / tampered"
            )

        fout.write(plaintext)
        index += 1


# ── password / keyfile resolution ─────────────────────────────────────────────

def _get_password(args, action: str) -> bytes:
    """Return the raw password bytes from --keyfile, --passphrase, or a prompt."""
    if args.keyfile:
        keyfile = Path(args.keyfile)
        if not keyfile.is_file():
            _die(f"keyfile not found: {args.keyfile}")
        return keyfile.read_bytes()

    if args.passphrase is not None:
        return args.passphrase.encode()

    # Interactive prompt — always confirm on encrypt to catch typos.
    pw = getpass(f"Passphrase ({action}): ")
    if not pw:
        _die("passphrase must not be empty")
    if action == "encrypt":
        pw2 = getpass("Confirm passphrase: ")
        if pw != pw2:
            _die("passphrases do not match")
    return pw.encode()


# ── output-file guard ──────────────────────────────────────────────────────────

def _open_output(path: str, force: bool):
    p = Path(path)
    if p.exists() and not force:
        _die(
            f"output file already exists: {path}\n"
            "       Use --force / -f to overwrite."
        )
    return p.open("wb")


# ── subcommand handlers ────────────────────────────────────────────────────────

def cmd_encrypt(args) -> None:
    input_path = Path(args.input)
    if not input_path.is_file():
        _die(f"input file not found: {args.input}")

    password = _get_password(args, "encrypt")
    fout     = _open_output(args.output, args.force)
    try:
        with input_path.open("rb") as fin:
            encrypt_stream(fin, fout, password)
        fout.close()
    except Exception:
        fout.close()
        Path(args.output).unlink(missing_ok=True)
        raise

    print(f"encrypted: {args.input} → {args.output}")


def cmd_decrypt(args) -> None:
    input_path = Path(args.input)
    if not input_path.is_file():
        _die(f"input file not found: {args.input}")

    password = _get_password(args, "decrypt")
    fout     = _open_output(args.output, args.force)
    try:
        with input_path.open("rb") as fin:
            decrypt_stream(fin, fout, password)
        fout.close()
    except (ValueError, OSError) as exc:
        # Remove partial plaintext — leaking it would be worse than losing it.
        fout.close()
        Path(args.output).unlink(missing_ok=True)
        _die(str(exc))

    print(f"decrypted: {args.input} → {args.output}")


# ── CLI ────────────────────────────────────────────────────────────────────────

def _die(msg: str) -> None:
    print(f"filecrypt: error: {msg}", file=sys.stderr)
    sys.exit(1)


def _add_common_args(parser: argparse.ArgumentParser) -> None:
    pw_group = parser.add_mutually_exclusive_group()
    pw_group.add_argument(
        "-p", "--passphrase",
        metavar="PASSPHRASE",
        help="passphrase as a CLI argument (omit to be prompted interactively)",
    )
    pw_group.add_argument(
        "-k", "--keyfile",
        metavar="FILE",
        help="file whose raw bytes are used as the password input to scrypt",
    )
    parser.add_argument(
        "-f", "--force",
        action="store_true",
        help="overwrite the output file if it already exists",
    )
    parser.add_argument("input",  help="path to the input file")
    parser.add_argument("output", help="path to the output file")


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="filecrypt",
        description=(
            "AES-256-GCM streaming file encryptor / decryptor.\n"
            "Key derivation: scrypt(n=2**17, r=8, p=1).  Chunk size: 64 KiB."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "examples:\n"
            "  filecrypt encrypt secret.txt secret.txt.enc\n"
            "  filecrypt decrypt secret.txt.enc secret.txt\n"
            "  filecrypt enc -p hunter2 plain.bin plain.bin.enc\n"
            "  filecrypt enc -k my.key  plain.bin plain.bin.enc\n"
            "  filecrypt dec -f -p hunter2 plain.bin.enc plain.bin\n"
        ),
    )
    sub = parser.add_subparsers(dest="command", required=True)

    enc = sub.add_parser(
        "encrypt", aliases=["enc", "e"],
        help="encrypt a file",
    )
    _add_common_args(enc)
    enc.set_defaults(func=cmd_encrypt)

    dec = sub.add_parser(
        "decrypt", aliases=["dec", "d"],
        help="decrypt a filecrypt archive",
    )
    _add_common_args(dec)
    dec.set_defaults(func=cmd_decrypt)

    return parser


def main() -> None:
    parser = _build_parser()
    args   = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
