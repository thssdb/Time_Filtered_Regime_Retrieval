"""Serve the IoTDB UDTF's local Unix socket using the resident retrieval and fetch workers."""
import argparse
import base64
import math
import os
import socketserver
import struct
from pathlib import Path
import numpy as np
from worker import Workers


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        try:
            line=self.rfile.readline(8193)
            fields=line.decode("ascii").split()
            if len(line)>8192 or len(fields)!=45 or fields[0]!="ours":
                raise ValueError("bad request")
            lo,hi=map(int,fields[1:3])
            q=list(map(float,fields[3:]))
            if not -(1<<63)<=lo<hi<(1<<63) or not all(math.isfinite(x) and abs(x)<=np.finfo("f4").max for x in q):
                raise ValueError("bad bounds or coordinates")
        except (ValueError,UnicodeError):
            self.wfile.write(b"ERROR invalid request\n")
            return
        ranks=self.server.workers.query("weighted_slice",lo,hi,q)["r"]
        _, data=self.server.workers.fetch(ranks)
        ids=self.server.ids[ranks].astype("<u4")
        self.wfile.write(base64.b64encode(struct.pack("<I",len(ranks))+ids.tobytes()+data)+b"\n")


if __name__=="__main__":
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("run",type=Path)
    ap.add_argument("build",type=Path)
    ap.add_argument("socket",type=Path)
    args=ap.parse_args()
    with Workers(args.run,args.build) as workers, socketserver.UnixStreamServer(str(args.socket),Handler) as server:
        os.chmod(args.socket,0o600)
        server.workers=workers
        server.ids=np.fromfile(args.run/"time/collection/time_ids.u32","<u4")
        try:
            server.serve_forever()
        finally:
            args.socket.unlink(missing_ok=True)
