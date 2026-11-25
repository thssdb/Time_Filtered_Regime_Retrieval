"""Paired synthetic or user-supplied queries, scored against exact window-restricted search."""
import argparse
import csv
import random
from pathlib import Path
from time import perf_counter_ns

import numpy as np
from worker import Workers

PRIMARY = ["weighted_row", "weighted_slice"]
SHARDED = ["all_scan", "skip_scan", "all_binary_packed", "skip_binary_packed"]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("run", type=Path)
    ap.add_argument("build", type=Path)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--windows", nargs="+", type=int, default=list(range(10,100,10)))
    ap.add_argument("--queries", type=int, default=16)
    ap.add_argument("--query-file", type=Path, help="NumPy array of held-out raw FP32 query vectors, shape (n,42)")
    ap.add_argument("--repeats", type=int, default=3)
    args = ap.parse_args()
    if min(args.queries,args.repeats)<1 or any(not 1<=w<=100 for w in args.windows):
        ap.error("windows must be 1..100; queries and repeats must be positive")
    base = args.run/"prepared/collection"
    stamps = np.memmap(base/"point_time_ns.i64", "<i8","r").reshape(-1,18)
    values = np.memmap(base/"raw54.f32", "<f4","r").reshape(-1,54)
    ids = np.fromfile(args.run/"time/collection/time_ids.u32","<u4")
    ends = stamps[ids,-1]
    rng = np.random.default_rng(710)
    if args.query_file:
        queries = np.load(args.query_file, allow_pickle=False)
        if queries.ndim!=2 or queries.shape[1]!=42 or len(queries)<args.queries or not np.isfinite(queries).all():
            raise ValueError("Expected at least --queries finite 42D vectors")
        queries = queries[:args.queries]
    else:
        queries = values[rng.integers(len(ids),size=args.queries)][:,np.r_[0:6,18:54]]
    arms = PRIMARY + SHARDED
    shuffle = random.Random(42)
    with Workers(args.run,args.build,sharded=True) as native, args.out.open("w") as dest:
        fields = "window,arm,observations,mean_us,p50_us,p95_us,fetch_p50_us,native_p50_us,recall_at_10".split(",")
        writer = csv.DictWriter(dest,fieldnames=fields)
        writer.writeheader()
        for window in args.windows:
            width = max(1,len(ends)*window//100)
            samples = {arm:[] for arm in arms}
            for q in queries:
                start = int(rng.integers(len(ends)-width+1))
                lo,hi = int(ends[start]),int(ends[start+width-1])+1
                truth = set(native.query("exact",lo,hi,q)["r"])
                commands={arm:native.command(arm,lo,hi,q) for arm in arms}
                order = arms.copy()
                for repeat in range(args.repeats+1):
                    shuffle.shuffle(order)
                    for arm in order:
                        tick=perf_counter_ns()
                        reply=native.submit(arm,commands[arm])
                        ranks=reply["r"]
                        fetch_us,records=native.fetch(ranks)
                        wall_us=(perf_counter_ns()-tick)/1000
                        expected=b"".join(stamps[ids[r]].tobytes()+values[ids[r]].tobytes() for r in ranks)
                        if records!=expected or len(ranks)!=len(set(ranks)) or any(not lo<=int(ends[r])<hi for r in ranks):
                            raise ValueError(f"fetch, duplicate ID or timestamp mismatch in {arm}")
                        recall=len(truth.intersection(ranks))/len(truth)
                        if repeat:
                            samples[arm].append((reply["search_us"]+fetch_us,fetch_us,wall_us,recall))
            for arm,rows in samples.items():
                native_us,fetch,client,recall=np.asarray(rows).T
                writer.writerow(dict(window=window,arm=arm,observations=len(rows),mean_us=client.mean(),
                                    p50_us=np.median(client),p95_us=np.percentile(client,95),
                                    fetch_p50_us=np.median(fetch),native_p50_us=np.median(native_us),
                                    recall_at_10=recall.mean()))
            dest.flush()


if __name__ == "__main__":
    main()
