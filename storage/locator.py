"""Map each chronological index rank to its time and value positions."""
import argparse
import csv
import struct
from pathlib import Path
import numpy as np

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    args = parser.parse_args()
    db = "collection"
    root = args.root
    ids = np.fromfile(root/"time"/db/"time_ids.u32", "<u4")
    rank = np.argsort(ids)
    loc = np.zeros(len(ids), dtype="<u4")
    seen = np.zeros(len(ids), dtype=bool)
    blocks = list(csv.reader((root/"blocks.tsv").read_text().splitlines(), delimiter="\t"))
    with (root/"templates"/f"{db}.placement.tsv").open() as stream:
        for line, block in zip(stream, blocks, strict=True):
            number, *ordered_ids = map(int, line.split())
            begin, length = map(int, block[3:5])
            if number != int(block[0]) or len(ordered_ids) != length or length > 128 or number >= 2**18:
                raise ValueError("Invalid physical placement")
            for p, original in enumerate(ordered_ids):
                h = int(rank[original])-begin
                if not 0 <= h < length or seen[original]:
                    raise ValueError("Invalid rank or duplicate ID")
                seen[original] = True
                loc[begin+h] = (number << 14) | (h << 7) | p
    if not seen.all():
        raise ValueError("Incomplete placement")
    dest = root/"stores"/db/"sim_common"
    dest.mkdir(parents=True, exist_ok=True)
    (dest/"address.bin").write_bytes(struct.pack("<8sIIII",b"NHADDR1\0",1,len(ids),len(blocks),0)+loc.tobytes())

if __name__=="__main__":
    main()
