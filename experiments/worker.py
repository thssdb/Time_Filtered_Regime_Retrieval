"""Resident native workers. Time ranks index the chronological locator directory."""
import json
import os
import struct
import subprocess
from contextlib import ExitStack
from pathlib import Path


class Workers:
    def __init__(self, run, build, sharded=False):
        self.run, self.build = Path(run), Path(build)
        self.env = dict(os.environ, SCALER_FILE=str(self.run/"scalers.f64"),
                        WEIGHT_FILE=str(self.run/"weights.f64"), KD_STORE=str(self.run/"first32"),
                        OMP_NUM_THREADS="1", OPENBLAS_NUM_THREADS="1")
        self.stack = ExitStack()
        try:
            self.search = self.open("search", self.run/"time/collection", self.run/"index.faiss")
            self.reader = self.open("read_records", "collection", self.run/"stores")
            if sharded:
                self.shards = self.open("shard_search", self.run/"time/collection", self.run/"shards")
        except BaseException:
            self.stack.close()
            raise

    def open(self, name, *args):
        proc = subprocess.Popen([self.build/name, *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=self.env)
        self.stack.callback(self.stop, proc)
        line = proc.stdout.readline()
        if line.strip() != b"READY" and (not line or json.loads(line).get("status") != "ready"):
            raise RuntimeError(f"{name} failed at startup")
        return proc

    @staticmethod
    def stop(proc):
        proc.stdin.close()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        proc.stdout.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.stack.close()

    @staticmethod
    def command(arm, lo, hi, coordinates):
        line = " ".join([arm, str(lo), str(hi), *(str(float(x)) for x in coordinates)])
        return (line+"\n").encode("ascii")

    def query(self, arm, lo, hi, coordinates):
        return self.submit(arm,self.command(arm,lo,hi,coordinates))

    def submit(self, arm, command):
        proc = self.shards if arm.startswith(("skip_", "all_")) else self.search
        proc.stdin.write(command)
        proc.stdin.flush()
        reply = proc.stdout.readline()
        if not reply:
            raise RuntimeError("native search stopped during query")
        return json.loads(reply)

    def fetch(self, ranks):
        message = " ".join(map(str, [len(ranks), *ranks])) + "\n"
        self.reader.stdin.write(message.encode("ascii"))
        self.reader.stdin.flush()
        nbytes = 8 + len(ranks)*(18*8+54*4)
        payload = self.reader.stdout.read(nbytes)
        if len(payload) != nbytes:
            raise RuntimeError("incomplete native fetch")
        return struct.unpack_from("<q", payload)[0]/1000, payload[8:]
