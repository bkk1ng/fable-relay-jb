"""
emit.py – x86-64 instruction emitter (Python 3, stdlib only)

Public API
----------
emit_sequence(moves, xor_pairs, syscall_num, nop_sled_len) -> str
    moves         : list of (reg: str, imm: int)   – MOV reg, imm
    xor_pairs     : list of (reg1: str, reg2: str) – XOR reg1, reg2
    syscall_num   : int                            – loaded into rax, then SYSCALL
    nop_sled_len  : int                            – prepended NOP bytes
    returns       : lowercase hex string of all bytes, no separators
"""

from __future__ import annotations
import struct
import unittest

# ---------------------------------------------------------------------------
# Register tables
# ---------------------------------------------------------------------------

# 64-bit registers: (REX.B bit, ModRM reg/rm field)
_REG64: dict[str, tuple[int, int]] = {
    "rax": (0, 0), "rcx": (0, 1), "rdx": (0, 2), "rbx": (0, 3),
    "rsp": (0, 4), "rbp": (0, 5), "rsi": (0, 6), "rdi": (0, 7),
    "r8":  (1, 0), "r9":  (1, 1), "r10": (1, 2), "r11": (1, 3),
    "r12": (1, 4), "r13": (1, 5), "r14": (1, 6), "r15": (1, 7),
}


def _reg(name: str) -> tuple[int, int]:
    n = name.lower()
    if n not in _REG64:
        raise ValueError(f"Unknown register: {name!r}")
    return _REG64[n]


# ---------------------------------------------------------------------------
# Instruction encoders
# ---------------------------------------------------------------------------

def _encode_nop(count: int) -> bytes:
    """Return `count` single-byte NOP (0x90) instructions."""
    return b"\x90" * count


def _encode_mov_reg_imm64(reg: str, imm: int) -> bytes:
    """
    MOV r64, imm64  –  REX.W + B8+rd  (10-byte form)
    Handles the full signed/unsigned 64-bit range.
    imm is sign-extended from Python int; we pack as unsigned 64-bit.
    """
    rex_b, rd = _reg(reg)
    # REX prefix: 0100 W R X B  (W=1 for 64-bit, B extends opcode reg field)
    rex = 0x48 | rex_b          # REX.W always set; REX.B when rd >= 8
    opcode = 0xB8 | rd          # B8+rd
    # Normalise imm to unsigned 64-bit
    imm_u64 = imm & 0xFFFFFFFFFFFFFFFF
    return bytes([rex, opcode]) + struct.pack("<Q", imm_u64)


def _encode_xor_reg_reg(dst: str, src: str) -> bytes:
    """
    XOR r64, r64  –  REX.W 0x33 /r  (3-byte form)
    ModRM = 11_src_dst  (mod=3, reg=src, rm=dst)
    REX.R extends the reg (src) field; REX.B extends the rm (dst) field.
    """
    rex_b_dst, rm = _reg(dst)
    rex_b_src, reg = _reg(src)
    rex = 0x48 | (rex_b_src << 2) | rex_b_dst   # REX.W | REX.R | REX.B
    modrm = 0xC0 | (reg << 3) | rm              # mod=11, reg=src, rm=dst
    return bytes([rex, 0x33, modrm])


def _encode_syscall() -> bytes:
    """SYSCALL – 2-byte encoding 0F 05."""
    return b"\x0f\x05"


# ---------------------------------------------------------------------------
# Main public function
# ---------------------------------------------------------------------------

def emit_sequence(
    moves: list[tuple[str, int]],
    xor_pairs: list[tuple[str, str]],
    syscall_num: int,
    nop_sled_len: int,
) -> str:
    """
    Build machine-code bytes for the requested sequence and return hex.

    Encoding order:
      1. NOP sled (nop_sled_len bytes)
      2. MOV instructions from `moves`
      3. XOR instructions from `xor_pairs`
      4. MOV rax, syscall_num
      5. SYSCALL

    Returns a lowercase hex string with no separators.
    """
    if nop_sled_len < 0:
        raise ValueError("nop_sled_len must be >= 0")

    buf = bytearray()
    buf += _encode_nop(nop_sled_len)

    for reg, imm in moves:
        buf += _encode_mov_reg_imm64(reg, imm)

    for dst, src in xor_pairs:
        buf += _encode_xor_reg_reg(dst, src)

    buf += _encode_mov_reg_imm64("rax", syscall_num)
    buf += _encode_syscall()

    return buf.hex()


# ---------------------------------------------------------------------------
# Regression tests
# ---------------------------------------------------------------------------

class _Tests(unittest.TestCase):

    # --- NOP sled -----------------------------------------------------------

    def test_nop_sled_zero(self):
        result = emit_sequence([], [], 0, 0)
        # Only: MOV rax,0 + SYSCALL = 10 + 2 = 12 bytes
        self.assertEqual(len(bytes.fromhex(result)), 12)

    def test_nop_sled_length(self):
        result = emit_sequence([], [], 0, 8)
        blob = bytes.fromhex(result)
        # 8 NOPs at the front
        self.assertEqual(blob[:8], b"\x90" * 8)

    # --- MOV r64, imm64 -----------------------------------------------------

    def test_mov_rax_zero_bytes(self):
        # REX.W=48, opcode=B8, 8 zero bytes
        blob = _encode_mov_reg_imm64("rax", 0)
        self.assertEqual(len(blob), 10)
        self.assertEqual(blob[0], 0x48)
        self.assertEqual(blob[1], 0xB8)
        self.assertEqual(blob[2:], b"\x00" * 8)

    def test_mov_rax_1(self):
        blob = _encode_mov_reg_imm64("rax", 1)
        self.assertEqual(blob, bytes.fromhex("48b80100000000000000"))

    def test_mov_rdi_imm(self):
        # rdi: rex_b=0, rd=7  → REX=0x48, opcode=0xBF
        blob = _encode_mov_reg_imm64("rdi", 0x1234)
        self.assertEqual(len(blob), 10)
        self.assertEqual(blob[0], 0x48)
        self.assertEqual(blob[1], 0xBF)
        self.assertEqual(blob[2:], struct.pack("<Q", 0x1234))

    def test_mov_r8_imm(self):
        # r8: rex_b=1, rd=0  → REX=0x49, opcode=0xB8
        blob = _encode_mov_reg_imm64("r8", 0xDEAD)
        self.assertEqual(blob[0], 0x49)
        self.assertEqual(blob[1], 0xB8)

    def test_mov_r15_imm(self):
        # r15: rex_b=1, rd=7  → REX=0x49, opcode=0xBF
        blob = _encode_mov_reg_imm64("r15", 0xFF)
        self.assertEqual(blob[0], 0x49)
        self.assertEqual(blob[1], 0xBF)

    def test_mov_negative_imm(self):
        # -1 should pack as 0xFFFFFFFFFFFFFFFF
        blob = _encode_mov_reg_imm64("rax", -1)
        self.assertEqual(blob[2:], b"\xff" * 8)

    # --- XOR r64, r64 -------------------------------------------------------

    def test_xor_rax_rax_bytes(self):
        # XOR rax, rax: REX=48, 0x33, ModRM=C0
        blob = _encode_xor_reg_reg("rax", "rax")
        self.assertEqual(len(blob), 3)
        self.assertEqual(blob, bytes.fromhex("4833c0"))

    def test_xor_rdx_rcx(self):
        # dst=rdx (rm=2,rex_b=0), src=rcx (reg=1,rex_r=0)
        # REX=48, 0x33, ModRM= 11_001_010 = 0xCA
        blob = _encode_xor_reg_reg("rdx", "rcx")
        self.assertEqual(blob, bytes.fromhex("4833ca"))

    def test_xor_r9_r8(self):
        # dst=r9 (rex_b=1,rm=1), src=r8 (rex_r=1,reg=0)
        # REX = 0x48 | (1<<2) | 1 = 0x4D
        # ModRM = 0xC0 | (0<<3) | 1 = 0xC1
        blob = _encode_xor_reg_reg("r9", "r8")
        self.assertEqual(blob[0], 0x4D)
        self.assertEqual(blob[1], 0x33)
        self.assertEqual(blob[2], 0xC1)

    def test_xor_r10_rax(self):
        # dst=r10 (rex_b=1,rm=2), src=rax (rex_r=0,reg=0)
        # REX=0x48|1=0x49, ModRM=0xC0|0|2=0xC2
        blob = _encode_xor_reg_reg("r10", "rax")
        self.assertEqual(blob[0], 0x49)
        self.assertEqual(blob[2], 0xC2)

    # --- SYSCALL ------------------------------------------------------------

    def test_syscall_bytes(self):
        self.assertEqual(_encode_syscall(), b"\x0f\x05")

    # --- Full sequence byte lengths -----------------------------------------

    def test_sequence_byte_count_no_sled(self):
        # 2 MOVs (20) + 1 XOR (3) + MOV rax (10) + SYSCALL (2) = 35
        result = emit_sequence(
            moves=[("rdi", 1), ("rsi", 0x400000)],
            xor_pairs=[("rdx", "rdx")],
            syscall_num=1,
            nop_sled_len=0,
        )
        self.assertEqual(len(bytes.fromhex(result)), 35)

    def test_sequence_byte_count_with_sled(self):
        # sled=16, 1 MOV (10), 0 XORs, MOV rax (10), SYSCALL (2) = 38
        result = emit_sequence(
            moves=[("rdi", 42)],
            xor_pairs=[],
            syscall_num=60,
            nop_sled_len=16,
        )
        self.assertEqual(len(bytes.fromhex(result)), 38)

    def test_sequence_sled_prefix_present(self):
        result = emit_sequence([], [], 60, 4)
        blob = bytes.fromhex(result)
        self.assertEqual(blob[:4], b"\x90" * 4)

    def test_sequence_ends_with_syscall(self):
        result = emit_sequence([], [], 0, 0)
        blob = bytes.fromhex(result)
        self.assertEqual(blob[-2:], b"\x0f\x05")

    # --- Hex output format --------------------------------------------------

    def test_output_is_lowercase_hex(self):
        result = emit_sequence([], [], 0, 0)
        self.assertEqual(result, result.lower())
        int(result, 16)  # must be valid hex

    # --- Edge cases ---------------------------------------------------------

    def test_unknown_register_raises(self):
        with self.assertRaises(ValueError):
            _encode_mov_reg_imm64("eax", 0)

    def test_negative_sled_raises(self):
        with self.assertRaises(ValueError):
            emit_sequence([], [], 0, -1)

    def test_large_imm(self):
        imm = 0xCAFEBABEDEADBEEF
        blob = _encode_mov_reg_imm64("rbx", imm)
        self.assertEqual(struct.unpack("<Q", blob[2:])[0], imm)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    unittest.main()
