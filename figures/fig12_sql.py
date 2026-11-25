"""Render the SQL comparison as a font-preserving vector EPS."""
import csv
from pathlib import Path

import matplotlib as mpl
import matplotlib.pyplot as plt
from matplotlib import font_manager


from paths import INPUT, OUTPUT as OUT
DATA = INPUT / "system_sql_latency.csv"
OUTPUT = OUT / "fig_iotdb_sql_latency.eps"
WINDOWS = list(range(10, 100, 10))
COLORS = {"faiss": "#334B64", "ours": "#BB363E"}
LABELS = {"faiss": "Default + FAISS", "ours": "Ours"}


def main():
    with DATA.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    values = {(row["system"], int(row["window"])): row for row in rows
              if row["system"] in LABELS}
    assert len(values) == 18
    font = "STIX Two Text" if any(f.name=="STIX Two Text" for f in font_manager.fontManager.ttflist) else "STIXGeneral"
    mpl.rcParams.update({
        "font.family": font,
        "font.size": 7.4,
        "axes.titlesize": 8.0,
        "axes.labelsize": 7.8,
        "xtick.labelsize": 7.4,
        "ytick.labelsize": 7.4,
        "legend.fontsize": 7.4,
        "ps.fonttype": 42,
    })
    figure, axes = plt.subplots(1, 2, figsize=(3.45, 1.95))
    specifications = (
        ("p50_ms", "(a) Median", "p50 (ms)", 0, 8, 2),
        ("p95_ms", "(b) 95th percentile", "p95 (ms)", 0, 14, 2),
    )
    for axis, (field, title, ylabel, low, high, step) in zip(axes, specifications):
        for system, dash, marker in (("faiss", (0, (4, 2)), "s"), ("ours", "solid", "o")):
            axis.plot(WINDOWS, [float(values[system, window][field]) for window in WINDOWS],
                      color=COLORS[system], linewidth=1.2, linestyle=dash,
                      marker=marker, markersize=3, markerfacecolor=("white" if system == "faiss" else COLORS[system]),
                      markeredgecolor=COLORS[system], markeredgewidth=.65,
                      label=LABELS[system])
        axis.set_title(title, fontweight="bold", pad=3)
        axis.set_xlim(8, 92)
        axis.set_ylim(low, high)
        axis.set_xticks(WINDOWS, [str(window) if window % 20 == 10 else "" for window in WINDOWS])
        axis.set_yticks(list(range(low, high + 1, step)))
        axis.set_xlabel("Time window (%)", labelpad=2)
        axis.set_ylabel(ylabel, labelpad=2)
        axis.grid(False)
        for spine in axis.spines.values():
            spine.set_color("#616970")
            spine.set_linewidth(.55)
        axis.tick_params(width=.55, length=2.5, color="#616970", pad=1.5)
    handles, labels = axes[0].get_legend_handles_labels()
    figure.legend(handles, labels, loc="upper center", bbox_to_anchor=(.5, .995), ncol=2,
                  frameon=False, handlelength=2.0, columnspacing=1.6, handletextpad=.45)
    figure.subplots_adjust(left=.13, right=.985, bottom=.25, top=.72, wspace=.38)
    figure.savefig(OUTPUT, format="eps", dpi=1200, facecolor="white", transparent=False)
    plt.close(figure)


if __name__ == "__main__":
    main()
