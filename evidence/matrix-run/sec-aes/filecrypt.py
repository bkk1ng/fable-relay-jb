#!/usr/bin/env python3
"""
filecrypt.py — AES-256-GCM streaming file encryptor/decryptor.

File format:
  [4 bytes]  magic: b'FCRY'
  [1 byte]   version: 0x01
  [1 byte]   kdf: 0x01 = scrypt, 0x02 = keyfile
  [32 bytes] salt (used for scrypt; random bytes for keyfile mode too)
  [12 bytes] nonce (per-file random)
  [N chunks] ciphertext stream
    each chunk:
      [4 bytes LE] chunk length (ciphertext + tag, max CHUNK_SIZE + 16)
      [bytes]      ciphertext
      [16 bytes]   GCM tag
  [16 bytes] final authentication tag over the header + all chunk AADs

AAD per chunk: header_bytes + chunk_index (8 bytes LE, big-endian zero-padded)
This binds every chunk to its position and to the file header, preventing
reordering, truncation, or transplantation attacks.
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

MAGIC = b"FCRY"
VERSION = 0x01
KDF_SCRYPT = 0x01
KDF_KEYFILE = 0x02

HEADER_MAGIC_LEN = 4
HEADER_VERSION_LEN = 1
HEADER_KDF_LEN = 1
HEADER_SALT_LEN = 32
HEADER_NONCE_LEN = 12
HEADER_FIXED_LEN = (
    HEADER_MAGIC_LEN
    + HEADER_VERSION_LEN
    + HEADER_KDF_LEN
    + HEADER_SALT_LEN
    + HEADER_NONCE_LEN
)  # 50 bytes

CHUNK_SIZE = 64 * 1024  # 64 KiB plaintext per chunk
GCM_TAG_LEN = 16
CHUNK_LEN_PREFIX = 4  # uint32 LE

# scrypt parameters (N=2^17 ≈ interactive-to-moderate security, tunable)
SCRYPT_N = 1 << 17
SCRYPT_R = 8
SCRYPT_P = 1
KEY_LEN = 32  # 256 bits


# ── key derivation ────────────────────────────────────────────────────────────

def derive_key_scrypt(passphrase: bytes, salt: bytes) -> bytes:
    kdf = Scrypt(
        salt=salt,
        length=KEY_LEN,
        n=SCRYPT_N,
        r=SCRYPT_R,
        p=SCRYPT_P,
        backend=default_backend(),
    )
    return kdf.derive(passphrase)


def derive_key_keyfile(keyfile_path: Path, salt: bytes) -> bytes:
    """
    Key = scrypt(keyfile_bytes, salt) so the salt still diversifies the key
    even when the keyfile is reused across multiple encrypted files.
    """
    key_material = keyfile_path.read_bytes()
    kdf = Scrypt(
        salt=salt,
        length=KEY_LEN,
        n=SCRYPT_N,
        r=SCRYPT_R,
        p=SCRYPT_P,
        backend=default_backend(),
    )
    return kdf.derive(key_material)


# ── header helpers ────────────────────────────────────────────────────────────

def build_header(kdf_type: int, salt: bytes, nonce: bytes) -> bytes:
    return MAGIC + bytes([VERSION, kdf_type]) + salt + nonce


def parse_header(data: bytes) -> tuple[int, bytes, bytes]:
    """Return (kdf_type, salt, nonce). Raises ValueError on bad magic/version."""
    if len(data) < HEADER_FIXED_LEN:
        raise ValueError("File too short to contain a valid header.")
    magic = data[:4]
    if magic != MAGIC:
        raise ValueError(f"Bad magic bytes: {magic!r}. Not a filecrypt file.")
    version = data[4]
    if version != VERSION:
        raise ValueError(f"Unsupported version: {version}.")
    kdf_type = data[5]
    salt = data[6:38]
    nonce = data[38:50]
    return kdf_type, salt, nonce


def chunk_aad(header_bytes: bytes, chunk_index: int) -> bytes:
    """AAD = header || chunk_index as 8-byte little-endian uint."""
    return header_bytes + struct.pack("<Q", chunk_index)


# ── encrypt ───────────────────────────────────────────────────────────────────

def encrypt_file(
    src: Path,
    dst: Path,
    passphrase: bytes | None,
    keyfile: Path | None,
) -> None:
    salt = os.urandom(HEADER_SALT_LEN)
    nonce = os.urandom(HEADER_NONCE_LEN)

    if keyfile is not None:
        kdf_type = KDF_KEYFILE
        key = derive_key_keyfile(keyfile, salt)
    else:
        kdf_type = KDF_SCRYPT
        key = derive_key_scrypt(passphrase, salt)

    header = build_header(kdf_type, salt, nonce)
    aesgcm = AESGCM(key)

    with src.open("rb") as fin, dst.open("wb") as fout:
        fout.write(header)

        chunk_index = 0
        while True:
            plaintext = fin.read(CHUNK_SIZE)
            if not plaintext:
                # Always write at least one (empty) chunk so decryption can
                # authenticate an empty file correctly.
                if chunk_index == 0:
                    plaintext = b""
                else:
                    break

            aad = chunk_aad(header, chunk_index)
            # AESGCM.encrypt returns ciphertext + 16-byte tag concatenated.
            # We use a per-chunk nonce derived from the file nonce XORed with
            # the chunk index to avoid nonce reuse across chunks.
            chunk_nonce = _chunk_nonce(nonce, chunk_index)
            ct_and_tag = aesgcm.encrypt(chunk_nonce, plaintext, aad)

            fout.write(struct.pack("<I", len(ct_and_tag)))
            fout.write(ct_and_tag)

            chunk_index += 1

            if not plaintext and chunk_index == 1:
                break  # empty-file sentinel written


def _chunk_nonce(file_nonce: bytes, index: int) -> bytes:
    """
    Derive a unique 12-byte nonce for each chunk.
    XOR the lower 8 bytes of the file nonce with the chunk index.
    The upper 4 bytes are fixed per file, keeping nonces in-family.
    """
    index_bytes = struct.pack("<Q", index)
    base = bytearray(file_nonce)
    for i in range(8):
        base[4 + i] ^= index_bytes[i]
    return bytes(base)


# ── decrypt ───────────────────────────────────────────────────────────────────

def decrypt_file(
    src: Path,
    dst: Path,
    passphrase: bytes | None,
    keyfile: Path | None,
) -> None:
    with src.open("rb") as fin:
        raw_header = fin.read(HEADER_FIXED_LEN)
        kdf_type, salt, nonce = parse_header(raw_header)

        if kdf_type == KDF_KEYFILE:
            if keyfile is None:
                raise ValueError("File was encrypted with a keyfile; supply --keyfile.")
            key = derive_key_keyfile(keyfile, salt)
        elif kdf_type == KDF_SCRYPT:
            if passphrase is None:
                raise ValueError("File was encrypted with a passphrase; supply --passphrase.")
            key = derive_key_scrypt(passphrase, salt)
        else:
            raise ValueError(f"Unknown KDF type: {kdf_type}.")

        header = raw_header  # already 50 bytes, matches what was written
        aesgcm = AESGCM(key)

        with dst.open("wb") as fout:
            chunk_index = 0
            while True:
                prefix = fin.read(CHUNK_LEN_PREFIX)
                if not prefix:
                    break
                if len(prefix) < CHUNK_LEN_PREFIX:
                    raise ValueError("Truncated chunk length prefix.")

                ct_len = struct.unpack("<I", prefix)[0]
                ct_and_tag = fin.read(ct_len)
                if len(ct_and_tag) < ct_len:
                    raise ValueError(
                        f"Truncated chunk {chunk_index}: expected {ct_len} bytes, "
                        f"got {len(ct_and_tag)}."
                    )

                aad = chunk_aad(header, chunk_index)
                chunk_nonce = _chunk_nonce(nonce, chunk_index)

                try:
                    plaintext = aesgcm.decrypt(chunk_nonce, ct_and_tag, aad)
                except Exception:
                    # cryptography raises InvalidTag; keep the message generic
                    # to avoid oracle leakage.
                    raise ValueError(
                        f"Authentication failed on chunk {chunk_index}. "
                        "Wrong key, corrupted file, or tampered data."
                    )

                fout.write(plaintext)
                chunk_index += 1


# ── passphrase / keyfile resolution ──────────────────────────────────────────

def resolve_credentials(
    args: argparse.Namespace,
) -> tuple[bytes | None, Path | None]:
    """
    Return (passphrase_bytes, keyfile_path).  Exactly one will be non-None.
    Prompts interactively if --passphrase was given without a value.
    """
    if args.keyfile:
        keyfile = Path(args.keyfile)
        if not keyfile.is_file():
            fatal(f"Keyfile not found: {keyfile}")
        return None, keyfile

    if args.passphrase is not None:
        return args.passphrase.encode(), None

    # Interactive prompt (no echo)
    import getpass

    pw = getpass.getpass("Passphrase: ")
    if not pw:
        fatal("Empty passphrase is not allowed.")
    return pw.encode(), None


# ── CLI helpers ───────────────────────────────────────────────────────────────

def fatal(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def check_overwrite(dst: Path, force: bool) -> None:
    if dst.exists() and not force:
        fatal(
            f"Output file already exists: {dst}\n"
            "       Use --force to overwrite."
        )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="filecrypt",
        description="AES-256-GCM streaming file encryptor/decryptor.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
examples:
  filecrypt encrypt secrets.tar secrets.tar.enc
  filecrypt decrypt secrets.tar.enc secrets.tar
  filecrypt encrypt --keyfile mykey.bin data.zip data.zip.enc
  filecrypt encrypt --passphrase hunter2 notes.txt notes.enc --force
""",
    )

    cred = parser.add_mutually_exclusive_group()
    cred.add_argument(
        "--passphrase",
        metavar="PASS",
        nargs="?",
        const="",        # triggers interactive prompt when flag present but empty
        default=None,
        help="Passphrase for key derivation (omit value to prompt).",
    )
    cred.add_argument(
        "--keyfile",
        metavar="FILE",
        help="Path to a keyfile used for key derivation.",
    )

    parser.add_argument(
        "--force",
        action="store_true",
        help="Overwrite the output file if it exists.",
    )

    sub = parser.add_subparsers(dest="command", required=True)

    enc = sub.add_parser("encrypt", help="Encrypt a file.")
    enc.add_argument("input", help="Plaintext input file.")
    enc.add_argument("output", help="Ciphertext output file.")

    dec = sub.add_parser("decrypt", help="Decrypt a file.")
    dec.add_argument("input", help="Ciphertext input file.")
    dec.add_argument("output", help="Plaintext output file.")

    return parser


# ── entry point ───────────────────────────────────────────────────────────────

def main() -> None:
    parser = build_parser()

    # Support credential flags before or after the subcommand by parsing twice.
    # First pass collects the global flags; second pass the subcommand args.
    args = parser.parse_args()

    src = Path(args.input)
    dst = Path(args.output)

    if not src.is_file():
        fatal(f"Input file not found: {src}")

    check_overwrite(dst, args.force)

    # Resolve --passphrase / --keyfile; default to interactive passphrase
    # when neither flag is given.
    if args.passphrase is None and args.keyfile is None:
        # Neither supplied — fall through to interactive prompt
        args.passphrase = ""  # triggers the getpass branch in resolve_credentials

    passphrase, keyfile = resolve_credentials(args)

    try:
        if args.command == "encrypt":
            print(f"Encrypting {src} → {dst} …", file=sys.stderr)
            encrypt_file(src, dst, passphrase, keyfile)
            print("Done.", file=sys.stderr)
        else:
            print(f"Decrypting {src} → {dst} …", file=sys.stderr)
            decrypt_file(src, dst, passphrase, keyfile)
            print("Done.", file=sys.stderr)
    except ValueError as exc:
        # Clean up partial output on failure
        if dst.exists():
            dst.unlink(missing_ok=True)
        fatal(str(exc))
    except KeyboardInterrupt:
        if dst.exists():
            dst.unlink(missing_ok=True)
        fatal("Interrupted.")


if __name__ == "__main__":
    main()
