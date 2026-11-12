"""
CRISPR Knock-in Protocol Toolkit
Language: Python 3.10+
Target: HDR-based precise genomic insertion

Covers:
  - PAM site discovery (SpCas9: NGG; Cas12a: TTTV)
  - gRNA scoring (Rule Set 2 / Doench-inspired heuristics)
  - Off-target candidate enumeration (up to N mismatches)
  - Donor template construction (ssODN or dsDNA HDR)
  - Genotyping primer design
  - End-to-end experiment design entry point
"""

from __future__ import annotations

import re
import math
import itertools
from dataclasses import dataclass, field
from typing import Iterator, Literal


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

COMPLEMENT: dict[str, str] = str.maketrans("ACGTacgt", "TGCAtgca")

# Position-specific scoring weights for 20-nt SpCas9 guide (1-indexed, PAM-distal to PAM-proximal)
# Derived from Doench et al. 2016 Rule Set 2 simplified positional model.
DOENCH_WEIGHTS: dict[int, dict[str, float]] = {
    # position: {nucleotide: weight}  — only positions with notable effects listed
    1:  {"G": 0.08, "C": 0.00, "A": -0.02, "T": -0.06},
    2:  {"G": 0.07, "C": 0.00, "A": -0.03, "T": -0.04},
    3:  {"G": 0.04, "C": 0.00, "A": -0.01, "T": -0.03},
    16: {"G": 0.11, "C": -0.05, "A": -0.03, "T": -0.03},
    17: {"G": 0.10, "C": -0.06, "A": -0.02, "T": -0.02},
    18: {"G": 0.09, "C": -0.08, "A": -0.01, "T": 0.00},
    19: {"G": 0.08, "C": -0.09, "A": 0.00, "T": 0.01},
    20: {"G": 0.05, "C": -0.11, "A": 0.01, "T": 0.05},
}

# Mismatch penalty by distance from PAM (position 1 = PAM-proximal = highest penalty)
MISMATCH_PENALTY: list[float] = [
    1.0, 0.95, 0.90, 0.85, 0.75, 0.65, 0.55, 0.45, 0.35, 0.25,  # pos 1-10 (PAM-proximal)
    0.18, 0.14, 0.11, 0.09, 0.07, 0.06, 0.05, 0.04, 0.03, 0.02,  # pos 11-20 (PAM-distal)
]


# ---------------------------------------------------------------------------
# Sequence utilities
# ---------------------------------------------------------------------------

def reverse_complement(seq: str) -> str:
    return seq.translate(COMPLEMENT)[::-1]


def gc_content(seq: str) -> float:
    s = seq.upper()
    return (s.count("G") + s.count("C")) / len(s) if seq else 0.0


def has_poly_n(seq: str, n: int = 4) -> bool:
    """Return True if sequence contains a homopolymer run of length >= n."""
    for base in "ACGT":
        if base * n in seq.upper():
            return True
    return False


def hamming(a: str, b: str) -> int:
    if len(a) != len(b):
        raise ValueError("Sequences must be equal length for Hamming distance.")
    return sum(x != y for x, y in zip(a.upper(), b.upper()))


# ---------------------------------------------------------------------------
# Data classes
# ---------------------------------------------------------------------------

@dataclass
class GuideRNA:
    sequence: str           # 20-nt protospacer (5'→3', no PAM)
    pam: str                # PAM sequence
    strand: Literal["+", "-"]
    start: int              # 0-based start on the + strand genome
    end: int                # exclusive
    score: float = 0.0
    off_target_count: dict[int, int] = field(default_factory=dict)  # {mismatches: count}

    @property
    def full_sequence(self) -> str:
        return self.sequence + self.pam

    def __repr__(self) -> str:
        return (
            f"GuideRNA({self.sequence} | PAM={self.pam} | {self.strand} "
            f"| pos={self.start}-{self.end} | score={self.score:.3f})"
        )


@dataclass
class DonorTemplate:
    insert: str             # sequence to knock in
    left_homology: str      # left homology arm
    right_homology: str     # right homology arm
    silent_mutations: list[tuple[int, str, str]] = field(default_factory=list)
    # ^ list of (0-based position relative to full donor, original_base, mutant_base)

    @property
    def full_sequence(self) -> str:
        return self.left_homology + self.insert + self.right_homology

    @property
    def total_length(self) -> int:
        return len(self.full_sequence)

    def __repr__(self) -> str:
        return (
            f"DonorTemplate(insert={len(self.insert)} bp | "
            f"arms={len(self.left_homology)}/{len(self.right_homology)} bp | "
            f"total={self.total_length} bp)"
        )


@dataclass
class GenotypingPrimer:
    name: str
    sequence: str
    tm: float               # °C
    gc: float               # fraction
    amplicon_size: int      # expected product size in bp
    purpose: Literal["junction_fwd", "junction_rev", "wt_check_fwd", "wt_check_rev"]

    def __repr__(self) -> str:
        return (
            f"{self.name}: 5'-{self.sequence}-3'  Tm={self.tm:.1f}°C  "
            f"GC={self.gc:.0%}  amplicon≈{self.amplicon_size} bp"
        )


@dataclass
class KnockinDesign:
    target_locus: str
    cut_position: int       # 0-based position of DSB on + strand
    guide: GuideRNA
    donor: DonorTemplate
    genotyping_primers: list[GenotypingPrimer] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)

    def summary(self) -> str:
        lines = [
            f"=== Knock-in Design: {self.target_locus} ===",
            f"Guide     : {self.guide}",
            f"Cut site  : position {self.cut_position} (between nt {self.cut_position} and {self.cut_position+1})",
            f"Donor     : {self.donor}",
        ]
        if self.donor.silent_mutations:
            lines.append(f"Silent mut: {len(self.donor.silent_mutations)} introduced to prevent re-cutting")
        lines.append("Primers   :")
        for p in self.genotyping_primers:
            lines.append(f"  {p}")
        if self.notes:
            lines.append("Notes     :")
            for n in self.notes:
                lines.append(f"  - {n}")
        return "\n".join(lines)


# ---------------------------------------------------------------------------
# PAM site discovery
# ---------------------------------------------------------------------------

def _pam_pattern(nuclease: str) -> tuple[re.Pattern, re.Pattern, int]:
    """
    Returns (fwd_pattern, rev_pattern, guide_length).
    fwd_pattern matches PAM on the + strand (protospacer is upstream).
    rev_pattern matches PAM on the - strand (protospacer is downstream on + strand).
    """
    if nuclease == "SpCas9":
        guide_len = 20
        # NGG PAM: guide is 5' of the PAM on the same strand
        fwd = re.compile(r"(?=([ACGT]{20}[ACG]G))")
        rev = re.compile(r"(?=(C[ACG][ACGT]{20}))")
        return fwd, rev, guide_len
    elif nuclease == "SaCas9":
        guide_len = 21
        fwd = re.compile(r"(?=([ACGT]{21}[ACGT]{2}GAA[ACG]))")
        rev = re.compile(r"(?=([ACG]TTC[ACGT]{2}[ACGT]{21}))")
        return fwd, rev, guide_len
    elif nuclease == "Cas12a":
        guide_len = 23
        # TTTV PAM: PAM is 5' of the protospacer
        fwd = re.compile(r"(?=(TTT[ACG][ACGT]{23}))")
        rev = re.compile(r"(?=([ACGT]{23}[ACG]AAA))")
        return fwd, rev, guide_len
    else:
        raise ValueError(f"Unsupported nuclease: {nuclease}. Choose SpCas9, SaCas9, or Cas12a.")


def find_pam_sites(
    genome_seq: str,
    nuclease: str = "SpCas9",
) -> list[GuideRNA]:
    """
    Scan genome_seq for all PAM-adjacent protospacers.
    Returns list of GuideRNA objects (unscored).
    """
    seq = genome_seq.upper()
    fwd_pat, rev_pat, guide_len = _pam_pattern(nuclease)
    guides: list[GuideRNA] = []

    if nuclease in ("SpCas9", "SaCas9"):
        pam_len = 3 if nuclease == "SpCas9" else 6
        for m in fwd_pat.finditer(seq):
            full = m.group(1)
            protospacer = full[:guide_len]
            pam = full[guide_len:]
            if has_poly_n(protospacer):
                continue
            guides.append(GuideRNA(
                sequence=protospacer,
                pam=pam,
                strand="+",
                start=m.start(),
                end=m.start() + guide_len,
            ))
        for m in rev_pat.finditer(seq):
            full = m.group(1)
            pam_rc = full[:pam_len]
            proto_rc = full[pam_len:]
            protospacer = reverse_complement(proto_rc)
            pam = reverse_complement(pam_rc)
            pos_start = m.start() + pam_len
            if has_poly_n(protospacer):
                continue
            guides.append(GuideRNA(
                sequence=protospacer,
                pam=pam,
                strand="-",
                start=pos_start,
                end=pos_start + guide_len,
            ))

    elif nuclease == "Cas12a":
        pam_len = 4
        for m in fwd_pat.finditer(seq):
            full = m.group(1)
            pam = full[:pam_len]
            protospacer = full[pam_len:]
            if has_poly_n(protospacer):
                continue
            guides.append(GuideRNA(
                sequence=protospacer,
                pam=pam,
                strand="+",
                start=m.start() + pam_len,
                end=m.start() + pam_len + guide_len,
            ))
        for m in rev_pat.finditer(seq):
            full = m.group(1)
            proto_rc = full[:guide_len]
            pam_rc = full[guide_len:]
            protospacer = reverse_complement(proto_rc)
            pam = reverse_complement(pam_rc)
            pos_start = m.start()
            if has_poly_n(protospacer):
                continue
            guides.append(GuideRNA(
                sequence=protospacer,
                pam=pam,
                strand="-",
                start=pos_start,
                end=pos_start + guide_len,
            ))

    return guides


# ---------------------------------------------------------------------------
# gRNA scoring
# ---------------------------------------------------------------------------

def score_guide(guide: GuideRNA) -> float:
    """
    Returns a composite on-target activity score in [0, 1].
    Components:
      - GC content penalty (ideal 40-70%)
      - Positional nucleotide weights (Doench-inspired)
      - Homopolymer penalty
    """
    seq = guide.sequence.upper()
    if len(seq) != 20:
        # Adapt for non-20nt guides: use GC + homopolymer only
        gc = gc_content(seq)
        gc_score = 1.0 - 2.0 * abs(gc - 0.55)
        hp_penalty = 0.3 if has_poly_n(seq, 4) else 0.0
        return max(0.0, min(1.0, gc_score - hp_penalty))

    # GC content score (peak at 50-60%)
    gc = gc_content(seq)
    if gc < 0.25 or gc > 0.80:
        gc_score = 0.1
    elif 0.40 <= gc <= 0.70:
        gc_score = 1.0 - abs(gc - 0.55) * 0.5
    else:
        gc_score = 0.5 + (gc - 0.25) * 1.0 if gc < 0.40 else 1.0 - (gc - 0.70) * 2.0

    # Positional weight contribution
    pos_score = 0.0
    for pos, weights in DOENCH_WEIGHTS.items():
        base = seq[pos - 1]
        pos_score += weights.get(base, 0.0)
    # Normalize positional contribution to [-0.3, 0.3]
    pos_score = max(-0.3, min(0.3, pos_score))

    # Homopolymer penalty
    hp_penalty = 0.25 if has_poly_n(seq, 4) else 0.0

    # 5'-G preference for U6 promoter expression
    g_bonus = 0.05 if seq[0] == "G" else 0.0

    raw = gc_score + pos_score + g_bonus - hp_penalty
    return max(0.0, min(1.0, raw))


def rank_guides(guides: list[GuideRNA]) -> list[GuideRNA]:
    """Score and sort guides descending by score."""
    for g in guides:
        g.score = score_guide(g)
    return sorted(guides, key=lambda g: g.score, reverse=True)


# ---------------------------------------------------------------------------
# Off-target prediction
# ---------------------------------------------------------------------------

def _generate_mismatch_variants(seq: str, max_mismatches: int) -> Iterator[str]:
    """
    Yield all sequences within Hamming distance max_mismatches from seq.
    Only iterates positions; yields seq itself at distance 0.
    For genome-scale use, replace with an indexed aligner (e.g., Bowtie2).
    """
    bases = "ACGT"
    for n_mm in range(0, max_mismatches + 1):
        for positions in itertools.combinations(range(len(seq)), n_mm):
            alternatives = [bases.replace(seq[p].upper(), "") for p in positions]
            for replacements in itertools.product(*alternatives):
                variant = list(seq.upper())
                for p, r in zip(positions, replacements):
                    variant[p] = r
                yield "".join(variant)


def off_target_score(guide_seq: str, target_seq: str) -> float:
    """
    MIT off-target score for a single candidate off-target site.
    Score = product of mismatch penalties at each mismatched position.
    Returns 0.0 for perfect match (no off-target concern) → highest on-target.
    Convention: higher score = more likely to be cleaved off-target.
    """
    if len(guide_seq) != len(target_seq):
        return 0.0
    g = guide_seq.upper()
    t = target_seq.upper()
    # PAM-proximal = last positions of the guide (index 19 = most proximal)
    # MISMATCH_PENALTY[0] = PAM-proximal (most penalizing = highest penalty = less likely off-target)
    score = 1.0
    for i, (gb, tb) in enumerate(zip(g, t)):
        if gb != tb:
            # Position relative to PAM: index 0 of guide is PAM-distal (pos 20 penalty)
            pam_prox_idx = (len(g) - 1) - i  # 0 = PAM-proximal
            penalty_idx = min(pam_prox_idx, len(MISMATCH_PENALTY) - 1)
            score *= (1.0 - MISMATCH_PENALTY[penalty_idx])
    return score


def screen_off_targets(
    guide: GuideRNA,
    genome_seq: str,
    max_mismatches: int = 3,
    pam_pattern: str = "[ACG]G",  # SpCas9 NGG simplified
) -> dict[int, list[tuple[int, str]]]:
    """
    Enumerate off-target sites in genome_seq with up to max_mismatches mismatches.
    Returns {n_mismatches: [(position, matched_sequence), ...]}

    NOTE: For real experiments, use an indexed genome aligner (Bowtie, BWA, CRISPRitz).
    This brute-force approach is for demonstration on short sequences only.
    """
    pam_re = re.compile(pam_pattern)
    genome = genome_seq.upper()
    guide_seq = guide.sequence.upper()
    results: dict[int, list[tuple[int, str]]] = {n: [] for n in range(max_mismatches + 1)}

    # Scan both strands
    for strand_seq, is_rev in [(genome, False), (reverse_complement(genome), True)]:
        for i in range(len(strand_seq) - len(guide_seq) - 2):
            candidate = strand_seq[i: i + len(guide_seq)]
            pam_region = strand_seq[i + len(guide_seq): i + len(guide_seq) + 3]
            if not pam_re.match(pam_region):
                continue
            mm = hamming(guide_seq, candidate)
            if mm <= max_mismatches:
                # Translate position back to + strand if on rev
                pos = (len(genome) - i - len(guide_seq)) if is_rev else i
                results[mm].append((pos, candidate))

    for n, hits in results.items():
        guide.off_target_count[n] = len(hits)

    return results


# ---------------------------------------------------------------------------
# Donor template construction
# ---------------------------------------------------------------------------

def design_donor_template(
    genome_seq: str,
    cut_position: int,
    insert_seq: str,
    arm_length: int = 500,
    guide: GuideRNA | None = None,
    introduce_silent_mutations: bool = True,
) -> DonorTemplate:
    """
    Build an HDR donor template.

    cut_position : 0-based position of the DSB on the + strand.
                   For SpCas9 this is 3 nt upstream of PAM end.
    arm_length   : homology arm length in bp (500 bp for plasmid donors,
                   60-120 bp for ssODN).
    guide        : if provided and introduce_silent_mutations=True,
                   mutate the PAM-proximal seed to prevent re-cutting.
    """
    genome = genome_seq.upper()
    left_start = max(0, cut_position - arm_length)
    right_end = min(len(genome), cut_position + arm_length)

    left_arm = genome[left_start:cut_position]
    right_arm = genome[cut_position:right_end]

    silent_muts: list[tuple[int, str, str]] = []

    if guide is not None and introduce_silent_mutations:
        # Introduce silent (synonymous) mutation at the PAM-proximal seed region
        # to prevent Cas9 from re-cutting the edited allele.
        # Strategy: mutate position 20 (last nt before PAM) if it falls in the donor.
        seed_pos_in_genome = guide.end - 1  # last nt of protospacer, 0-based
        # Map to position within the donor full sequence
        donor_offset = left_start  # donor starts here on the genome
        seed_in_donor = seed_pos_in_genome - donor_offset

        if 0 <= seed_in_donor < len(left_arm) + len(insert_seq) + len(right_arm):
            # Determine which arm / insert the seed falls in
            if seed_in_donor < len(left_arm):
                seg = "left"
                seg_idx = seed_in_donor
                arm_list = list(left_arm)
            elif seed_in_donor < len(left_arm) + len(insert_seq):
                seg = "insert"
                seg_idx = seed_in_donor - len(left_arm)
                arm_list = None  # don't mutate the insert
            else:
                seg = "right"
                seg_idx = seed_in_donor - len(left_arm) - len(insert_seq)
                arm_list = list(right_arm)

            if seg != "insert" and arm_list is not None:
                original = arm_list[seg_idx]
                # Pick a transition that preserves GC category when possible
                transitions = {"A": "G", "G": "A", "C": "T", "T": "C"}
                mutant = transitions.get(original, original)
                arm_list[seg_idx] = mutant
                donor_abs_pos = seed_in_donor
                silent_muts.append((donor_abs_pos, original, mutant))
                if seg == "left":
                    left_arm = "".join(arm_list)
                else:
                    right_arm = "".join(arm_list)

    return DonorTemplate(
        insert=insert_seq,
        left_homology=left_arm,
        right_homology=right_arm,
        silent_mutations=silent_muts,
    )


# ---------------------------------------------------------------------------
# Tm calculation (nearest-neighbor, SantaLucia 1998)
# ---------------------------------------------------------------------------

NN_PARAMS: dict[str, tuple[float, float]] = {
    # dinucleotide: (ΔH kcal/mol, ΔS cal/mol/K)  — unified SantaLucia 1998
    "AA": (-7.9,  -22.2), "AT": (-7.2,  -20.4), "AC": (-7.8,  -21.0), "AG": (-7.8,  -21.0),
    "TA": (-7.2,  -21.3), "TT": (-7.9,  -22.2), "TC": (-8.2,  -22.2), "TG": (-8.5,  -22.7),
    "CA": (-8.5,  -22.7), "CT": (-7.8,  -21.0), "CC": (-8.0,  -19.9), "CG": (-10.6, -27.2),
    "GA": (-8.2,  -22.2), "GT": (-8.4,  -22.4), "GC": (-9.8,  -24.4), "GG": (-8.0,  -19.9),
}
H_INIT =  0.2   # kcal/mol
S_INIT = -5.7   # cal/mol/K
R      =  1.987 # cal/mol/K

def melting_temp(
    seq: str,
    conc_primer: float = 250e-9,  # 250 nM
    conc_na: float = 50e-3,        # 50 mM Na+
) -> float:
    """
    SantaLucia 1998 nearest-neighbor Tm with salt correction.
    Returns Tm in °C.
    """
    s = seq.upper()
    n = len(s)
    if n < 4:
        # Wallace rule fallback for short oligos
        return 2 * (s.count("A") + s.count("T")) + 4 * (s.count("G") + s.count("C")) - 7.0

    dH = H_INIT * 1000  # convert to cal
    dS = S_INIT
    for i in range(n - 1):
        di = s[i:i+2]
        h, sv = NN_PARAMS.get(di, (-8.0, -21.0))
        dH += h * 1000
        dS += sv

    # Symmetry correction (non-self-complementary)
    dS += R * math.log(1.0)  # = 0 for non-palindrome

    # Duplex Tm
    tm_k = dH / (dS + R * math.log(conc_primer / 4.0))
    # Salt correction (Owczarzy 2004 simplified)
    tm_k_corrected = 1.0 / (1.0 / tm_k + (4.29 * gc_content(s) - 3.95) * 1e-5 * math.log(conc_na) + 9.40e-6 * (math.log(conc_na)) ** 2)
    return tm_k_corrected - 273.15


# ---------------------------------------------------------------------------
# Genotyping primer design
# ---------------------------------------------------------------------------

def _pick_primer(
    seq: str,
    target_tm: float = 60.0,
    min_len: int = 18,
    max_len: int = 28,
) -> tuple[str, float] | None:
    """
    Greedy Tm-targeting primer picker.
    Returns (primer_sequence, tm) or None if no good candidate found.
    """
    best: tuple[str, float] | None = None
    best_delta = 999.0

    for length in range(min_len, max_len + 1):
        if length > len(seq):
            break
        candidate = seq[:length]
        gc = gc_content(candidate)
        if gc < 0.35 or gc > 0.70:
            continue
        if has_poly_n(candidate, 4):
            continue
        tm = melting_temp(candidate)
        delta = abs(tm - target_tm)
        if delta < best_delta:
            best_delta = delta
            best = (candidate, tm)

    return best


def design_genotyping_primers(
    genome_seq: str,
    cut_position: int,
    donor: DonorTemplate,
    flank_distance: int = 300,
    target_tm: float = 60.0,
) -> list[GenotypingPrimer]:
    """
    Design four primers:
      1. junction_fwd  — outside left arm, forward
      2. junction_rev  — inside insert, reverse  (junction PCR)
      3. wt_check_fwd  — outside left arm, forward  (WT band PCR)
      4. wt_check_rev  — outside right arm, reverse (WT band spans cut site)
    """
    genome = genome_seq.upper()
    primers: list[GenotypingPrimer] = []

    # Region upstream of left homology arm
    upstream_start = max(0, cut_position - len(donor.left_homology) - flank_distance)
    upstream_seq   = genome[upstream_start: cut_position - len(donor.left_homology)]

    # Region downstream of right homology arm
    downstream_start = cut_position + len(donor.right_homology)
    downstream_seq   = genome[downstream_start: downstream_start + flank_distance]

    # 1. Junction forward (binds upstream of left arm)
    res = _pick_primer(upstream_seq[-50:], target_tm=target_tm)
    if res:
        p_seq, tm = res
        amplicon = len(donor.left_homology) + len(donor.insert) // 2 + flank_distance // 2
        primers.append(GenotypingPrimer(
            name="JXN_FWD",
            sequence=p_seq,
            tm=tm,
            gc=gc_content(p_seq),
            amplicon_size=amplicon,
            purpose="junction_fwd",
        ))

    # 2. Junction reverse (reverse complement, binds insert)
    insert_rc = reverse_complement(donor.insert)
    res = _pick_primer(insert_rc[:50], target_tm=target_tm)
    if res:
        p_seq, tm = res
        amplicon = len(donor.left_homology) + len(donor.insert) // 2 + flank_distance // 2
        primers.append(GenotypingPrimer(
            name="JXN_REV",
            sequence=p_seq,
            tm=tm,
            gc=gc_content(p_seq),
            amplicon_size=amplicon,
            purpose="junction_rev",
        ))

    # 3. WT check forward (same upstream region)
    res = _pick_primer(upstream_seq[-50:], target_tm=target_tm)
    if res:
        p_seq, tm = res
        amplicon = 2 * flank_distance
        primers.append(GenotypingPrimer(
            name="WT_FWD",
            sequence=p_seq,
            tm=tm,
            gc=gc_content(p_seq),
            amplicon_size=amplicon,
            purpose="wt_check_fwd",
        ))

    # 4. WT check reverse (downstream, reverse complement)
    res = _pick_primer(reverse_complement(downstream_seq[:50]), target_tm=target_tm)
    if res:
        p_seq, tm = res
        amplicon = 2 * flank_distance
        primers.append(GenotypingPrimer(
            name="WT_REV",
            sequence=p_seq,
            tm=tm,
            gc=gc_content(p_seq),
            amplicon_size=amplicon,
            purpose="wt_check_rev",
        ))

    return primers


# ---------------------------------------------------------------------------
# Cut-site calculation
# ---------------------------------------------------------------------------

def cas9_cut_position(guide: GuideRNA, nuclease: str = "SpCas9") -> int:
    """
    Return 0-based DSB position on the + strand.
    SpCas9: cuts between nt 3 and 4 upstream of PAM (blunt cut).
    """
    if nuclease == "SpCas9":
        if guide.strand == "+":
            return guide.end - 3  # guide.end is exclusive, pam starts there
        else:
            return guide.start + 3
    elif nuclease == "SaCas9":
        if guide.strand == "+":
            return guide.end - 3
        else:
            return guide.start + 3
    elif nuclease == "Cas12a":
        # Cas12a: staggered cut, +18/+23 from PAM-distal end
        if guide.strand == "+":
            return guide.start + 18
        else:
            return guide.end - 18
    else:
        raise ValueError(f"Unknown nuclease: {nuclease}")


# ---------------------------------------------------------------------------
# End-to-end design entry point
# ---------------------------------------------------------------------------

def design_knockin_experiment(
    genome_seq: str,
    insert_seq: str,
    target_locus_name: str = "locus",
    nuclease: str = "SpCas9",
    arm_length: int = 500,
    top_n_guides: int = 3,
    max_off_target_mismatches: int = 3,
    screen_off_targets_flag: bool = True,
    flank_distance: int = 300,
    target_tm: float = 60.0,
) -> list[KnockinDesign]:
    """
    Full knock-in design pipeline.

    Parameters
    ----------
    genome_seq               : Reference sequence around the target locus.
                               Should be ≥ 2 × arm_length + insert_length + 200 bp.
    insert_seq               : Sequence to knock in at the DSB.
    target_locus_name        : Human-readable locus identifier.
    nuclease                 : 'SpCas9', 'SaCas9', or 'Cas12a'.
    arm_length               : Homology arm length in bp.
    top_n_guides             : Number of top-scoring guides to design donors for.
    max_off_target_mismatches: Hamming distance for off-target scan.
    screen_off_targets_flag  : If True, run off-target screening (slow on long seqs).
    flank_distance           : Distance outside arms for genotyping primers.
    target_tm                : Target Tm for genotyping primers (°C).

    Returns
    -------
    List of KnockinDesign objects, one per top guide, ranked by guide score.
    """
    if len(genome_seq) < arm_length * 2 + 100:
        raise ValueError(
            f"genome_seq is too short ({len(genome_seq)} bp). "
            f"Provide at least {arm_length * 2 + 100} bp around the target."
        )

    # 1. Find and score all candidate guides
    guides = find_pam_sites(genome_seq, nuclease=nuclease)
    if not guides:
        raise RuntimeError(f"No PAM sites found for {nuclease} in the provided sequence.")

    guides = rank_guides(guides)

    # 2. Filter guides that cut within the useful central window
    center = len(genome_seq) // 2
    window = arm_length
    guides = [
        g for g in guides
        if abs(cas9_cut_position(g, nuclease) - center) < window
    ]
    if not guides:
        raise RuntimeError(
            "No guides found within the central window. "
            "Provide a longer genomic sequence or adjust arm_length."
        )

    designs: list[KnockinDesign] = []

    for guide in guides[:top_n_guides]:
        cut_pos = cas9_cut_position(guide, nuclease)
        notes: list[str] = []

        # 3. Off-target screening
        if screen_off_targets_flag:
            screen_off_targets(guide, genome_seq, max_mismatches=max_off_target_mismatches)
            for mm, count in guide.off_target_count.items():
                if mm == 0 and count > 1:
                    notes.append(f"WARNING: {count} perfect-match off-targets detected.")
                elif mm <= 2 and count > 5:
                    notes.append(f"NOTE: {count} off-targets at {mm} mismatches — consider alternate guide.")

        # 4. Donor template
        donor = design_donor_template(
            genome_seq=genome_seq,
            cut_position=cut_pos,
            insert_seq=insert_seq,
            arm_length=arm_length,
            guide=guide,
            introduce_silent_mutations=True,
        )
        if donor.silent_mutations:
            notes.append(
                f"Silent mutation(s) introduced in donor at position(s) "
                f"{[m[0] for m in donor.silent_mutations]} to prevent re-cutting."
            )

        # ssODN recommendation
        if len(insert_seq) <= 50:
            notes.append(
                "Insert ≤ 50 bp: ssODN delivery (60-80 nt arms) is preferred for efficiency. "
                "Adjust arm_length=80 for ssODN mode."
            )
        else:
            notes.append(
                "Insert > 50 bp: use a plasmid or dsDNA donor with ≥ 400 bp arms. "
                "Confirm arm_length ≥ 400 bp."
            )

        # 5. Genotyping primers
        geno_primers = design_genotyping_primers(
            genome_seq=genome_seq,
            cut_position=cut_pos,
            donor=donor,
            flank_distance=flank_distance,
            target_tm=target_tm,
        )
        if not geno_primers:
            notes.append("Could not design genotyping primers — check flank_distance and sequence GC content.")

        # 6. GC / efficiency notes
        gc = gc_content(guide.sequence)
        if gc < 0.40:
            notes.append(f"Low guide GC ({gc:.0%}) — may reduce Cas9 activity.")
        if gc > 0.70:
            notes.append(f"High guide GC ({gc:.0%}) — may increase off-target activity.")

        designs.append(KnockinDesign(
            target_locus=target_locus_name,
            cut_position=cut_pos,
            guide=guide,
            donor=donor,
            genotyping_primers=geno_primers,
            notes=notes,
        ))

    return designs


# ---------------------------------------------------------------------------
# Example usage
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    # Synthetic 2000 bp genomic region (replace with real sequence in practice)
    import random
    random.seed(42)
    GENOME = "".join(random.choices("ACGT", k=2000))

    # Insert a Cas9 PAM site near center for demonstration
    center = 1000
    GENOME = GENOME[:center - 23] + "GGCCCGATCGATCGATCGATCGG" + GENOME[center:]  # NGG PAM embedded

    # Insert sequence (e.g., GFP fragment, epitope tag, loxP site)
    INSERT = "ATGGTGAGCAAGGGCGAGGAGCTGTTCACCGGGGTGGTGCCCATCCTGGTCGAGCTGGAC"  # first 60 nt of EGFP

    designs = design_knockin_experiment(
        genome_seq=GENOME,
        insert_seq=INSERT,
        target_locus_name="DEMO_LOCUS",
        nuclease="SpCas9",
        arm_length=200,
        top_n_guides=3,
        screen_off_targets_flag=True,
        flank_distance=150,
        target_tm=60.0,
    )

    for d in designs:
        print(d.summary())
        print()
