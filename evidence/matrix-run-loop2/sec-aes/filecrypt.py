#!/usr/bin/env python3
"""
filecrypt.py — AES-256-GCM streaming file encryptor/decryptor.

File format (encrypt output):
  [4]  magic: b'FCRY'
  [1]  version: 0x01
  [32] scrypt salt
  [4]  chunk_size (big-endian uint32, the plaintext chunk size used)
  then, for each chunk:
    [12] nonce
    [4]  sequence number (big-endian uint32, authenticated as AAD)
    [2]  ciphertext length (big-endian uint16)
    [N]  ciphertext + 16-byte GCM tag

The sequence number is included as AAD so reordering or truncation is detected.
The final chunk has the high bit of its sequence number set (0x80000000) so a
truncated file missing the last chunk is rejected.
"""

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
SALT_LEN       = 32
NONCE_LEN      = 12
TAG_LEN        = 16          # AES-GCM authentication tag, appended by AESGCM
KEY_LEN        = 32          # AES-256
CHUNK_SIZE     = 64 * 1024   # 64 KiB plaintext per chunk
HEADER_STRUCT  = struct.Struct(">4sBL")   # magic(4) version(1) chunk_size(4)
SEQ_STRUCT     = struct.Struct(">L")      # sequence number

# scrypt parameters (N, r, p) — tune for ~100 ms on a modern laptop
SCRYPT_N = 2**17
SCRYPT_R = 8
SCRYPT_P = 1

FINAL_FLAG = 0x80000000  # OR'd into sequence number of the last chunk


# ── key derivation ────────────────────────────────────────────────────────────

def derive_key(passphrase: bytes, salt: bytes) -> bytes:
    """Derive a 256-bit key from a passphrase via scrypt."""
    kdf = Scrypt(
        salt=salt,
        length=KEY_LEN,
        n=SCRYPT_N,
        r=SCRYPT_R,
        p=SCRYPT_P,
        backend=default_backend(),
    )
    return kdf.derive(passphrase)


def load_secret(passphrase: str | None, keyfile: str | None) -> bytes:
    """Return the raw secret bytes from a passphrase or keyfile."""
    if keyfile is not None:
        with open(keyfile, "rb") as fh:
            return fh.read()
    if passphrase is not None:
        return passphrase.encode()
    # Prompt without echoing when nothing was supplied on the CLI.
    import getpass
    pw = getpass.getpass("Passphrase: ")
    return pw.encode()


# ── encrypt ───────────────────────────────────────────────────────────────────

def encrypt_file(
    src: Path,
    dst: Path,
    secret: bytes,
    force: bool,
) -> None:
    if dst.exists() and not force:
        sys.exit(f"error: '{dst}' already exists — use --force to overwrite")

    salt = os.urandom(SALT_LEN)
    key  = derive_key(secret, salt)
    aes  = AESGCM(key)

    # Collect chunks first so we know which one is last.
    with open(src, "rb") as fh:
        chunks = []
        while True:
            block = fh.read(CHUNK_SIZE)
            if not block:
                break
            chunks.append(block)

    # An empty source file produces a single empty chunk so the format stays
    # well-defined and we still write auth-tagged output.
    if not chunks:
        chunks = [b""]

    with open(dst, "wb") as fh:
        # Header
        fh.write(MAGIC)
        fh.write(bytes([VERSION]))
        fh.write(salt)
        fh.write(struct.pack(">L", CHUNK_SIZE))

        for idx, block in enumerate(chunks):
            is_last = idx == len(chunks) - 1
            seq     = idx | (FINAL_FLAG if is_last else 0)
            nonce   = os.urandom(NONCE_LEN)
            aad     = SEQ_STRUCT.pack(seq)

            ciphertext = aes.encrypt(nonce, block, aad)  # appends 16-byte tag

            fh.write(nonce)
            fh.write(aad)
            fh.write(struct.pack(">H", len(ciphertext)))
            fh.write(ciphertext)

    print(f"encrypted: '{src}' → '{dst}'")


# ── decrypt ───────────────────────────────────────────────────────────────────

def decrypt_file(
    src: Path,
    dst: Path,
    secret: bytes,
    force: bool,
) -> None:
    if dst.exists() and not force:
        sys.exit(f"error: '{dst}' already exists — use --force to overwrite")

    with open(src, "rb") as fh:

        # ── header ──────────────────────────────────────────────────────────
        magic = fh.read(4)
        if magic != MAGIC:
            sys.exit("error: not a filecrypt file (bad magic)")

        version_byte = fh.read(1)
        if not version_byte:
            sys.exit("error: truncated header (version)")
        if version_byte[0] != VERSION:
            sys.exit(f"error: unsupported version {version_byte[0]:#04x}")

        salt = fh.read(SALT_LEN)
        if len(salt) != SALT_LEN:
            sys.exit("error: truncated header (salt)")

        raw_chunk = fh.read(4)
        if len(raw_chunk) != 4:
            sys.exit("error: truncated header (chunk_size)")
        (file_chunk_size,) = struct.unpack(">L", raw_chunk)

        # ── key derivation ───────────────────────────────────────────────────
        key = derive_key(secret, salt)
        aes = AESGCM(key)

        # ── stream chunks to a temp file, then rename ────────────────────────
        # Writing to a temp file means a failed decryption never creates a
        # partial plaintext output at the destination path.
        tmp_path = dst.with_suffix(dst.suffix + ".tmp")
        try:
            with open(tmp_path, "wb") as out:
                expected_seq = 0
                saw_final    = False

                while True:
                    nonce = fh.read(NONCE_LEN)
                    if not nonce:
                        break  # end of file — check termination below
                    if len(nonce) != NONCE_LEN:
                        sys.exit("error: truncated chunk (nonce)")

                    aad = fh.read(4)
                    if len(aad) != 4:
                        sys.exit("error: truncated chunk (sequence)")
                    (seq,) = SEQ_STRUCT.unpack(aad)

                    is_final    = bool(seq & FINAL_FLAG)
                    actual_seq  = seq & ~FINAL_FLAG

                    if actual_seq != expected_seq:
                        sys.exit(
                            f"error: chunk sequence mismatch "
                            f"(got {actual_seq}, expected {expected_seq})"
                        )

                    raw_len = fh.read(2)
                    if len(raw_len) != 2:
                        sys.exit("error: truncated chunk (length field)")
                    (ct_len,) = struct.unpack(">H", raw_len)

                    ciphertext = fh.read(ct_len)
                    if len(ciphertext) != ct_len:
                        sys.exit("error: truncated chunk (ciphertext)")

                    try:
                        plaintext = aes.decrypt(nonce, ciphertext, aad)
                    except Exception:
                        sys.exit(
                            f"error: authentication failed on chunk {actual_seq} "
                            "(wrong passphrase/keyfile or corrupted data)"
                        )

                    out.write(plaintext)
                    expected_seq += 1

                    if is_final:
                        saw_final = True
                        # Confirm there's nothing left in the file.
                        trailing = fh.read(1)
                        if trailing:
                            sys.exit("error: unexpected data after final chunk")
                        break

                if not saw_final:
                    sys.exit("error: file is truncated (final chunk never seen)")

            tmp_path.replace(dst)

        except BaseException:
            # Clean up the temp file on any failure.
            try:
                tmp_path.unlink(missing_ok=True)
            except OSError:
                pass
            raise

    print(f"decrypted: '{src}' → '{dst}'")


# ── CLI ───────────────────────────────────────────────────────────────────────

def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="filecrypt",
        description="AES-256-GCM streaming file encryptor/decryptor.",
    )

    secret_group = argparse.ArgumentParser(add_help=False)
    sg = secret_group.add_mutually_exclusive_group()
    sg.add_argument(
        "--passphrase", "-p",
        metavar="PASS",
        help="passphrase (omit to prompt without echo)",
    )
    sg.add_argument(
        "--keyfile", "-k",
        metavar="FILE",
        help="path to a keyfile whose raw bytes are used as the secret",
    )

    common = argparse.ArgumentParser(add_help=False, parents=[secret_group])
    common.add_argument(
        "--force", "-f",
        action="store_true",
        help="overwrite the output file if it already exists",
    )

    sub = parser.add_subparsers(dest="command", required=True)

    enc = sub.add_parser(
        "encrypt",
        parents=[common],
        help="encrypt a file",
    )
    enc.add_argument("input",  type=Path, help="plaintext input file")
    enc.add_argument("output", type=Path, help="encrypted output file")

    dec = sub.add_parser(
        "decrypt",
        parents=[common],
        help="decrypt a file",
    )
    dec.add_argument("input",  type=Path, help="encrypted input file")
    dec.add_argument("output", type=Path, help="plaintext output file")

    return parser


def main() -> None:
    parser = build_parser()
    args   = parser.parse_args()

    secret = load_secret(args.passphrase, args.keyfile)

    if args.command == "encrypt":
        if not args.input.is_file():
            sys.exit(f"error: input file '{args.input}' not found")
        encrypt_file(args.input, args.output, secret, args.force)

    elif args.command == "decrypt":
        if not args.input.is_file():
            sys.exit(f"error: input file '{args.input}' not found")
        decrypt_file(args.input, args.output, secret, args.force)


if __name__ == "__main__":
    main()
