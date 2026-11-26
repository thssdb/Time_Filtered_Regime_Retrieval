"""Read a raw 42-coordinate query as JSON on stdin and return ordered full records."""
import json
import argparse
import struct
import sys
from contextlib import ExitStack
from pathlib import Path

from method import RegimeSearch
from connection import connect


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("database")
    parser.add_argument("build", type=Path)
    options = parser.parse_args()
    args = json.load(sys.stdin)
    with ExitStack() as stack:
        con = stack.enter_context(connect(options.database))
        engine = RegimeSearch(con, options.build.resolve(), stack)
        _, ids, records = engine.query(int(args['start_ns']), int(args['end_ns']), args['coordinates'])
        matches = []
        for identifier, record in zip(ids, records):
            values = struct.unpack('<18q54f', record)
            matches.append(dict(id=identifier, timestamps=values[:18], values=values[18:]))
        json.dump(matches, sys.stdout, allow_nan=False)
        sys.stdout.write('\n')


if __name__ == '__main__':
    main()
