"""Build the 60-sample value and timestamp columns for a supplied public collection."""
import argparse
import os
import subprocess
import sys
from pathlib import Path

import numpy as np

from time_templates import seal_column

repo = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(repo / "experiments" / "public_datasets"))
from build_public import physical_layout


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="prepared NASA_battery, Hydraulic_system or metropt3 directory")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    raw, blocks = physical_layout(args.source)
    times = np.memmap(args.source / "point_time_ns.i64", "<i8", "r").reshape(-1, 60)
    args.output.mkdir(parents=True, exist_ok=False)
    chronological = [ids[np.argsort(times[ids, 0], kind="stable")] for ids in blocks]
    seal_column(times, chronological, args.output / "time.template")
    placement = args.output / "placement.tsv"
    with placement.open("w") as out:
        for i, ids in enumerate(blocks):
            print(i, *ids, sep="\t", file=out)
    target = repo / "iotdb" / "target"
    cp = os.pathsep.join([str(target / "classes"), (target / "classpath.txt").read_text().strip()])
    subprocess.run(["java", "-cp", cp, "BuildValuePages", args.source / "raw54.f32", placement,
                    args.output, str(raw.shape[1]), "60", "64", "first", "--unsigned-keys"], check=True)
    encoded = sum(p.stat().st_size for p in args.output.glob("*.pack")) + (args.output / "time.template").stat().st_size
    print(f"Combined compression ratio: {8 * (raw.size + times.size) / encoded:.6f}x")
