"""Apply the paper's font policy to an editable result-figure PPTX."""

from __future__ import annotations

import os
import re
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile


FONT_NAME = "Linux Biolinum O"


def normalize_result_figure_fonts(path: Path, size_centipoints: int) -> None:
    """Set all slide and chart text to one font and one printed-size calibration."""
    path = Path(path)
    rebuilt = path.with_name(f".{path.name}.font-policy.tmp")
    with ZipFile(path) as source, ZipFile(rebuilt, "w", ZIP_DEFLATED) as target:
        for item in source.infolist():
            payload = source.read(item)
            is_slide = item.filename == "ppt/slides/slide1.xml"
            is_chart = item.filename.startswith("ppt/charts/chart") and item.filename.endswith(".xml")
            if is_slide or is_chart:
                xml = payload.decode("utf-8")
                xml = re.sub(r'typeface="[^"]+"', f'typeface="{FONT_NAME}"', xml)
                xml = re.sub(r'\bsz="\d+"', f'sz="{size_centipoints}"', xml)
                payload = xml.encode("utf-8")
            target.writestr(item, payload)
    os.replace(rebuilt, path)
