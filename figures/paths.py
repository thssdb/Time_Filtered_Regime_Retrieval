import os
from pathlib import Path

HERE = Path(__file__).resolve().parent
INPUT = Path(os.environ.get('REGIME_FIGURE_INPUT', HERE/'input'))
OUTPUT = Path(os.environ.get('REGIME_FIGURE_OUTPUT', HERE/'render'))
OUTPUT.mkdir(parents=True, exist_ok=True)
