"""
language: Python 3, file: liner_formation.py
Liner jet formation — 6 aircraft in a straight line at a fixed heading.
"""

from dataclasses import dataclass, field
import math
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import matplotlib.transforms as mtransforms
import numpy as np


# ---------------------------------------------------------------------------
# Data
# ---------------------------------------------------------------------------

@dataclass
class Jet:
    x: float          # metres, East-positive
    y: float          # metres, North-positive
    heading: float    # degrees, clockwise from North
    callsign: str
    color: str = "steelblue"


# ---------------------------------------------------------------------------
# Formation builder
# ---------------------------------------------------------------------------

def build_liner_formation(
    lead_x: float = 0.0,
    lead_y: float = 0.0,
    heading_deg: float = 35.0,
    spacing_m: float = 300.0,
    count: int = 6,
    base_callsign: str = "VIPER",
) -> list[Jet]:
    """
    Returns `count` jets spaced evenly in a straight line behind the lead.
    Spacing is measured along the formation track (inverse of heading).
    """
    # Direction vector from lead back along the trail
    trail_rad = math.radians(heading_deg + 180)
    dx = math.sin(trail_rad)   # East component
    dy = math.cos(trail_rad)   # North component

    colors = plt.cm.cool(np.linspace(0.2, 0.9, count))

    jets: list[Jet] = []
    for i in range(count):
        jets.append(Jet(
            x=lead_x + dx * spacing_m * i,
            y=lead_y + dy * spacing_m * i,
            heading=heading_deg,
            callsign=f"{base_callsign}{i + 1}",
            color=colors[i],
        ))
    return jets


# ---------------------------------------------------------------------------
# Drawing helpers
# ---------------------------------------------------------------------------

def _jet_polygon(scale: float = 1.0) -> np.ndarray:
    """
    Unit jet silhouette in local frame (nose pointing up = +Y).
    Returns (N, 2) array of vertices.
    """
    s = scale
    return np.array([
        # fuselage
        [0,     1.6*s],   # nose
        [0.15*s, 0.4*s],
        [0.15*s,-0.4*s],
        [0,    -1.0*s],   # tail
        [-0.15*s,-0.4*s],
        [-0.15*s, 0.4*s],
        # wings (interleaved to close shape simply)
        [0.15*s, 0.1*s],
        [0.9*s, -0.5*s],
        [0.9*s, -0.7*s],
        [0.15*s,-0.4*s],
        [-0.15*s,-0.4*s],
        [-0.9*s,-0.7*s],
        [-0.9*s,-0.5*s],
        [-0.15*s, 0.1*s],
    ])


def draw_jet(ax: plt.Axes, jet: Jet, scale: float = 80.0) -> None:
    """Draw a single jet at its position and heading."""
    verts = _jet_polygon(scale)

    # Rotate to match heading (clockwise from North → standard trig angle)
    angle_rad = math.radians(90 - jet.heading)   # convert to CCW from East
    c, s = math.cos(angle_rad), math.sin(angle_rad)
    rot = np.array([[c, -s], [s, c]])
    rotated = (rot @ verts.T).T

    xs = rotated[:, 0] + jet.x
    ys = rotated[:, 1] + jet.y

    ax.fill(xs, ys, color=jet.color, alpha=0.9, zorder=3)
    ax.plot(xs, ys, color="white", linewidth=0.4, alpha=0.6, zorder=4)

    # Callsign label offset perpendicular-right of heading
    perp_rad = math.radians(90 - jet.heading + 90)
    lx = jet.x + math.cos(perp_rad) * scale * 1.6
    ly = jet.y + math.sin(perp_rad) * scale * 1.6
    ax.text(lx, ly, jet.callsign, color="white", fontsize=7,
            ha="center", va="center", zorder=5,
            fontfamily="monospace")


def draw_formation(
    jets: list[Jet],
    title: str = "Liner Formation",
    save_path: str | None = "liner_formation.png",
) -> plt.Figure:
    """Render the full formation on a dark-background axes."""
    fig, ax = plt.subplots(figsize=(10, 10), facecolor="#0d1117")
    ax.set_facecolor("#0d1117")

    # --- grid / atmosphere cues ---
    padding = 500
    all_x = [j.x for j in jets]
    all_y = [j.y for j in jets]
    cx = (min(all_x) + max(all_x)) / 2
    cy = (min(all_y) + max(all_y)) / 2
    span = max(max(all_x) - min(all_x), max(all_y) - min(all_y)) + padding * 2

    ax.set_xlim(cx - span / 2, cx + span / 2)
    ax.set_ylim(cy - span / 2, cy + span / 2)
    ax.set_aspect("equal")

    # subtle grid
    for gv in np.arange(cx - span / 2, cx + span / 2, 200):
        ax.axvline(gv, color="#1f2937", linewidth=0.4, zorder=0)
    for gh in np.arange(cy - span / 2, cy + span / 2, 200):
        ax.axhline(gh, color="#1f2937", linewidth=0.4, zorder=0)

    # --- formation track line ---
    lead = jets[0]
    tail = jets[-1]
    ax.plot([lead.x, tail.x], [lead.y, tail.y],
            color="#374151", linewidth=1.2, linestyle="--", zorder=1)

    # --- jets ---
    for jet in jets:
        draw_jet(ax, jet, scale=80)

    # --- heading arrow ---
    hdg_rad = math.radians(90 - lead.heading)
    arrow_len = span * 0.12
    ax.annotate(
        "", xy=(lead.x + math.cos(hdg_rad) * arrow_len,
                lead.y + math.sin(hdg_rad) * arrow_len),
        xytext=(lead.x, lead.y),
        arrowprops=dict(arrowstyle="->", color="#6ee7b7", lw=1.5),
        zorder=6,
    )

    # --- annotation: heading & spacing ---
    info = (
        f"HDG {lead.heading:03.0f}°   "
        f"SPACING {jets[1].x - jets[0].x:.0f} m (x) "
        f"/ {jets[1].y - jets[0].y:.0f} m (y)   "
        f"AIRCRAFT {len(jets)}"
    )
    ax.text(0.02, 0.97, info, transform=ax.transAxes,
            color="#9ca3af", fontsize=8, va="top", fontfamily="monospace")

    # --- axes cosmetics ---
    ax.set_xlabel("East  (m)", color="#6b7280", fontsize=9)
    ax.set_ylabel("North (m)", color="#6b7280", fontsize=9)
    ax.tick_params(colors="#4b5563", labelsize=8)
    for spine in ax.spines.values():
        spine.set_edgecolor("#1f2937")

    ax.set_title(title, color="#e5e7eb", fontsize=13, pad=12,
                 fontfamily="monospace")

    fig.tight_layout()

    if save_path:
        fig.savefig(save_path, dpi=150, bbox_inches="tight",
                    facecolor=fig.get_facecolor())
        print(f"Saved → {save_path}")

    return fig


# ---------------------------------------------------------------------------
# Simulation step (optional, extends to animated sequences)
# ---------------------------------------------------------------------------

def simulate_step(jets: list[Jet], dt: float = 1.0, speed_mps: float = 250.0) -> None:
    """
    Advance each jet one timestep along its current heading.
    Mutates in place. speed_mps: metres per second (~M 0.75 at sea level).
    """
    for jet in jets:
        rad = math.radians(jet.heading)
        jet.x += math.sin(rad) * speed_mps * dt
        jet.y += math.cos(rad) * speed_mps * dt


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    formation = build_liner_formation(
        lead_x=0,
        lead_y=0,
        heading_deg=35,
        spacing_m=300,
        count=6,
        base_callsign="VIPER",
    )

    fig = draw_formation(formation, title="VIPER FLIGHT — LINER FORMATION")
    plt.show()
