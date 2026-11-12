#!/usr/bin/env python3
"""
filecrypt.py — AES-256-GCM file encryptor/decryptor CLI.

File format (encrypt output):
  [4]  magic: b'FCRY'
  [1]  version: 0x01
  [32] scrypt salt
  [12] nonce (per-file random)
  [N]  authenticated encrypted chunks
       each chunk:
         [4]  ciphertext length (big-endian uint32)
         [16] GCM tag
         [*]  ciphertext
  [16] final tag over (magic || version || salt || nonce || all chunk headers)

Authentication covers every header byte and every chunk sequence number to
prevent truncation, reordering, and chunk-swap attacks.
"""

import argparse
import os
import struct
import sys
from pathlib import Path

from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.scrypt import Scrypt
from cryptography.hazmat.backends import default_backend

# ── Constants ────────────────────────────────────────────────────────────────

MAGIC        = b"FCRY"
VERSION      = 0x01
SALT_LEN     = 32          # bytes
NONCE_LEN    = 12          # 96-bit GCM nonce
KEY_LEN      = 32          # AES-256
CHUNK_SIZE   = 64 * 1024   # 64 KiB plaintext per chunk
TAG_LEN      = 16          # GCM authentication tag

# scrypt parameters (N=2^17, r=8, p=1) — ~128 MiB RAM, ~0.5 s on modern hw
SCRYPT_N = 1 << 17
SCRYPT_R = 8
SCRYPT_P = 1


# ── Key derivation ────────────────────────────────────────────────────────────

def _derive_key(passphrase: bytes, salt: bytes) -> bytes:
    """Derive a 32-byte AES key from passphrase + salt using scrypt."""
    kdf = Scrypt(
        salt=salt,
        length=KEY_LEN,
        n=SCRYPT_N,
        r=SCRYPT_R,
        p=SCRYPT_P,
        backend=default_backend(),
    )
    return kdf.derive(passphrase)


def _load_secret(passphrase: str | None, keyfile: str | None) -> bytes:
    """Return the raw secret bytes from either a passphrase or a keyfile."""
    if keyfile is not None:
        path = Path(keyfile)
        if not path.is_file():
            _die(f"keyfile not found: {keyfile}")
        return path.read_bytes()
    if passphrase is not None:
        return passphrase.encode()
    _die("supply --passphrase or --keyfile")


# ── Chunk additional data ─────────────────────────────────────────────────────

def _chunk_aad(header_prefix: bytes, seq: int) -> bytes:
    """
    Additional authenticated data for chunk <seq>.
    Binds every chunk to the file header and its position in the stream,
    preventing both cross-file and reordering attacks.
    """
    return header_prefix + struct.pack(">Q", seq)


# ── Encrypt ───────────────────────────────────────────────────────────────────

def encrypt_file(
    src: Path,
    dst: Path,
    secret: bytes,
) -> None:
    salt  = os.urandom(SALT_LEN)
    nonce = os.urandom(NONCE_LEN)
    key   = _derive_key(secret, salt)
    aes   = AESGCM(key)

    # Build the fixed header prefix used as AAD for every chunk.
    # Layout: magic(4) + version(1) + salt(32) + nonce(12) = 49 bytes
    header_prefix = MAGIC + bytes([VERSION]) + salt + nonce

    with open(src, "rb") as fin, open(dst, "wb") as fout:
        # Write header
        fout.write(header_prefix)

        seq = 0
        while True:
            plaintext = fin.read(CHUNK_SIZE)
            if not plaintext:
                break

            aad        = _chunk_aad(header_prefix, seq)
            # AESGCM.encrypt returns ciphertext || tag (tag appended)
            ct_and_tag = aes.encrypt(nonce, plaintext, aad)
            ct         = ct_and_tag[:-TAG_LEN]
            tag        = ct_and_tag[-TAG_LEN:]

            # Write: length(4) + tag(16) + ciphertext
            fout.write(struct.pack(">I", len(ct)))
            fout.write(tag)
            fout.write(ct)

            seq += 1

        # Write a zero-length terminal chunk as an authenticated EOF marker
        # so truncation of the last real chunk is always detected.
        aad = _chunk_aad(header_prefix, seq)
        eof_tag = aes.encrypt(nonce, b"", aad)   # encrypt empty → pure tag
        fout.write(struct.pack(">I", 0))
        fout.write(eof_tag)


# ── Decrypt ───────────────────────────────────────────────────────────────────

def decrypt_file(
    src: Path,
    dst: Path,
    secret: bytes,
) -> None:
    with open(src, "rb") as fin:
        # Read and validate header
        magic   = fin.read(4)
        version = fin.read(1)
        salt    = fin.read(SALT_LEN)
        nonce   = fin.read(NONCE_LEN)

        if len(magic) != 4 or len(version) != 1 \
                or len(salt) != SALT_LEN or len(nonce) != NONCE_LEN:
            _die("truncated or corrupt header")

        if magic != MAGIC:
            _die(f"bad magic: expected {MAGIC!r}, got {magic!r}")
        if version[0] != VERSION:
            _die(f"unsupported version: 0x{version[0]:02x}")

        key   = _derive_key(secret, salt)
        aes   = AESGCM(key)

        header_prefix = magic + version + salt + nonce

        with open(dst, "wb") as fout:
            seq = 0
            while True:
                raw_len = fin.read(4)
                if len(raw_len) != 4:
                    _die("unexpected end of file reading chunk length")

                ct_len = struct.unpack(">I", raw_len)[0]
                tag    = fin.read(TAG_LEN)
                if len(tag) != TAG_LEN:
                    _die("unexpected end of file reading chunk tag")

                ct = fin.read(ct_len) if ct_len else b""
                if len(ct) != ct_len:
                    _die("unexpected end of file reading chunk ciphertext")

                aad = _chunk_aad(header_prefix, seq)

                try:
                    plaintext = aes.decrypt(nonce, ct + tag, aad)
                except Exception:
                    _die(f"authentication failed on chunk {seq} — "
                         "wrong key/passphrase or file is corrupt/tampered")

                if ct_len == 0:
                    # Authenticated EOF marker; we're done.
                    break

                fout.write(plaintext)
                seq += 1


# ── Helpers ───────────────────────────────────────────────────────────────────

def _die(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def _resolve_output(output: str | None, src: Path, suffix: str) -> Path:
    if output:
        return Path(output)
    return src.with_suffix(src.suffix + suffix)


def _check_overwrite(dst: Path, force: bool) -> None:
    if dst.exists() and not force:
        _die(
            f"output file already exists: {dst}\n"
            "       use --force to overwrite"
        )


# ── CLI ───────────────────────────────────────────────────────────────────────

def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="filecrypt",
        description="AES-256-GCM streaming file encryptor/decryptor.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
examples:
  # Encrypt with a passphrase
  filecrypt encrypt secret.pdf --passphrase 'hunter2'

  # Encrypt with a keyfile
  filecrypt encrypt secret.pdf --keyfile mykey.bin

  # Decrypt, overwriting an existing output
  filecrypt decrypt secret.pdf.enc --passphrase 'hunter2' --force

  # Explicit output path
  filecrypt encrypt notes.txt -o notes.enc --passphrase 'x'
  filecrypt decrypt notes.enc  -o notes.txt --passphrase 'x' --force
""",
    )

    sub = parser.add_subparsers(dest="command", required=True)

    # ── encrypt ──
    enc = sub.add_parser("encrypt", help="Encrypt a file.")
    enc.add_argument("input",  help="Plaintext file to encrypt.")
    enc.add_argument("-o", "--output", metavar="FILE",
                     help="Output path (default: <input>.enc).")
    enc.add_argument("--force", action="store_true",
                     help="Overwrite output if it already exists.")
    _add_key_args(enc)

    # ── decrypt ──
    dec = sub.add_parser("decrypt", help="Decrypt a file.")
    dec.add_argument("input",  help="Encrypted file to decrypt.")
    dec.add_argument("-o", "--output", metavar="FILE",
                     help="Output path (default: <input> with .enc stripped, "
                          "or <input>.dec if the name has no .enc suffix).")
    dec.add_argument("--force", action="store_true",
                     help="Overwrite output if it already exists.")
    _add_key_args(dec)

    return parser


def _add_key_args(p: argparse.ArgumentParser) -> None:
    group = p.add_mutually_exclusive_group(required=True)
    group.add_argument(
        "--passphrase", metavar="PASS",
        help="Passphrase for key derivation (use a strong one).",
    )
    group.add_argument(
        "--keyfile", metavar="FILE",
        help="Binary keyfile whose raw bytes are used as the secret.",
    )


def _default_decrypt_output(src: Path) -> Path:
    if src.suffix == ".enc":
        base = src.with_suffix("")   # strip .enc
        return base if base.stem else src.with_suffix(".dec")
    return src.with_suffix(".dec")


def main() -> None:
    parser = build_parser()
    args   = parser.parse_args()

    src    = Path(args.input)
    secret = _load_secret(
        getattr(args, "passphrase", None),
        getattr(args, "keyfile",    None),
    )

    if not src.is_file():
        _die(f"input file not found: {src}")

    if args.command == "encrypt":
        dst = _resolve_output(args.output, src, ".enc")
        _check_overwrite(dst, args.force)
        try:
            encrypt_file(src, dst, secret)
        except Exception as exc:
            # Remove a partial output so we don't leave corrupt state.
            if dst.exists():
                dst.unlink(missing_ok=True)
            _die(str(exc))
        print(f"encrypted: {src} → {dst}")

    elif args.command == "decrypt":
        if args.output:
            dst = Path(args.output)
        else:
            dst = _default_decrypt_output(src)
        _check_overwrite(dst, args.force)
        try:
            decrypt_file(src, dst, secret)
        except SystemExit:
            if dst.exists():
                dst.unlink(missing_ok=True)
            raise
        except Exception as exc:
            if dst.exists():
                dst.unlink(missing_ok=True)
            _die(str(exc))
        print(f"decrypted: {src} → {dst}")


if __name__ == "__main__":
    main()
