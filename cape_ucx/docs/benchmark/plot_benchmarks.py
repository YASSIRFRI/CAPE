#!/usr/bin/env python3
"""Benchmark plotting script for cape/dickpt/mpi performance data."""

import os
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
from pathlib import Path

BENCH_DIR = Path(__file__).parent
OUT_DIR = BENCH_DIR / "plots"
OUT_DIR.mkdir(exist_ok=True)

IMPL_COLORS = {"mpi": "#1f77b4", "cape": "#ff7f0e", "dickpt": "#2ca02c"}
IMPL_MARKERS = {"mpi": "o", "cape": "s", "dickpt": "^"}

# ── load & clean ──────────────────────────────────────────────────────────────

def load_data() -> pd.DataFrame:
    frames = []
    for f in BENCH_DIR.glob("*.csv"):
        df = pd.read_csv(f)
        frames.append(df)
    df = pd.concat(frames, ignore_index=True)

    # normalise app names: mamult == mul_manual
    df["app"] = df["app"].replace("mamult", "mul_manual")

    # create a human-readable problem label  n=3000 or n=3000,d=256
    def problem_label(row):
        if pd.notna(row["d"]) and row["d"] != "":
            return f"n={int(row['n'])}, d={int(row['d'])}"
        return f"n={int(row['n'])}"

    df["problem"] = df.apply(problem_label, axis=1)
    return df


# def remove_outliers(df: pd.DataFrame) -> pd.DataFrame:
#     """Drop per-group outliers using IQR fencing (1.5×IQR)."""
#     groups = ["impl", "app", "n", "nodes"]
#     cleaned = []
#     for _, g in df.groupby(groups, dropna=False):
#         if len(g) < 3:
#             cleaned.append(g)
#             continue
#         q1, q3 = g["app_ms"].quantile([0.25, 0.75])
#         iqr = q3 - q1
#         lo, hi = q1 - 1.5 * iqr, q3 + 1.5 * iqr
#         mask = g["app_ms"].between(lo, hi)
#         # keep at least 2 points even if all are outliers
#         cleaned.append(g[mask] if mask.sum() >= 2 else g)
#     return pd.concat(cleaned, ignore_index=True)


def aggregate(df: pd.DataFrame) -> pd.DataFrame:
    grp = df.groupby(["impl", "app", "n", "d", "nodes", "problem"], dropna=False)
    agg = grp["app_ms"].agg(mean="mean", std="std", median="median").reset_index()
    agg["std"] = agg["std"].fillna(0)
    return agg


# ── helpers ───────────────────────────────────────────────────────────────────

def savefig(fig, name: str):
    path = OUT_DIR / f"{name}.png"
    fig.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  saved → {path.relative_to(BENCH_DIR)}")


def impls_for(agg: pd.DataFrame, app: str, prob: str) -> list[str]:
    sub = agg[(agg["app"] == app) & (agg["problem"] == prob)]
    return sorted(sub["impl"].unique(), key=lambda x: list(IMPL_COLORS).index(x) if x in IMPL_COLORS else 99)


# ── plot 1 : execution time vs nodes ─────────────────────────────────────────

def plot_exec_time(agg: pd.DataFrame):
    for app in agg["app"].unique():
        app_df = agg[agg["app"] == app]
        problems = sorted(app_df["problem"].unique())
        ncols = min(len(problems), 3)
        nrows = (len(problems) + ncols - 1) // ncols
        fig, axes = plt.subplots(nrows, ncols, figsize=(6 * ncols, 4.5 * nrows), squeeze=False)
        fig.suptitle(f"Execution Time — {app}", fontsize=14, fontweight="bold", y=1.01)

        for idx, prob in enumerate(problems):
            ax = axes[idx // ncols][idx % ncols]
            prob_df = app_df[app_df["problem"] == prob]
            for impl in impls_for(agg, app, prob):
                idf = prob_df[prob_df["impl"] == impl].sort_values("nodes")
                ax.errorbar(
                    idf["nodes"], idf["mean"] / 1000,
                    yerr=idf["std"] / 1000,
                    label=impl, color=IMPL_COLORS.get(impl),
                    marker=IMPL_MARKERS.get(impl, "o"),
                    linewidth=1.8, markersize=6, capsize=4,
                )
            ax.set_title(prob)
            ax.set_xlabel("Nodes")
            ax.set_ylabel("Time (s)")
            ax.xaxis.set_major_locator(ticker.MaxNLocator(integer=True))
            ax.grid(True, linestyle="--", alpha=0.4)
            ax.legend(fontsize=8)

        # hide spare axes
        for j in range(len(problems), nrows * ncols):
            axes[j // ncols][j % ncols].set_visible(False)

        fig.tight_layout()
        savefig(fig, f"exec_time_{app}")


# ── plot 2 : speedup vs nodes ─────────────────────────────────────────────────

def plot_speedup(agg: pd.DataFrame):
    base_nodes = agg["nodes"].min()

    for app in agg["app"].unique():
        app_df = agg[agg["app"] == app]
        problems = sorted(app_df["problem"].unique())
        ncols = min(len(problems), 3)
        nrows = (len(problems) + ncols - 1) // ncols
        fig, axes = plt.subplots(nrows, ncols, figsize=(6 * ncols, 4.5 * nrows), squeeze=False)
        fig.suptitle(f"Speedup (relative to {base_nodes} nodes) — {app}",
                     fontsize=14, fontweight="bold", y=1.01)

        all_nodes = sorted(agg["nodes"].unique())

        for idx, prob in enumerate(problems):
            ax = axes[idx // ncols][idx % ncols]
            prob_df = app_df[app_df["problem"] == prob]

            # ideal speedup line
            ax.plot(all_nodes, [n / base_nodes for n in all_nodes],
                    "k--", linewidth=1, label="ideal", alpha=0.5)

            for impl in impls_for(agg, app, prob):
                idf = prob_df[prob_df["impl"] == impl].sort_values("nodes")
                base_row = idf[idf["nodes"] == base_nodes]
                if base_row.empty or base_row["mean"].iloc[0] == 0:
                    continue
                base_time = base_row["mean"].iloc[0]
                speedup = base_time / idf["mean"]
                ax.plot(
                    idf["nodes"], speedup,
                    label=impl, color=IMPL_COLORS.get(impl),
                    marker=IMPL_MARKERS.get(impl, "o"),
                    linewidth=1.8, markersize=6,
                )

            ax.set_title(prob)
            ax.set_xlabel("Nodes")
            ax.set_ylabel("Speedup")
            ax.xaxis.set_major_locator(ticker.MaxNLocator(integer=True))
            ax.grid(True, linestyle="--", alpha=0.4)
            ax.legend(fontsize=8)

        for j in range(len(problems), nrows * ncols):
            axes[j // ncols][j % ncols].set_visible(False)

        fig.tight_layout()
        savefig(fig, f"speedup_{app}")


# ── plot 3 : parallel efficiency vs nodes ────────────────────────────────────

def plot_efficiency(agg: pd.DataFrame):
    base_nodes = agg["nodes"].min()

    for app in agg["app"].unique():
        app_df = agg[agg["app"] == app]
        problems = sorted(app_df["problem"].unique())
        ncols = min(len(problems), 3)
        nrows = (len(problems) + ncols - 1) // ncols
        fig, axes = plt.subplots(nrows, ncols, figsize=(6 * ncols, 4.5 * nrows), squeeze=False)
        fig.suptitle(f"Parallel Efficiency — {app}", fontsize=14, fontweight="bold", y=1.01)

        for idx, prob in enumerate(problems):
            ax = axes[idx // ncols][idx % ncols]
            prob_df = app_df[app_df["problem"] == prob]

            ax.axhline(1.0, color="k", linestyle="--", linewidth=1, alpha=0.5, label="ideal")

            for impl in impls_for(agg, app, prob):
                idf = prob_df[prob_df["impl"] == impl].sort_values("nodes")
                base_row = idf[idf["nodes"] == base_nodes]
                if base_row.empty or base_row["mean"].iloc[0] == 0:
                    continue
                base_time = base_row["mean"].iloc[0]
                efficiency = (base_time / idf["mean"]) / (idf["nodes"] / base_nodes)
                ax.plot(
                    idf["nodes"], efficiency,
                    label=impl, color=IMPL_COLORS.get(impl),
                    marker=IMPL_MARKERS.get(impl, "o"),
                    linewidth=1.8, markersize=6,
                )

            ax.set_title(prob)
            ax.set_xlabel("Nodes")
            ax.set_ylabel("Efficiency")
            ax.xaxis.set_major_locator(ticker.MaxNLocator(integer=True))
            ax.set_ylim(0, 1.4)
            ax.grid(True, linestyle="--", alpha=0.4)
            ax.legend(fontsize=8)

        for j in range(len(problems), nrows * ncols):
            axes[j // ncols][j % ncols].set_visible(False)

        fig.tight_layout()
        savefig(fig, f"efficiency_{app}")


# ── plot 4 : impl comparison bar chart at fixed node counts ──────────────────

def plot_bar_comparison(agg: pd.DataFrame):
    for nodes in sorted(agg["nodes"].unique()):
        node_df = agg[agg["nodes"] == nodes]
        apps = sorted(node_df["app"].unique())
        problems = sorted(node_df["problem"].unique())

        fig, ax = plt.subplots(figsize=(max(8, len(problems) * 2), 5))
        impls = sorted(agg["impl"].unique(), key=lambda x: list(IMPL_COLORS).index(x) if x in IMPL_COLORS else 99)
        x = np.arange(len(problems))
        width = 0.8 / len(impls)

        for i, impl in enumerate(impls):
            impl_df = node_df[node_df["impl"] == impl]
            means, stds = [], []
            for prob in problems:
                row = impl_df[impl_df["problem"] == prob]
                means.append(row["mean"].iloc[0] / 1000 if not row.empty else 0)
                stds.append(row["std"].iloc[0] / 1000 if not row.empty else 0)
            offset = (i - len(impls) / 2 + 0.5) * width
            bars = ax.bar(x + offset, means, width, yerr=stds,
                          label=impl, color=IMPL_COLORS.get(impl), capsize=3, alpha=0.85)

        ax.set_xticks(x)
        ax.set_xticklabels(problems, rotation=20, ha="right", fontsize=8)
        ax.set_ylabel("Time (s)")
        ax.set_title(f"Execution Time Comparison — {nodes} nodes", fontweight="bold")
        ax.legend()
        ax.grid(True, axis="y", linestyle="--", alpha=0.4)
        fig.tight_layout()
        savefig(fig, f"bar_comparison_nodes{nodes}")


# ── plot 5 : scaling summary heatmap (speedup table) ─────────────────────────

def plot_speedup_heatmap(agg: pd.DataFrame):
    base_nodes = agg["nodes"].min()
    nodes_list = sorted(agg["nodes"].unique())
    impls = sorted(agg["impl"].unique(), key=lambda x: list(IMPL_COLORS).index(x) if x in IMPL_COLORS else 99)

    for impl in impls:
        impl_df = agg[agg["impl"] == impl]
        problems = sorted(impl_df["problem"].unique())
        matrix = np.full((len(problems), len(nodes_list)), np.nan)

        for pi, prob in enumerate(problems):
            prob_df = impl_df[impl_df["problem"] == prob]
            base_row = prob_df[prob_df["nodes"] == base_nodes]
            if base_row.empty or base_row["mean"].iloc[0] == 0:
                continue
            base_time = base_row["mean"].iloc[0]
            for ni, n in enumerate(nodes_list):
                row = prob_df[prob_df["nodes"] == n]
                if not row.empty:
                    matrix[pi, ni] = base_time / row["mean"].iloc[0]

        fig, ax = plt.subplots(figsize=(max(5, len(nodes_list) * 1.5), max(4, len(problems) * 0.6)))
        im = ax.imshow(matrix, aspect="auto", cmap="RdYlGn", vmin=0.5, vmax=len(nodes_list))
        plt.colorbar(im, ax=ax, label="Speedup")

        for (pi, ni), val in np.ndenumerate(matrix):
            if not np.isnan(val):
                ax.text(ni, pi, f"{val:.2f}", ha="center", va="center", fontsize=8,
                        color="black" if 0.8 < val < len(nodes_list) * 0.9 else "white")

        ax.set_xticks(range(len(nodes_list)))
        ax.set_xticklabels([str(n) for n in nodes_list])
        ax.set_yticks(range(len(problems)))
        ax.set_yticklabels(problems, fontsize=8)
        ax.set_xlabel("Nodes")
        ax.set_title(f"Speedup Heatmap — {impl}", fontweight="bold")
        fig.tight_layout()
        savefig(fig, f"speedup_heatmap_{impl}")


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    print("Loading data...")
    df = load_data()
    print(f"  {len(df)} raw rows, impls: {df['impl'].unique()}, apps: {df['app'].unique()}")

    print("Removing outliers...")
    df_clean = df
    dropped = len(df) - len(df_clean)
    print(f"  dropped {dropped} outlier rows")

    print("Aggregating...")
    agg = aggregate(df_clean)

    print("Generating plots...")
    plot_exec_time(agg)
    plot_speedup(agg)
    plot_efficiency(agg)
    plot_bar_comparison(agg)
    plot_speedup_heatmap(agg)

    print(f"\nDone. All plots saved to: {OUT_DIR}")


if __name__ == "__main__":
    main()
