"""Convert one nine-window benchmark summary (Fig. 8) or eight independent batches (Fig. 11)."""
import argparse
import csv
import math
import statistics
from pathlib import Path

from benchmark import SHARDED


def summary(path):
    with path.open() as source:
        rows=list(csv.DictReader(source))
    indexed={(int(r["window"]),r["arm"]):r for r in rows}
    if len(indexed)!=len(rows):
        raise ValueError(f"duplicate observations in {path}")
    return indexed


if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("output",type=Path)
    p.add_argument("batches",type=Path,nargs="+")
    args=p.parse_args()
    if len(args.batches) not in (1,8) or len(set(x.resolve() for x in args.batches))!=len(args.batches):
        p.error("supply one summary or eight distinct batch files")
    args.output.mkdir(parents=True,exist_ok=True)
    runs=[summary(path) for path in args.batches]
    windows=range(10,100,10)
    if len(runs)==1:
        with (args.output/"fig8_query_windows.csv").open("w") as f:
            w=csv.writer(f)
            w.writerow(["route","window","p50_us","p95_us"])
            for window in windows:
                for arm,route in [("weighted_slice","pruned"),("weighted_row","row")]:
                    r=runs[0][window,arm]
                    w.writerow([route,window,r["p50_us"],r["p95_us"]])
    else:
        with (args.output/"query_pruning_ablation.csv").open("w") as f:
            w=csv.writer(f)
            w.writerow(["statistic","window","arm","batches","mean_latency_us","ci95_half_us"])
            for window in windows:
                for arm in SHARDED:
                    values=[float(r[window,arm]["p50_us"]) for r in runs]
                    w.writerow(["p50",window,arm,8,statistics.mean(values),
                                2.364624251*statistics.stdev(values)/math.sqrt(8)])
