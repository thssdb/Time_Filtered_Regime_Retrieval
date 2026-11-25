"""Render the method diagrams and/or evaluation charts from source."""
import argparse
import os
import subprocess
import sys
from pathlib import Path


def run(script,*args):
    subprocess.run([sys.executable, Path(__file__).with_name(script+".py"),*args],check=True)


if __name__=="__main__":
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("which",choices=["methods","evaluation","all"])
    ap.add_argument("--build",type=Path,default=Path("build"))
    ap.add_argument("--input",type=Path,help="directory of aggregate CSVs")
    ap.add_argument("--output",type=Path,required=True)
    a=ap.parse_args()
    if a.input:
        os.environ["REGIME_FIGURE_INPUT"]=str(a.input.resolve())
    os.environ["REGIME_FIGURE_OUTPUT"]=str(a.output.resolve())
    os.environ.setdefault("MPLBACKEND","Agg")
    if a.which in ("methods","all"):
        run("synthetic_storage_example")
        run("synthetic_queries",a.build.resolve())
        for name in ["fig01_motivation","fig02_query","fig03_04_storage","fig05_06_retrieval"]:
            run(name)
    if a.which in ("evaluation","all"):
        for name in ["fig07_encodings","fig08_latency","fig09_reference","fig10_order",
                     "fig11_pruning","fig12_sql","fig13_public_datasets"]:
            run(name)
