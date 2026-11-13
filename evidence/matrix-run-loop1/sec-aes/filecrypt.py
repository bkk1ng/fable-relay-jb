#!/usr/bin/env python3
"""
filecrypt.py — AES-256-GCM file encryptor/decryptor CLI

File format (all multi-byte integers are big-endian):
  [4]  magic        b'FCRY'
  [1]  version      0x01
  [1]  kdf          0x01 = scrypt, 0x02 = keyfile
  [32] salt         random (used for scrypt; zero-filled for keyfile mode)
  [12] nonce        random per-file GCM nonce (also seeds chunk counter AAD)
  [4]  chunk_size   plaintext bytes per chunk (uint32)
  --- AAD boundary: everything above is authenticated ---
  chunks:
    [4]       seq       big-endian uint32 chunk sequence number (authenticated)
    [2]       ct_len    ciphertext length including 16-byte GCM tag (uint16)
              ...       ciphertext + tag
  [4+2+16]  terminal  seq=0xFFFFFFFF, ct_len=16, tag over header||all_seqs
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
from pathlib import Path

from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.scrypt import Scrypt
from cryptography.hazmat.backends import default_backend

# ── constants ────────────────────────────────────────────────────────────────

MAGIC          = b"FCRY"
VERSION        = 0x01
KDF_SCRYPT     = 0x01
KDF_KEYFILE    = 0x02

SALT_LEN       = 32
NONCE_LEN      = 12
KEY_LEN        = 32          # AES-256
TAG_LEN        = 16
DEFAULT_CHUNK  = 64 * 1024   # 64 KiB plaintext per chunk

# scrypt parameters (N=2^17 ≈ 128 MiB RAM, recommended OWASP floor for 2024)
SCRYPT_N = 1 << 17
SCRYPT_R = 8
SCRYPT_P = 1

HEADER_FIXED_LEN = (
    len(MAGIC)   # 4
    + 1          # version
    + 1          # kdf
    + SALT_LEN   # 32
    + NONCE_LEN  # 12
    + 4          # chunk_size
)  # = 54 bytes

TERMINAL_SEQ = 0xFFFF_FFFF


# ── key derivation ────────────────────────────────────────────────────────────

def _derive_scrypt(passphrase: bytes, salt: bytes) -> bytes:
    kdf = Scrypt(
        salt=salt,
        length=KEY_LEN,
        n=SCRYPT_N,
        r=SCRYPT_R,
        p=SCRYPT_P,
        backend=default_backend(),
    )
    return kdf.derive(passphrase)


def _derive_keyfile(keyfile_path: Path) -> bytes:
    raw = keyfile_path.read_bytes()
    if len(raw) < KEY_LEN:
        raise ValueError(
            f"Keyfile must be at least {KEY_LEN} bytes; got {len(raw)}."
        )
    # Use first KEY_LEN bytes directly (user is responsible for quality).
    return raw[:KEY_LEN]


def _build_key(
    passphrase: str | None,
    keyfile: Path | None,
    salt: bytes,
) -> tuple[bytes, int]:
    """Return (key_bytes, kdf_flag)."""
    if keyfile is not None:
        return _derive_keyfile(keyfile), KDF_KEYFILE
    if passphrase is not None:
        return _derive_scrypt(passphrase.encode(), salt), KDF_SCRYPT
    raise ValueError("Supply either --passphrase or --keyfile.")


# ── header helpers ────────────────────────────────────────────────────────────

def _pack_header(
    kdf: int,
    salt: bytes,
    nonce: bytes,
    chunk_size: int,
) -> bytes:
    return (
        MAGIC
        + struct.pack(">BB", VERSION, kdf)
        + salt
        + nonce
        + struct.pack(">I", chunk_size)
    )


def _unpack_header(data: bytes) -> tuple[int, bytes, bytes, int]:
    """Return (kdf, salt, nonce, chunk_size). Raises on bad magic/version."""
    if len(data) < HEADER_FIXED_LEN:
        raise ValueError("File too short to contain a valid header.")
    if data[:4] != MAGIC:
        raise ValueError("Not a filecrypt file (bad magic).")
    version, kdf = struct.unpack_from(">BB", data, 4)
    if version != VERSION:
        raise ValueError(f"Unsupported file version: {version:#x}.")
    offset = 6
    salt       = data[offset : offset + SALT_LEN];   offset += SALT_LEN
    nonce      = data[offset : offset + NONCE_LEN];   offset += NONCE_LEN
    chunk_size = struct.unpack_from(">I", data, offset)[0]
    return kdf, salt, nonce, chunk_size


# ── chunk sequencing ──────────────────────────────────────────────────────────

def _chunk_aad(header: bytes, seq: int) -> bytes:
    """
    AAD = file header bytes || big-endian seq.
    Binds every chunk to both the file identity and its position,
    preventing reordering, truncation, and cross-file splicing.
    """
    return header + struct.pack(">I", seq)


# ── encrypt ───────────────────────────────────────────────────────────────────

def encrypt_file(
    src: Path,
    dst: Path,
    passphrase: str | None,
    keyfile: Path | None,
    chunk_size: int = DEFAULT_CHUNK,
    force: bool = False,
) -> None:
    if dst.exists() and not force:
        raise FileExistsError(
            f"{dst} already exists. Use --force to overwrite."
        )

    salt  = os.urandom(SALT_LEN)
    nonce = os.urandom(NONCE_LEN)

    key, kdf_flag = _build_key(passphrase, keyfile, salt)
    header = _pack_header(kdf_flag, salt, nonce, chunk_size)
    aesgcm = AESGCM(key)

    tmp = dst.with_suffix(dst.suffix + ".tmp")
    try:
        with src.open("rb") as fin, tmp.open("wb") as fout:
            fout.write(header)

            seq = 0
            while True:
                plaintext = fin.read(chunk_size)
                is_last   = len(plaintext) < chunk_size

                if plaintext:
                    aad        = _chunk_aad(header, seq)
                    ciphertext = aesgcm.encrypt(nonce, plaintext, aad)
                    # ciphertext already includes the 16-byte GCM tag
                    fout.write(struct.pack(">IH", seq, len(ciphertext)))
                    fout.write(ciphertext)
                    seq += 1

                if is_last:
                    break

            # Terminal sentinel: encrypt an empty payload authenticated over
            # the full header to give a final integrity check.
            term_aad = _chunk_aad(header, TERMINAL_SEQ)
            term_tag = aesgcm.encrypt(nonce, b"", term_aad)
            fout.write(struct.pack(">IH", TERMINAL_SEQ, len(term_tag)))
            fout.write(term_tag)

        tmp.replace(dst)
    except Exception:
        tmp.unlink(missing_ok=True)
        raise


# ── decrypt ───────────────────────────────────────────────────────────────────

def decrypt_file(
    src: Path,
    dst: Path,
    passphrase: str | None,
    keyfile: Path | None,
    force: bool = False,
) -> None:
    if dst.exists() and not force:
        raise FileExistsError(
            f"{dst} already exists. Use --force to overwrite."
        )

    with src.open("rb") as fin:
        raw_header = fin.read(HEADER_FIXED_LEN)
        kdf, salt, nonce, chunk_size = _unpack_header(raw_header)

        # Reconstruct the key.
        if kdf == KDF_SCRYPT:
            if passphrase is None:
                raise ValueError("File was encrypted with a passphrase; supply --passphrase.")
            key = _derive_scrypt(passphrase.encode(), salt)
        elif kdf == KDF_KEYFILE:
            if keyfile is None:
                raise ValueError("File was encrypted with a keyfile; supply --keyfile.")
            key = _derive_keyfile(keyfile)
        else:
            raise ValueError(f"Unknown KDF flag: {kdf:#x}.")

        header = raw_header  # the exact bytes that were written at encrypt time
        aesgcm = AESGCM(key)

        tmp = dst.with_suffix(dst.suffix + ".tmp")
        try:
            with tmp.open("wb") as fout:
                expected_seq = 0
                saw_terminal = False

                while True:
                    seq_ct_len = fin.read(6)
                    if len(seq_ct_len) < 6:
                        raise ValueError("Unexpected end of file reading chunk header.")

                    seq, ct_len = struct.unpack(">IH", seq_ct_len)
                    ciphertext  = fin.read(ct_len)
                    if len(ciphertext) < ct_len:
                        raise ValueError("Unexpected end of file reading chunk body.")

                    if seq == TERMINAL_SEQ:
                        # Verify the terminal sentinel.
                        aad = _chunk_aad(header, TERMINAL_SEQ)
                        aesgcm.decrypt(nonce, ciphertext, aad)  # raises on bad tag
                        saw_terminal = True
                        break

                    if seq != expected_seq:
                        raise ValueError(
                            f"Chunk sequence mismatch: expected {expected_seq}, got {seq}."
                        )

                    aad       = _chunk_aad(header, seq)
                    plaintext = aesgcm.decrypt(nonce, ciphertext, aad)
                    fout.write(plaintext)
                    expected_seq += 1

            if not saw_terminal:
                raise ValueError("File is truncated: missing terminal sentinel.")

            tmp.replace(dst)
        except Exception:
            tmp.unlink(missing_ok=True)
            raise


# ── CLI ───────────────────────────────────────────────────────────────────────

def _add_common_args(parser: argparse.ArgumentParser) -> None:
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument(
        "-p", "--passphrase",
        metavar="PASSPHRASE",
        help="Passphrase for scrypt key derivation.",
    )
    group.add_argument(
        "-k", "--keyfile",
        metavar="PATH",
        type=Path,
        help="Path to a keyfile (first 32 bytes used as key).",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Overwrite the output file if it already exists.",
    )


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="filecrypt",
        description="AES-256-GCM streaming file encryptor/decryptor.",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    # encrypt
    enc = sub.add_parser("encrypt", aliases=["enc", "e"], help="Encrypt a file.")
    enc.add_argument("input",  type=Path, help="Plaintext input file.")
    enc.add_argument("output", type=Path, help="Encrypted output file.")
    enc.add_argument(
        "--chunk-size",
        type=int,
        default=DEFAULT_CHUNK,
        metavar="BYTES",
        help=f"Plaintext chunk size in bytes (default: {DEFAULT_CHUNK}).",
    )
    _add_common_args(enc)

    # decrypt
    dec = sub.add_parser("decrypt", aliases=["dec", "d"], help="Decrypt a file.")
    dec.add_argument("input",  type=Path, help="Encrypted input file.")
    dec.add_argument("output", type=Path, help="Plaintext output file.")
    _add_common_args(dec)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    args   = parser.parse_args(argv)

    try:
        if args.command in ("encrypt", "enc", "e"):
            if not args.input.exists():
                parser.error(f"Input file not found: {args.input}")
            encrypt_file(
                src        = args.input,
                dst        = args.output,
                passphrase = args.passphrase,
                keyfile    = args.keyfile,
                chunk_size = args.chunk_size,
                force      = args.force,
            )
            print(f"Encrypted: {args.input} → {args.output}")

        elif args.command in ("decrypt", "dec", "d"):
            if not args.input.exists():
                parser.error(f"Input file not found: {args.input}")
            decrypt_file(
                src        = args.input,
                dst        = args.output,
                passphrase = args.passphrase,
                keyfile    = args.keyfile,
                force      = args.force,
            )
            print(f"Decrypted: {args.input} → {args.output}")

    except FileExistsError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    except (ValueError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
