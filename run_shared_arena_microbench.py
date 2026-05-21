#!/usr/bin/env python3

import argparse
import csv
import os
import re
import shlex
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd


SCRIPT_DIR = Path(__file__).resolve().parent
RUN_BENCH_SCRIPT = SCRIPT_DIR / "run_shared_arena_latency_bench.sh"
DEFAULT_BUILD_DIR = SCRIPT_DIR.parents[2] / "build-scudo-bench-android-aarch64"
DEFAULT_BINARY = DEFAULT_BUILD_DIR / "ScudoSharedArenaLatencyBench-aarch64-Test"

SSH_BASE_ARGS = [
    "ssh",
    "-o",
    "BatchMode=yes",
    "-o",
    "StrictHostKeyChecking=accept-new",
]

SIZE_RE = re.compile(
    r"=+ size=(?P<human>.+?), threads=(?P<threads>\d+), unit=ns =+"
)
MODE_SIZE_RE = re.compile(
    r"=+ size=(?P<human>.+?), mode=(?P<mode>.+?), unit=ns =+"
)
METRIC_RE = re.compile(r"^\s*(?P<metric>[a-z_]+)\s+mean=(?P<mean>[0-9.]+)")
ROUTE_HINT_RE = re.compile(
    r"^route_hint:\s+size=(?P<size_bytes>\d+)\s+bytes\s+route=(?P<route>\w+)"
)
ROUTE_ACTUAL_RE = re.compile(
    r"^route_actual:\s+size=(?P<size_bytes>\d+)\s+bytes\s+route=(?P<route>\w+)"
)
SECONDARY_CACHE_RE = re.compile(
    r"^secondary_cache_retrieve:\s+calls=(?P<calls>\d+)\s+hits=(?P<hits>\d+)\s+hit_rate=(?P<rate>[0-9.]+)%"
)


@dataclass
class BenchRecord:
    repetition: int
    threads: int
    size_human: str
    size_bytes: int
    path: str
    metric: str
    mean_ns: float
    route_hint: str = ""
    secondary_cache_calls: int = 0
    secondary_cache_hits: int = 0
    secondary_cache_hit_rate: float = 0.0


def run(cmd, cwd=None, env=None, capture_output=False):
    return subprocess.run(
        cmd,
        cwd=cwd,
        env=env,
        text=True,
        capture_output=capture_output,
        check=True,
    )


def parse_list(text):
    return [item.strip() for item in text.split(",") if item.strip()]


def parse_sizes_bytes(sizes):
    return [int(x) for x in parse_list(sizes)]


def parse_threads(threads):
    return [int(x) for x in parse_list(threads)]

def parse_env_kv_list(text):
    env = {}
    for item in parse_list(text):
        if "=" not in item:
            continue
        k, v = item.split("=", 1)
        k = k.strip()
        v = v.strip()
        if k:
            env[k] = v
    return env


def detect_remote_adb_bin(remote_host):
    cmd = [
        *SSH_BASE_ARGS,
        remote_host,
        "bash",
        "-lc",
        "command -v adb || true; test -x /opt/homebrew/bin/adb && echo /opt/homebrew/bin/adb || true",
    ]
    result = subprocess.run(cmd, text=True, capture_output=True, check=True)
    for line in result.stdout.splitlines():
      line = line.strip()
      if line:
        return line
    raise RuntimeError("远端机器未找到 adb")


def remote_shell(remote_host, command, capture_output=False):
    cmd = [
        *SSH_BASE_ARGS,
        remote_host,
        f"bash -lc {shlex.quote(command)}",
    ]
    return run(cmd, capture_output=capture_output)


def adb_remote_cmd(remote_host, adb_bin, shell_fragment, capture_output=False, use_su=False):
    if use_su:
        shell_fragment = f"su -c {shlex.quote(shell_fragment)}"
    cmd = f"{shlex.quote(adb_bin)} shell {shlex.quote(shell_fragment)}"
    return remote_shell(remote_host, cmd, capture_output=capture_output)


def build_android_binary(ndk, extra_env):
    env = os.environ.copy()
    env.update(extra_env)
    env["MODE"] = "android"
    env["ANDROID_NDK"] = ndk
    env["ANDROID_RUN"] = "0"
    run([str(RUN_BENCH_SCRIPT), "--iterations", "1", "--warmup", "0", "--threads", "1"],
        cwd=SCRIPT_DIR,
        env=env)


def deploy_binary(binary_path, remote_host, remote_stage_dir, device_dir, adb_bin):
    remote_shell(remote_host, f"mkdir -p {shlex.quote(remote_stage_dir)}")
    run(["scp", str(binary_path), f"{remote_host}:{remote_stage_dir}/{binary_path.name}"])
    remote_shell(
        remote_host,
        f"{shlex.quote(adb_bin)} push {shlex.quote(remote_stage_dir + '/' + binary_path.name)} "
        f"{shlex.quote(device_dir + '/' + binary_path.name)} >/dev/null && "
        f"{shlex.quote(adb_bin)} shell chmod 755 {shlex.quote(device_dir + '/' + binary_path.name)}",
    )


def kill_bench_processes(remote_host, adb_bin, binary_name, use_su=False):
    if not remote_host or not adb_bin:
        return
    device_cmd = (
        f"for p in $(pidof {shlex.quote(binary_name)} 2>/dev/null); do "
        f"kill -9 $p; "
        f"done; true"
    )
    if use_su:
        device_cmd = f"su -c {shlex.quote(device_cmd)}"
    cmd = [
        *SSH_BASE_ARGS,
        remote_host,
        f"bash -lc {shlex.quote(f'{adb_bin} shell {shlex.quote(device_cmd)}')}",
    ]
    subprocess.run(cmd, text=True, capture_output=True, check=False)


def parse_human_size_to_bytes(human):
    number_text, unit = human.split()
    number = float(number_text)
    factors = {
        "B": 1,
        "KiB": 1024,
        "MiB": 1024 * 1024,
        "GiB": 1024 * 1024 * 1024,
    }
    return int(round(number * factors[unit]))


def format_size_bytes(size_bytes):
    if size_bytes >= 1024 * 1024:
        return f"{size_bytes / (1024 * 1024):.2f} MiB"
    if size_bytes >= 1024:
        return f"{size_bytes / 1024:.2f} KiB"
    return f"{size_bytes} B"


def parse_benchmark_output(output, repetition, threads):
    records = []
    current_size_human = None
    current_size_bytes = None
    current_path = None
    current_route_hint = ""
    current_trad_cache = {"calls": 0, "hits": 0, "rate": 0.0}

    for raw_line in output.splitlines():
        line = raw_line.strip()
        route_match = ROUTE_ACTUAL_RE.match(line) or ROUTE_HINT_RE.match(line)
        if route_match:
            current_route_hint = route_match.group("route")
            continue
        size_match = SIZE_RE.search(line) or MODE_SIZE_RE.search(line)
        if size_match:
            current_size_human = size_match.group("human")
            current_size_bytes = parse_human_size_to_bytes(current_size_human)
            current_path = None
            current_trad_cache = {"calls": 0, "hits": 0, "rate": 0.0}
            continue

        if line.startswith("-- delegated"):
            current_path = "delegated"
            continue
        if line.startswith("-- traditional"):
            current_path = "traditional"
            continue
        if line.startswith("-- delegated skipped"):
            raise RuntimeError("delegated 路径被跳过，SharedArena 没有 ready")

        cache_match = SECONDARY_CACHE_RE.match(line)
        if cache_match:
            current_trad_cache = {
                "calls": int(cache_match.group("calls")),
                "hits": int(cache_match.group("hits")),
                "rate": float(cache_match.group("rate")),
            }
            continue

        metric_match = METRIC_RE.match(line)
        if metric_match and current_size_human and current_path:
            cache_calls = 0
            cache_hits = 0
            cache_rate = 0.0
            if current_path == "traditional":
                cache_calls = current_trad_cache["calls"]
                cache_hits = current_trad_cache["hits"]
                cache_rate = current_trad_cache["rate"]
            records.append(
                BenchRecord(
                    repetition=repetition,
                    threads=threads,
                    size_human=current_size_human,
                    size_bytes=current_size_bytes,
                    path=current_path,
                    metric=metric_match.group("metric"),
                    mean_ns=float(metric_match.group("mean")),
                    route_hint=current_route_hint,
                    secondary_cache_calls=cache_calls,
                    secondary_cache_hits=cache_hits,
                    secondary_cache_hit_rate=cache_rate,
                )
            )

    if not records:
        raise RuntimeError("未能从 benchmark 输出解析到任何结果")
    return records


METRIC_LABELS = {
    "alloc_only": "alloc_only",
    "dealloc_only": "free_only",
}


def metric_label(metric):
    return METRIC_LABELS.get(metric, metric)


def metric_dataframe(summary_df, metric):
    df = summary_df[summary_df["metric"] == metric].copy()
    df["size_kib"] = df["size_bytes"] / 1024.0
    return df.sort_values(["threads", "path", "size_bytes"])


def plot_latency_by_thread(summary_df, metric, output_png):
    metric_df = metric_dataframe(summary_df, metric)

    plt.style.use("seaborn-v0_8-whitegrid")
    thread_values = sorted(metric_df["threads"].unique())
    fig, axes = plt.subplots(2, 2, figsize=(14, 9))
    axes = axes.flatten()

    path_styles = {
        "delegated": {"color": "#1f77b4", "marker": "o", "label": "delegated"},
        "traditional": {"color": "#d62728", "marker": "s", "label": "traditional"},
    }

    for ax, threads in zip(axes, thread_values):
        subset = metric_df[metric_df["threads"] == threads]
        path_lines = {}
        for path, style in path_styles.items():
            line = subset[subset["path"] == path].sort_values("size_bytes")
            path_lines[path] = line
            if line.empty:
                continue
            marker_primary = "^"
            marker_secondary = style["marker"]
            route_series = (
                line["route_hint"]
                if "route_hint" in line.columns
                else pd.Series([""] * len(line))
            )
            markers = [
                marker_primary if r == "primary" else marker_secondary
                for r in route_series.tolist()
            ]
            if len(set(markers)) <= 1:
                ax.plot(
                    line["size_kib"],
                    line["mean_ns"],
                    marker=markers[0] if markers else style["marker"],
                    linewidth=2,
                    color=style["color"],
                    label=style["label"],
                )
            else:
                ax.plot(
                    line["size_kib"],
                    line["mean_ns"],
                    linewidth=2,
                    color=style["color"],
                    label=style["label"],
                )
                for x, y, m in zip(line["size_kib"], line["mean_ns"], markers):
                    ax.plot([x], [y], marker=m, color=style["color"], markersize=6)
            lower = (line["mean_ns"] - line["std_ns"]).clip(lower=1.0)
            upper = line["mean_ns"] + line["std_ns"]
            ax.fill_between(
                line["size_kib"],
                lower,
                upper,
                color=style["color"],
                alpha=0.16,
            )

        delegated_line = path_lines.get("delegated")
        traditional_line = path_lines.get("traditional")
        if delegated_line is not None and traditional_line is not None:
            merged = delegated_line.merge(
                traditional_line,
                on=["threads", "size_human", "size_bytes", "size_kib"],
                suffixes=("_delegated", "_traditional"),
            )
            for _, row in merged.iterrows():
                speedup = row["mean_ns_traditional"] / row["mean_ns_delegated"]
                y_mid = (row["mean_ns_traditional"] * row["mean_ns_delegated"]) ** 0.5
                ax.annotate(
                    f"{speedup:.2f}x",
                    (row["size_kib"], y_mid),
                    textcoords="offset points",
                    xytext=(0, -10 if speedup >= 1.0 else 10),
                    ha="center",
                    va="center",
                    fontsize=8,
                    color="#222222",
                    bbox={
                        "boxstyle": "round,pad=0.2",
                        "facecolor": "white",
                        "edgecolor": "none",
                        "alpha": 0.8,
                    },
                )
        ax.set_xscale("log", base=2)
        ax.set_yscale("log", base=10)
        ax.set_title(f"{threads} threads")
        ax.set_xlabel("Allocation size (KiB)")
        ax.set_ylabel(f"{metric_label(metric)} mean latency (ns)")
        ax.grid(True, which="both", linestyle="--", alpha=0.35)
        handles, labels = ax.get_legend_handles_labels()
        if handles:
            ax.legend()

        trad = subset[subset["path"] == "traditional"]
        if not trad.empty and "secondary_cache_hit_rate" in trad.columns:
            for _, row in trad.iterrows():
                calls = row.get("secondary_cache_calls", 0)
                if not calls:
                    continue
                rate = row.get("secondary_cache_hit_rate", 0.0)
                ax.annotate(
                    f"{rate:.0f}%",
                    (row["size_kib"], row["mean_ns"]),
                    textcoords="offset points",
                    xytext=(6, 6),
                    ha="left",
                    va="bottom",
                    fontsize=7,
                    color="#444444",
                    bbox={
                        "boxstyle": "round,pad=0.15",
                        "facecolor": "white",
                        "edgecolor": "none",
                        "alpha": 0.7,
                    },
                )

    for ax in axes[len(thread_values):]:
        ax.axis("off")

    fig.suptitle(f"Scudo {metric_label(metric)} latency by thread count")
    fig.tight_layout()
    fig.savefig(output_png, dpi=180, bbox_inches="tight")


def plot_latency_small_sizes(summary_df, metric, output_png):
    metric_df = metric_dataframe(summary_df, metric)
    metric_df = metric_df[metric_df["size_bytes"] <= 512 * 1024].copy()

    plt.style.use("seaborn-v0_8-whitegrid")
    fig, axes = plt.subplots(1, 2, figsize=(14, 5), sharey=False)

    for idx, path in enumerate(["delegated", "traditional"]):
        ax = axes[idx]
        subset = metric_df[metric_df["path"] == path]
        for threads in sorted(subset["threads"].unique()):
            line = subset[subset["threads"] == threads].sort_values("size_bytes")
            ax.plot(
                line["size_kib"],
                line["mean_ns"],
                marker="o",
                linewidth=2,
                label=f"{threads} threads",
            )
        ax.set_xscale("log", base=2)
        ax.set_xlabel("Allocation size (KiB)")
        ax.set_ylabel(f"{metric_label(metric)} mean latency (ns)")
        ax.set_title(f"{path} (<= 512 KiB)")
        ax.grid(True, which="both", linestyle="--", alpha=0.35)
        ax.legend()

    fig.suptitle(f"Scudo {metric_label(metric)} latency on small/medium sizes")
    fig.tight_layout()
    fig.savefig(output_png, dpi=180, bbox_inches="tight")


def plot_speedup_heatmap(summary_df, metric, output_png):
    metric_df = summary_df[summary_df["metric"] == metric].copy()
    delegated = metric_df[metric_df["path"] == "delegated"].pivot_table(
        index="threads",
        columns=["size_bytes", "size_human"],
        values="mean_ns",
        aggfunc="first",
    )
    traditional = metric_df[metric_df["path"] == "traditional"].pivot_table(
        index="threads",
        columns=["size_bytes", "size_human"],
        values="mean_ns",
        aggfunc="first",
    )
    speedup = traditional / delegated
    speedup = speedup.sort_index(axis=0).sort_index(axis=1, level=0)

    plt.style.use("seaborn-v0_8-whitegrid")
    fig, ax = plt.subplots(figsize=(max(8, 1.2 * len(speedup.columns)), 4.8))
    if speedup.empty:
        ax.text(
            0.5,
            0.5,
            "speedup unavailable (missing delegated or traditional data)",
            ha="center",
            va="center",
            transform=ax.transAxes,
        )
        ax.axis("off")
        fig.tight_layout()
        fig.savefig(output_png, dpi=180, bbox_inches="tight")
        return
    image = ax.imshow(speedup.values, cmap="RdYlGn", aspect="auto", vmin=0.0)
    cbar = fig.colorbar(image, ax=ax)
    cbar.set_label("traditional / delegated speedup")

    ax.set_xticks(range(len(speedup.columns)))
    ax.set_xticklabels([col[1] for col in speedup.columns], rotation=35, ha="right")
    ax.set_yticks(range(len(speedup.index)))
    ax.set_yticklabels([str(t) for t in speedup.index])
    ax.set_xlabel("Allocation size")
    ax.set_ylabel("Threads")
    ax.set_title(f"{metric_label(metric)} speedup heatmap (>1 means delegated faster)")

    for row in range(speedup.shape[0]):
        for col in range(speedup.shape[1]):
            value = speedup.iloc[row, col]
            text_color = "black" if 0.55 <= value <= 1.8 else "white"
            ax.text(col, row, f"{value:.2f}x", ha="center", va="center",
                    color=text_color, fontsize=9)

    fig.tight_layout()
    fig.savefig(output_png, dpi=180, bbox_inches="tight")


def plot_speedup_lines(summary_df, metric, output_png):
    metric_df = summary_df[summary_df["metric"] == metric].copy()
    delegated = metric_df[metric_df["path"] == "delegated"].copy()
    traditional = metric_df[metric_df["path"] == "traditional"].copy()
    merged = delegated.merge(
        traditional,
        on=["threads", "size_human", "size_bytes", "metric"],
        suffixes=("_delegated", "_traditional"),
    )
    if merged.empty:
        plt.style.use("seaborn-v0_8-whitegrid")
        fig, ax = plt.subplots(figsize=(10, 5.5))
        ax.text(
            0.5,
            0.5,
            "speedup unavailable (missing delegated or traditional data)",
            ha="center",
            va="center",
            transform=ax.transAxes,
        )
        ax.axis("off")
        fig.tight_layout()
        fig.savefig(output_png, dpi=180, bbox_inches="tight")
        return
    merged["speedup"] = merged["mean_ns_traditional"] / merged["mean_ns_delegated"]
    merged["size_kib"] = merged["size_bytes"] / 1024.0

    plt.style.use("seaborn-v0_8-whitegrid")
    fig, ax = plt.subplots(figsize=(10, 5.5))
    for threads in sorted(merged["threads"].unique()):
        line = merged[merged["threads"] == threads].sort_values("size_bytes")
        ax.plot(
            line["size_kib"],
            line["speedup"],
            marker="o",
            linewidth=2,
            label=f"{threads} threads",
        )
    ax.axhline(1.0, color="black", linestyle="--", linewidth=1)
    ax.set_xscale("log", base=2)
    ax.set_yscale("log", base=2)
    ax.set_xlabel("Allocation size (KiB)")
    ax.set_ylabel("traditional / delegated")
    ax.set_title(f"{metric_label(metric)} speedup by allocation size")
    ax.grid(True, which="both", linestyle="--", alpha=0.35)
    ax.legend()
    fig.tight_layout()
    fig.savefig(output_png, dpi=180, bbox_inches="tight")


def plot_results(summary_df, results_dir):
    for metric in ["alloc_only", "dealloc_only"]:
        plot_latency_by_thread(
            summary_df, metric, results_dir / f"{metric}_latency_by_thread.png"
        )
        plot_latency_small_sizes(
            summary_df, metric, results_dir / f"{metric}_latency_small_sizes.png"
        )
        plot_speedup_heatmap(
            summary_df, metric, results_dir / f"{metric}_speedup_heatmap.png"
        )
        plot_speedup_lines(
            summary_df, metric, results_dir / f"{metric}_speedup_lines.png"
        )


def write_metric_tables(f, summary_df, metric):
    metric_df = summary_df[summary_df["metric"] == metric].copy()
    delegated = metric_df[metric_df["path"] == "delegated"].copy()
    traditional = metric_df[metric_df["path"] == "traditional"].copy()
    speedup_df = delegated.merge(
        traditional,
        on=["threads", "size_human", "size_bytes", "metric"],
        suffixes=("_delegated", "_traditional"),
    )
    speedup_df["speedup"] = (
        speedup_df["mean_ns_traditional"] / speedup_df["mean_ns_delegated"]
    )

    f.write(f"## {metric_label(metric)} mean latency\n\n")
    pivot = metric_df.pivot_table(
        index=["path", "threads"],
        columns="size_human",
        values="mean_ns",
        aggfunc="first",
    )
    pivot = pivot.reindex(
        sorted(pivot.columns, key=parse_human_size_to_bytes),
        axis=1,
    )
    columns = [str(col) for col in pivot.columns.tolist()]
    f.write("| path | threads | " + " | ".join(columns) + " |\n")
    f.write("| --- | ---: | " + " | ".join(["---:"] * len(columns)) + " |\n")
    for (path, threads), row in pivot.iterrows():
        values = []
        for col in pivot.columns:
            value = row[col]
            values.append("" if pd.isna(value) else f"{value:.2f}")
        f.write(f"| {path} | {threads} | " + " | ".join(values) + " |\n")
    f.write("\n")

    f.write(f"## {metric_label(metric)} speedup\n\n")
    speedup_pivot = speedup_df.pivot_table(
        index="threads",
        columns="size_human",
        values="speedup",
        aggfunc="first",
    )
    speedup_pivot = speedup_pivot.reindex(
        sorted(speedup_pivot.columns, key=parse_human_size_to_bytes),
        axis=1,
    )
    columns = [str(col) for col in speedup_pivot.columns.tolist()]
    f.write("| threads | " + " | ".join(columns) + " |\n")
    f.write("| ---: | " + " | ".join(["---:"] * len(columns)) + " |\n")
    for threads, row in speedup_pivot.sort_index().iterrows():
        values = []
        for col in speedup_pivot.columns:
            value = row[col]
            values.append("" if pd.isna(value) else f"{value:.2f}x")
        f.write(f"| {threads} | " + " | ".join(values) + " |\n")
    f.write("\n")


def write_summary_md(summary_df, output_md, args):
    with open(output_md, "w", encoding="utf-8") as f:
        f.write("# SharedArena Microbench Summary\n\n")
        f.write(f"- Remote host: `{args.remote_host}`\n")
        f.write(f"- Device dir: `{args.device_dir}`\n")
        f.write(f"- Repetitions: `{args.repetitions}`\n")
        f.write(f"- Iterations per run: `{args.iterations}`\n")
        f.write(f"- Warmup per run: `{args.warmup}`\n")
        f.write(f"- Thread counts: `{args.thread_counts}`\n")
        f.write(f"- Sizes: `{args.sizes}`\n\n")
        write_metric_tables(f, summary_df, "alloc_only")
        write_metric_tables(f, summary_df, "dealloc_only")


def build_summary_df(df):
    return (
        df.groupby(["threads", "size_human", "size_bytes", "path", "metric"], as_index=False)
        .agg(
            mean_ns=("mean_ns", "mean"),
            std_ns=("mean_ns", "std"),
            route_hint=("route_hint", "first"),
            secondary_cache_calls=("secondary_cache_calls", "first"),
            secondary_cache_hits=("secondary_cache_hits", "first"),
            secondary_cache_hit_rate=("secondary_cache_hit_rate", "first"),
        )
        .fillna({"std_ns": 0.0})
    )


def merge_existing_results(args, results_dir):
    inputs = [Path(item) for item in parse_list(args.merge_inputs)]
    frames = []
    for item in inputs:
        csv_path = item
        if item.is_dir():
            csv_path = item / "raw_results.csv"
        if not csv_path.exists():
            raise RuntimeError(f"合并输入不存在 raw_results.csv: {csv_path}")
        frame = pd.read_csv(csv_path)
        frame["source"] = str(item)
        frames.append(frame)
    if not frames:
        raise RuntimeError("未提供任何 --merge-inputs")

    df = pd.concat(frames, ignore_index=True)
    raw_csv = results_dir / "raw_results.csv"
    df.to_csv(raw_csv, index=False, quoting=csv.QUOTE_MINIMAL)

    summary_df = build_summary_df(df)
    summary_csv = results_dir / "summary.csv"
    summary_df.to_csv(summary_csv, index=False, quoting=csv.QUOTE_MINIMAL)

    plot_results(summary_df, results_dir)
    output_md = results_dir / "summary.md"
    write_summary_md(summary_df, output_md, args)

    print(f"合并结果目录: {results_dir}")
    print(f"原始数据: {raw_csv}")
    print(f"汇总数据: {summary_csv}")
    print(f"摘要: {output_md}")


def main():
    parser = argparse.ArgumentParser(description="批量运行 SharedArena Android microbench 并绘图")
    parser.add_argument(
        "--local-linux",
        action="store_true",
        help="在本机 Linux 直接运行 --binary（跳过 ssh/adb）。注意 benchmark 仍可能因 SharedArenaPool not ready 而跳过 delegated。",
    )
    parser.add_argument("--remote-host", default="lrc@192.168.60.141")
    parser.add_argument("--remote-stage-dir", default="/tmp/scudo-bench")
    parser.add_argument("--device-dir", default="/data/local/tmp/scudo")
    parser.add_argument("--android-ndk", default="/home/lrc/patent/kernel/kernel_platform/prebuilts/ndk-r26")
    parser.add_argument("--build-dir", default=str(DEFAULT_BUILD_DIR))
    parser.add_argument("--binary", default=str(DEFAULT_BINARY))
    parser.add_argument("--thread-counts", default="1,2,4,8")
    parser.add_argument("--sizes", default="65536,131072,262144,1048576")
    parser.add_argument("--iterations", type=int, default=200)
    parser.add_argument("--warmup", type=int, default=50)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument(
        "--bench-extra-args",
        default="",
        help="额外透传给 benchmark 的参数字符串（会按 shell 风格拆分），例如：\"--allocator combined\"",
    )
    parser.add_argument(
        "--bench-extra-env",
        default="",
        help="额外透传给 benchmark 的环境变量，逗号分隔 K=V，例如：\"SCUDO_SHARED_ARENA_ALLOW_NO_KERNEL=1,SCUDO_SHARED_ARENA_TRACE=1\"",
    )
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--skip-deploy", action="store_true")
    parser.add_argument("--use-su", action="store_true", help="通过 adb shell su -c 运行 benchmark")
    parser.add_argument("--results-dir", default=None)
    parser.add_argument(
        "--merge-inputs",
        default="",
        help="只合并已有结果，不运行 benchmark；输入为逗号分隔的结果目录或 raw_results.csv",
    )
    args = parser.parse_args()

    results_dir = Path(args.results_dir) if args.results_dir else (
        SCRIPT_DIR / "microbench-results" / datetime.now().strftime("%Y%m%d-%H%M%S")
    )
    raw_dir = results_dir / "raw"
    raw_dir.mkdir(parents=True, exist_ok=True)

    if args.merge_inputs:
        merge_existing_results(args, results_dir)
        return

    thread_counts = parse_threads(args.thread_counts)
    size_bytes = parse_sizes_bytes(args.sizes)
    size_arg = ",".join(str(x) for x in size_bytes)
    extra_bench_args = shlex.split(args.bench_extra_args) if args.bench_extra_args else []
    extra_bench_env = parse_env_kv_list(args.bench_extra_env)

    extra_env = {}
    if not args.local_linux and not args.skip_build:
        build_android_binary(args.android_ndk, extra_env)

    binary_path = Path(args.binary)
    if not binary_path.exists():
        binary_path = Path(args.build_dir) / "ScudoSharedArenaLatencyBench-aarch64-Test"
    if not binary_path.exists():
        raise RuntimeError(f"未找到 benchmark 二进制: {binary_path}")

    adb_bin = None
    if not args.local_linux:
        adb_bin = detect_remote_adb_bin(args.remote_host)
        if not args.skip_deploy:
            deploy_binary(binary_path, args.remote_host, args.remote_stage_dir, args.device_dir, adb_bin)

    all_records = []
    for repetition in range(1, args.repetitions + 1):
        for threads in thread_counts:
            print(
                f"[run] repetition={repetition}/{args.repetitions} "
                f"threads={threads} sizes={size_arg}",
                flush=True,
            )
            kill_bench_processes(args.remote_host, adb_bin, binary_path.name, args.use_su)
            time.sleep(1)
            raw_path = raw_dir / f"threads{threads:02d}_rep{repetition:02d}.txt"
            if args.local_linux:
                cmd = [
                    str(binary_path),
                    "--iterations",
                    str(args.iterations),
                    "--warmup",
                    str(args.warmup),
                    "--threads",
                    str(threads),
                    "--sizes",
                    size_arg,
                ]
                cmd.extend(extra_bench_args)
                env = os.environ.copy()
                env["MALLOC_USE_APP_DEFAULTS"] = "1"
                env.update(extra_bench_env)
                try:
                    result = run(cmd, cwd=str(SCRIPT_DIR), env=env,
                                 capture_output=True)
                except subprocess.CalledProcessError as exc:
                    raw_path.write_text((exc.stdout or "") + (exc.stderr or ""),
                                        encoding="utf-8")
                    raise
            else:
                env_exports = ""
                if extra_bench_env:
                    env_exports = " ".join(
                        f"export {shlex.quote(k)}={shlex.quote(v)} &&"
                        for k, v in extra_bench_env.items()
                    )
                shell_cmd = (
                    f"cd {shlex.quote(args.device_dir)} && "
                    f"{env_exports} env MALLOC_USE_APP_DEFAULTS=1 ./{shlex.quote(binary_path.name)} "
                    f"--iterations {args.iterations} --warmup {args.warmup} "
                    f"--threads {threads} --sizes {size_arg}"
                )
                if extra_bench_args:
                    shell_cmd += " " + " ".join(shlex.quote(x) for x in extra_bench_args)
                try:
                    result = adb_remote_cmd(
                        args.remote_host, adb_bin, shell_cmd,
                        capture_output=True, use_su=args.use_su)
                except subprocess.CalledProcessError as exc:
                    raw_path.write_text((exc.stdout or "") + (exc.stderr or ""),
                                        encoding="utf-8")
                    raise
            raw_path.write_text(result.stdout, encoding="utf-8")
            records = parse_benchmark_output(result.stdout, repetition, threads)
            all_records.extend(records)
            print(
                f"[done] repetition={repetition}/{args.repetitions} "
                f"threads={threads} parsed_records={len(records)}",
                flush=True,
            )

    df = pd.DataFrame([r.__dict__ for r in all_records])
    raw_csv = results_dir / "raw_results.csv"
    df.to_csv(raw_csv, index=False, quoting=csv.QUOTE_MINIMAL)

    summary_df = build_summary_df(df)
    summary_csv = results_dir / "summary.csv"
    summary_df.to_csv(summary_csv, index=False, quoting=csv.QUOTE_MINIMAL)

    plot_results(summary_df, results_dir)

    output_md = results_dir / "summary.md"
    write_summary_md(summary_df, output_md, args)

    print(f"结果目录: {results_dir}")
    print(f"原始数据: {raw_csv}")
    print(f"汇总数据: {summary_csv}")
    print(f"图表: {results_dir / 'alloc_only_latency_by_thread.png'}")
    print(f"图表: {results_dir / 'alloc_only_latency_small_sizes.png'}")
    print(f"图表: {results_dir / 'alloc_only_speedup_heatmap.png'}")
    print(f"图表: {results_dir / 'alloc_only_speedup_lines.png'}")
    print(f"图表: {results_dir / 'dealloc_only_latency_by_thread.png'}")
    print(f"图表: {results_dir / 'dealloc_only_latency_small_sizes.png'}")
    print(f"图表: {results_dir / 'dealloc_only_speedup_heatmap.png'}")
    print(f"图表: {results_dir / 'dealloc_only_speedup_lines.png'}")
    print(f"摘要: {output_md}")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"错误：{exc}", file=sys.stderr)
        sys.exit(1)
