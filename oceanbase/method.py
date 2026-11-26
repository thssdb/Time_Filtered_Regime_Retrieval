"""OceanBase-backed regime retrieval, with native time-range scans and two-stage SQL ranking."""
import os
import struct
import subprocess
from collections import defaultdict
from contextlib import ExitStack
from pathlib import Path
from tempfile import TemporaryDirectory

import numpy as np


DIM = 42
COLS = [f'z{i}' for i in range(DIM)]
RECORD_SIZE = 18*8 + 54*4


def fit_query(raw, scalers, weights):
    mu, sd = np.repeat(scalers.reshape(3, 2), [6, 18, 18], axis=0).T
    raw = np.asarray(raw, np.float32)
    if raw.shape != (DIM,) or not np.isfinite(raw).all():
        raise ValueError('expected 42 finite coordinates')
    q = ((raw.astype(float)-mu)/sd).astype(np.float32).astype(float)
    weighted = weights*q
    norm = np.sqrt(sum(float(a*b) for a,b in zip(weighted,q)))
    if not np.isfinite(norm) or norm <= 0:
        raise ValueError('weighted cosine requires a finite, positive query norm')
    inverse = 1.0 / norm
    return q, weighted, inverse


def ranking_sql(nlists):
    # DOUBLE storage contains the exact FP32 coordinate. Explicit products and
    # their sum preserve the paper worker's coordinate order in both stages.
    dot = '+'.join(f'(%s*{x})' for x in COLS)
    squared = '+'.join(f'((%s*(%s-{x}))*(%s-{x}))' for x in COLS)
    slots = ','.join(['%s']*nlists)
    return f'''SELECT id, addr FROM
        (SELECT id,addr,{','.join(COLS)}, (({dot})*inv_norm)*%s AS score
         FROM postings WHERE list_id IN ({slots}) AND end_ns >= %s AND end_ns < %s
         ORDER BY score DESC,id LIMIT 100) shortlisted
        ORDER BY SQRT({squared}),id LIMIT 10'''


class RegimeSearch:
    """Open each partition once. Query-time postings and compressed payloads live in SQL."""
    def __init__(self, connection, binaries, stack: ExitStack):
        self.db = connection
        self.directory = Path(stack.enter_context(TemporaryDirectory()))
        with self.db.cursor() as c:
            c.execute('SELECT name,payload FROM metadata')
            objects = dict(c.fetchall())
            # The locator stores the ordinal in device-ID order. Device IDs may
            # start at a nonzero value and can have gaps across sealed files.
            c.execute('SELECT block_id FROM blocks ORDER BY block_id')
            self.block_ids = [row[0] for row in c.fetchall()]
        if objects.get('ready') != b'group16-v1':
            raise ValueError("incomplete or unsupported OceanBase store")
        for name in ['graph','bounds','0.dict','1.dict','2.dict']:
            (self.directory/name).write_bytes(objects[name])
        self.weights = np.frombuffer(objects['weights'], '<f8')
        self.scalers = np.frombuffer(objects['scalers'], '<f8')
        self.router = self._worker(stack, [binaries/'ob_router','query',self.directory/'graph',
                                          self.directory/'bounds'])
        self.pages = self._worker(stack, [binaries/'ob_pages','read',self.directory,'-'])
        self.sql = {}

    @staticmethod
    def _worker(stack, command):
        p = subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,env={**os.environ,"OMP_NUM_THREADS":"1"})
        def close():
            p.stdin.close()
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
            p.stdout.close()
        stack.callback(close)
        if p.stdout.readline() != b'READY\n': raise RuntimeError(f'{command[0]} failed to start')
        return p

    def query(self, lo, hi, raw):
        if not -(1 << 63) <= lo < hi < (1 << 63):
            raise ValueError('expected a nonempty signed 64-bit time window')
        q, weighted, inv = fit_query(raw,self.scalers,self.weights)
        route_q=(weighted*inv).astype('<f4')
        command=' '.join([str(lo),str(hi),*(format(x,'.9g') for x in route_q)])
        self.router.stdin.write((command+'\n').encode())
        self.router.stdin.flush()
        wire=self.router.stdout.readline()
        if not wire: raise RuntimeError('router terminated')
        lists=list(map(int,wire.split()))
        if not lists: return [],[],[]
        if len(set(lists))!=len(lists) or len(lists)>16: raise RuntimeError('invalid probe list')
        tail=[float(v) for w,x in zip(self.weights,q) for v in (w,x,x)]
        if len(lists) not in self.sql: self.sql[len(lists)]=ranking_sql(len(lists))
        args=[*map(float,weighted),float(inv),*lists,int(lo),int(hi),*tail]
        with self.db.cursor() as c:
            c.execute(self.sql[len(lists)],args)
            hits=list(c.fetchall())
            if not hits: return lists,[],[]
            groups=defaultdict(list)
            for i,(_,addr) in enumerate(hits): groups[self.block_ids[addr>>14]].append((i,addr&16383))
            slots=','.join(['%s']*len(groups))
            c.execute(f'SELECT block_id,payload FROM blocks WHERE block_id IN ({slots})',list(groups))
            blocks=dict(c.fetchall())
        records=[None]*len(hits)
        for block,items in groups.items():
            blob=blocks[block]
            offsets=[offset for _,offset in items]
            self.pages.stdin.write(struct.pack('<I',len(blob))+blob+struct.pack('<'+'I'*(len(items)+1),len(items),*offsets))
            self.pages.stdin.flush()
            data=self.pages.stdout.read(RECORD_SIZE*len(items))
            if len(data)!=RECORD_SIZE*len(items): raise RuntimeError('short decoded block')
            for j,(i,_) in enumerate(items): records[i]=data[j*RECORD_SIZE:(j+1)*RECORD_SIZE]
        return lists,[row[0] for row in hits],records
