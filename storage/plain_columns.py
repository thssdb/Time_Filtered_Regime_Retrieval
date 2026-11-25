"""Write the PLAIN comparison as one eight-byte column per signal and one for time."""
import argparse
from pathlib import Path

import numpy as np


def write_columns(source, placement, output, signals, samples):
    raw = np.memmap(source / "raw54.f32", "<f4", "r").reshape(-1, signals, samples)
    times = np.memmap(source / "point_time_ns.i64", "<i8", "r").reshape(len(raw), samples)
    with placement.open() as lines:
        order = np.array([int(word) for line in lines for word in line.split()[1:]])
    if not np.array_equal(np.sort(order), np.arange(len(raw))):
        raise ValueError("placement must contain each segment exactly once")
    output.mkdir(parents=True, exist_ok=False)
    times[order].tofile(output / "time.i64")
    for channel in range(signals):
        raw[order, channel].astype("<f8").tofile(output / f"c{channel}.f64")
    encoded = sum(file.stat().st_size for file in output.iterdir())
    if encoded != 8 * (times.size + raw.size):
        raise ValueError("unexpected PLAIN payload size")
    return encoded


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("source", type=Path)
    ap.add_argument("placement", type=Path)
    ap.add_argument("output", type=Path)
    ap.add_argument("--signals", type=int, default=3)
    ap.add_argument("--samples", type=int, default=18)
    args = ap.parse_args()
    size = write_columns(args.source, args.placement, args.output, args.signals, args.samples)
    print(f"PLAIN: {size} bytes, compression ratio 1.0x")
