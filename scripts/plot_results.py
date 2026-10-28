#!/usr/bin/env python3
"""Generates all 5 required charts from benchmark CSV files (Section 6)."""

import os
import sys
import pandas as pd
import matplotlib.pyplot as plt


COLORS = {
    "LOCK_FREE": "#2563eb",
    "SHARDED": "#16a34a",
    "GLOBAL_LOCK": "#dc2626",
}
LABELS = {
    "LOCK_FREE": "Lock-Free (CAS SkipList)",
    "SHARDED": "Sharded (16 Buckets)",
    "GLOBAL_LOCK": "Global Lock (1 Mutex)",
}


def plot_slide3(results_dir: str) -> None:
    csv_path = os.path.join(results_dir, "slide3_distribution.csv")
    if not os.path.exists(csv_path):
        return
    df = pd.read_csv(csv_path)
    fig, ax = plt.subplots(figsize=(7, 4.5))
    idx = range(len(df))
    w = 0.35
    ax.bar([i - w / 2 for i in idx], df["zipf_measured"], width=w,
           label="Zipfian (s=1.0)", color="#2563eb")
    ax.bar([i + w / 2 for i in idx], df["uniform_measured"], width=w,
           label="Uniform (1/N)", color="#64748b")
    ax.set_xticks(list(idx))
    ax.set_xticklabels([f"Rank {r}" for r in df["rank"]])
    ax.set_ylabel("Selection Probability")
    ax.set_title("Slide 3 Reproduction: Zipfian (N=1000, s=1.0) vs. Uniform")
    ax.legend()
    ax.grid(axis="y", linestyle="--", alpha=0.5)
    fig.tight_layout()
    fig.savefig(os.path.join(results_dir, "chart5_slide3_distribution.png"), dpi=150)
    plt.close(fig)


def plot_throughput_and_latency(results_dir: str) -> None:
    csv_path = os.path.join(results_dir, "benchmark_matrix.csv")
    if not os.path.exists(csv_path):
        return
    df = pd.read_csv(csv_path)
    grouped = df.groupby(["distribution", "memtable", "threads"]).agg(
        tput_mean=("throughput_ops_sec", "mean"),
        tput_std=("throughput_ops_sec", "std"),
        p99_mean=("p99_write_us", "mean"),
        p99_std=("p99_write_us", "std"),
        comp_ratio=("compaction_io_ratio", "mean"),
    ).reset_index()

    for dist in ["UNIFORM", "ZIPFIAN"]:
        sub = grouped[grouped["distribution"] == dist]
        # Chart 1: Throughput vs Threads
        fig, ax = plt.subplots(figsize=(7.5, 4.8))
        for mt in ["LOCK_FREE", "SHARDED", "GLOBAL_LOCK"]:
            m_df = sub[sub["memtable"] == mt].sort_values("threads")
            ax.errorbar(
                m_df["threads"], m_df["tput_mean"] / 1000.0,
                yerr=m_df["tput_std"].fillna(0) / 1000.0,
                marker="o", linewidth=2, capsize=4,
                color=COLORS[mt], label=LABELS[mt]
            )
        ax.set_xlabel("Concurrent Client Threads")
        ax.set_ylabel("Throughput (K ops/sec)")
        ax.set_title(f"Write Throughput vs. Threads ({dist} Distribution)")
        ax.set_xticks([1, 2, 4, 8, 16, 32])
        ax.grid(True, linestyle="--", alpha=0.5)
        ax.legend()
        fig.tight_layout()
        fig.savefig(os.path.join(results_dir, f"chart1_throughput_{dist.lower()}.png"), dpi=150)
        plt.close(fig)

        # Chart 2: P99 Write Latency vs Threads
        fig, ax = plt.subplots(figsize=(7.5, 4.8))
        for mt in ["LOCK_FREE", "SHARDED", "GLOBAL_LOCK"]:
            m_df = sub[sub["memtable"] == mt].sort_values("threads")
            ax.errorbar(
                m_df["threads"], m_df["p99_mean"],
                yerr=m_df["p99_std"].fillna(0),
                marker="s", linewidth=2, capsize=4,
                color=COLORS[mt], label=LABELS[mt]
            )
        ax.set_xlabel("Concurrent Client Threads")
        ax.set_ylabel("P99 Write Latency (us)")
        ax.set_title(f"P99 Write Latency vs. Threads ({dist} Distribution)")
        ax.set_xticks([1, 2, 4, 8, 16, 32])
        ax.grid(True, linestyle="--", alpha=0.5)
        ax.legend()
        fig.tight_layout()
        fig.savefig(os.path.join(results_dir, f"chart2_p99_latency_{dist.lower()}.png"), dpi=150)
        plt.close(fig)

    # Chart 3: Compaction Overhead
    sub_u = grouped[(grouped["distribution"] == "UNIFORM") & (grouped["memtable"] == "LOCK_FREE")]
    fig, ax = plt.subplots(figsize=(7, 4.5))
    ax.bar(sub_u["threads"].astype(str), sub_u["comp_ratio"], color="#7c3aed")
    ax.set_xlabel("Concurrent Client Threads")
    ax.set_ylabel("Compaction I/O Bytes / Foreground Write Bytes")
    ax.set_title("Background Compaction I/O Ratio vs. Foreground Writes")
    ax.grid(axis="y", linestyle="--", alpha=0.5)
    fig.tight_layout()
    fig.savefig(os.path.join(results_dir, "chart3_compaction_overhead.png"), dpi=150)
    plt.close(fig)


def plot_read_amplification(results_dir: str) -> None:
    csv_path = os.path.join(results_dir, "read_amplification.csv")
    if not os.path.exists(csv_path):
        return
    df = pd.read_csv(csv_path)
    fig, ax = plt.subplots(figsize=(7, 4.5))
    ax.plot(df["level_count"], df["avg_files_touched_per_lookup"],
            marker="o", linewidth=2.2, color="#ea580c")
    ax.set_xlabel("Active SSTable Batches / Levels")
    ax.set_ylabel("Average SSTables Touched per Lookup")
    ax.set_title("Read Amplification vs. Level Count")
    ax.grid(True, linestyle="--", alpha=0.5)
    fig.tight_layout()
    fig.savefig(os.path.join(results_dir, "chart4_read_amplification.png"), dpi=150)
    plt.close(fig)


if __name__ == "__main__":
    res_dir = sys.argv[1] if len(sys.argv) > 1 else "results"
    plot_slide3(res_dir)
    plot_throughput_and_latency(res_dir)
    plot_read_amplification(res_dir)
    print(f"[ok] All charts generated in {res_dir}/")
