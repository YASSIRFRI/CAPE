#!/usr/bin/env python3
"""Plot DICKPT Heat3D and Matmul thread scaling from benchmark measurements."""

from pathlib import Path
import csv

import matplotlib.pyplot as plt
import numpy as np


OUT = Path(__file__).resolve().parent

# Heat3D columns supplied by the benchmark:
# threads, app_ms, sweep_ms, writeback_ms, ckpt_ms
HEAT = np.array(
    [
        [1, 54776, 4233, 18473, 32036],
        [2, 48486, 2477, 13839, 32135],
        [4, 42299, 1304, 8456, 32506],
        [8, 39183, 826, 6075, 32249],
        [16, 38501, 801, 5368, 32300],
        [32, 38765, 744, 5561, 32427],
    ],
    dtype=float,
)

# Matmul columns: threads -> repetitions of (app_ms, compute_ms, ckpt_ms).
# Medians are used below because the first repetition at p>1 is visibly a
# warm-up/outlier relative to the remaining repetitions.
MATMUL = {
    1: [
        (520, 405, 114), (521, 406, 114), (519, 403, 115),
        (522, 397, 124), (521, 397, 123), (521, 397, 123),
        (520, 397, 123), (520, 396, 123), (521, 399, 120),
        (520, 403, 116),
    ],
    2: [
        (352, 240, 111), (295, 230, 64), (293, 220, 71),
        (294, 230, 63), (294, 226, 66), (295, 230, 63),
        (295, 220, 74), (293, 229, 64), (290, 226, 64),
        (291, 227, 64),
    ],
    4: [
        (201, 116, 85), (287, 114, 172), (285, 110, 174),
        (279, 114, 164), (283, 109, 173), (281, 109, 172),
        (283, 109, 174), (282, 112, 168), (285, 112, 172),
        (283, 112, 170),
    ],
    8: [
        (146, 60, 85), (214, 58, 156), (214, 57, 157),
        (213, 57, 155), (216, 58, 157), (215, 57, 157),
    ],
}


def amdahl_speedup(p, serial_fraction):
    return 1.0 / (serial_fraction + (1.0 - serial_fraction) / p)


def fit_amdahl(p, measured_speedup):
    """Least-squares one-parameter Amdahl fit, anchored at S(1)=1."""
    grid = np.linspace(0.0, 1.0, 1_000_001)
    modeled = 1.0 / (
        grid[:, None] + (1.0 - grid[:, None]) / p[None, :]
    )
    error = np.sum((modeled - measured_speedup[None, :]) ** 2, axis=1)
    return float(grid[np.argmin(error)])


def configure_style():
    plt.rcParams.update(
        {
            "figure.dpi": 130,
            "savefig.dpi": 300,
            "font.size": 10,
            "axes.titlesize": 12,
            "axes.labelsize": 11,
            "axes.grid": True,
            "grid.alpha": 0.25,
            "grid.linestyle": "--",
            "legend.frameon": False,
            "lines.linewidth": 2.0,
            "lines.markersize": 6,
        }
    )


def save_figure(fig, stem):
    fig.savefig(OUT / f"{stem}.png", bbox_inches="tight")
    fig.savefig(OUT / f"{stem}.pdf", bbox_inches="tight")
    plt.close(fig)


def main():
    configure_style()

    hp = HEAT[:, 0]
    happ, hsweep, hwrite, hckpt = HEAT[:, 1:].T
    hspeed = happ[0] / happ
    hcompute = hsweep + hwrite
    hcompute_speed = hcompute[0] / hcompute
    hf = fit_amdahl(hp, hspeed)

    mp = np.array(sorted(MATMUL), dtype=float)
    mmed = np.array(
        [np.median(np.asarray(MATMUL[int(p)]), axis=0) for p in mp]
    )
    mapp, mcompute, mckpt = mmed.T
    mspeed = mapp[0] / mapp
    mcompute_speed = mcompute[0] / mcompute
    mf = fit_amdahl(mp, mspeed)

    # 1. Measured speedups and Amdahl fits.
    fig, ax = plt.subplots(figsize=(8.2, 5.0))
    dense_p = np.geomspace(1, 32, 300)
    ax.plot(dense_p, dense_p, color="0.70", linestyle=":", label="Ideal")
    ax.plot(hp, hspeed, "o-", color="#0072B2", label="Heat3D total")
    ax.plot(
        hp, hcompute_speed, "o--", color="#56B4E9",
        label="Heat3D compute (sweep + write-back)",
    )
    ax.plot(
        dense_p, amdahl_speedup(dense_p, hf), color="#0072B2",
        linestyle="-.", label=f"Heat3D Amdahl fit ($f_s$={hf:.3f})",
    )
    ax.plot(mp, mspeed, "s-", color="#D55E00", label="Matmul total (median)")
    ax.plot(
        mp, mcompute_speed, "s--", color="#E69F00",
        label="Matmul compute (median)",
    )
    ax.plot(
        dense_p, amdahl_speedup(dense_p, mf), color="#D55E00",
        linestyle="-.", label=f"Matmul Amdahl fit ($f_s$={mf:.3f})",
    )
    ax.set_xscale("log", base=2)
    ax.set_xticks([1, 2, 4, 8, 16, 32])
    ax.get_xaxis().set_major_formatter(plt.ScalarFormatter())
    ax.set_xlim(0.9, 35)
    ax.set_ylim(0.8, 9.0)
    ax.set_xlabel("Threads per DICKPT rank")
    ax.set_ylabel("Speedup over 1 thread")
    ax.set_title("DICKPT thread scaling (16 ranks)")
    ax.legend(ncol=2, fontsize=8.5)
    save_figure(fig, "speedup_comparison")

    # 2. Runtime component breakdown. Keep milliseconds for Matmul and seconds
    # for Heat3D so both panels remain legible.
    fig, axes = plt.subplots(1, 2, figsize=(10.5, 4.6))
    xh = np.arange(len(hp))
    hother = np.maximum(happ - hsweep - hwrite - hckpt, 0)
    axes[0].bar(xh, hsweep / 1000, label="Sweep", color="#56B4E9")
    axes[0].bar(
        xh, hwrite / 1000, bottom=hsweep / 1000,
        label="Write-back", color="#009E73",
    )
    axes[0].bar(
        xh, hckpt / 1000, bottom=(hsweep + hwrite) / 1000,
        label="Checkpoint", color="#D55E00",
    )
    axes[0].bar(
        xh, hother / 1000, bottom=(hsweep + hwrite + hckpt) / 1000,
        label="Other", color="0.65",
    )
    axes[0].set_xticks(xh, hp.astype(int))
    axes[0].set_xlabel("Threads")
    axes[0].set_ylabel("Runtime (s)")
    axes[0].set_title("Heat3D 512³ × 200")
    axes[0].legend(fontsize=8)

    xm = np.arange(len(mp))
    mother = np.maximum(mapp - mcompute - mckpt, 0)
    axes[1].bar(xm, mcompute, label="Compute", color="#56B4E9")
    axes[1].bar(
        xm, mckpt, bottom=mcompute, label="Checkpoint", color="#D55E00",
    )
    axes[1].bar(
        xm, mother, bottom=mcompute + mckpt, label="Other", color="0.65",
    )
    axes[1].set_xticks(xm, mp.astype(int))
    axes[1].set_xlabel("Threads")
    axes[1].set_ylabel("Median runtime (ms)")
    axes[1].set_title("Matmul 2048²")
    axes[1].legend(fontsize=8)
    fig.suptitle("DICKPT runtime composition (16 ranks)")
    fig.tight_layout()
    save_figure(fig, "runtime_breakdown")

    # 3. Fitted serial/parallel fraction model, with the directly observed
    # one-thread checkpoint share shown as a diagnostic rather than conflated
    # with the Amdahl fit.
    names = ["Heat3D", "Matmul"]
    fitted_serial = np.array([hf, mf])
    baseline_ckpt = np.array([hckpt[0] / happ[0], mckpt[0] / mapp[0]])
    x = np.arange(2)
    width = 0.34
    fig, ax = plt.subplots(figsize=(7.2, 4.6))
    ax.bar(
        x - width / 2, fitted_serial * 100, width,
        label="Fitted serial fraction", color="#CC79A7",
    )
    ax.bar(
        x + width / 2, baseline_ckpt * 100, width,
        label="1-thread checkpoint share", color="#D55E00",
    )
    for xpos, value in zip(x - width / 2, fitted_serial):
        ax.text(xpos, value * 100 + 1.2, f"{value:.1%}", ha="center")
    for xpos, value in zip(x + width / 2, baseline_ckpt):
        ax.text(xpos, value * 100 + 1.2, f"{value:.1%}", ha="center")
    ax.set_xticks(x, names)
    ax.set_ylim(0, 80)
    ax.set_ylabel("Fraction of 1-thread runtime (%)")
    ax.set_title("Amdahl serial-fraction model")
    ax.legend()
    save_figure(fig, "serial_fraction_model")

    # Machine-readable values used in the figures.
    with (OUT / "scaling_summary.csv").open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(
            [
                "app", "threads", "aggregate", "app_ms", "compute_ms",
                "ckpt_ms", "speedup", "compute_speedup",
                "amdahl_serial_fraction",
            ]
        )
        for p, app, comp, ckpt, speed, cspeed in zip(
            hp, happ, hcompute, hckpt, hspeed, hcompute_speed
        ):
            writer.writerow(
                [
                    "heat3d", int(p), "single_rep", app, comp, ckpt,
                    speed, cspeed, hf,
                ]
            )
        for p, app, comp, ckpt, speed, cspeed in zip(
            mp, mapp, mcompute, mckpt, mspeed, mcompute_speed
        ):
            writer.writerow(
                [
                    "matmul", int(p), "median", app, comp, ckpt,
                    speed, cspeed, mf,
                ]
            )

    print(f"Heat3D Amdahl serial fraction: {hf:.6f}")
    print(f"Matmul Amdahl serial fraction: {mf:.6f}")
    print(f"Wrote figures and summary to {OUT}")


if __name__ == "__main__":
    main()
