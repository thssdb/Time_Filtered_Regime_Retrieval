"""Render four verified p50 query paths as a single-column editable PPTX."""

from __future__ import annotations

import argparse
import csv
import math
import sys
from copy import deepcopy
from io import BytesIO
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile

import xlsxwriter
from xlsxwriter.utility import xl_range_formula
from lxml import etree as ET
from pptx import Presentation
from pptx.chart.data import CategoryChartData
from pptx.dml.color import RGBColor
from pptx.enum.chart import XL_CHART_TYPE
from pptx.enum.dml import MSO_LINE_DASH_STYLE
from pptx.enum.shapes import MSO_CONNECTOR, MSO_SHAPE
from pptx.enum.text import MSO_ANCHOR, PP_ALIGN
from pptx.util import Inches, Pt


from paths import INPUT, OUTPUT
from paper_result_figure_fonts import normalize_result_figure_fonts  # noqa: E402


FONT = "Linux Biolinum O"
FONT_SIZE = 9.09
INK = "#242A30"
AXIS = "#5B6670"
GRID = "#D6DADF"
WINDOWS = tuple(range(10, 100, 10))
ARMS = (
    ("all_scan", "Neither", "#7B8794", "circle", "solid"),
    ("skip_scan", "HNSW time pruning", "#1F5B82", "triangle", "solid"),
    ("all_binary_packed", "IVF binary search", "#D5A54E", "square", "dash"),
    ("skip_binary_packed", "Both", "#4C8D8A", "diamond", "dash"),
)
NS = {
    "c": "http://schemas.openxmlformats.org/drawingml/2006/chart",
    "r": "http://schemas.openxmlformats.org/officeDocument/2006/relationships",
}


def read_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as source:
        return list(csv.DictReader(source))


def read_summary(path: Path) -> dict[tuple[str, int, str], tuple[float, float]]:
    cells: dict[tuple[str, int, str], tuple[float, float]] = {}
    for row in read_rows(path):
        key = (row["statistic"], int(row["window"]), row["arm"])
        if key in cells:
            raise ValueError(f"duplicate summary cell {key}")
        if int(row["batches"]) != 8:
            raise ValueError(f"summary cell is not based on 8 batches: {key}")
        mean = float(row["mean_latency_us"])
        half = float(row["ci95_half_us"])
        if not math.isfinite(mean) or not math.isfinite(half) or half < 0:
            raise ValueError(f"invalid summary values at {key}")
        cells[key] = (mean, half)
    expected = {(stat, window, arm) for stat in ("p50",) for window in WINDOWS
                for arm, *_ in ARMS}
    if set(cells) != expected:
        raise ValueError(f"summary requires 36 cells; missing={expected - set(cells)}")
    return cells


def add_panel_title(slide, x: float, text: str) -> None:
    shape = slide.shapes.add_textbox(Inches(x), Inches(0.01), Inches(3.31), Inches(0.16))
    frame = shape.text_frame
    frame.margin_left = frame.margin_right = frame.margin_top = frame.margin_bottom = 0
    frame.vertical_anchor = MSO_ANCHOR.MIDDLE
    frame.word_wrap = False
    paragraph = frame.paragraphs[0]
    paragraph.alignment = PP_ALIGN.CENTER
    run = paragraph.add_run()
    run.text = text
    run.font.name = FONT
    run.font.size = Pt(FONT_SIZE)
    run.font.bold = True
    run.font.color.rgb = RGBColor.from_string(INK.removeprefix("#"))


def add_legend(slide) -> None:
    marker_shapes = {
        "circle": MSO_SHAPE.OVAL,
        "square": MSO_SHAPE.RECTANGLE,
        "diamond": MSO_SHAPE.DIAMOND,
        "triangle": MSO_SHAPE.ISOSCELES_TRIANGLE,
    }
    positions = (
        (0.01, 0.04, 0.48),
        (0.61, 0.04, 1.19),
        (1.90, 0.04, 0.96),
        (2.96, 0.04, 0.41),
    )
    for (x, y, width), (_, label, color, marker, dash) in zip(positions, ARMS, strict=True):
        line = slide.shapes.add_connector(
            MSO_CONNECTOR.STRAIGHT, Inches(x), Inches(y + 0.085),
            Inches(x + 0.08), Inches(y + 0.085)
        )
        line.line.color.rgb = RGBColor.from_string(color.removeprefix("#"))
        line.line.width = Pt(1.25)
        line.line.dash_style = (
            MSO_LINE_DASH_STYLE.DASH if dash == "dash" else MSO_LINE_DASH_STYLE.SOLID
        )
        point = slide.shapes.add_shape(
            marker_shapes[marker], Inches(x + 0.015), Inches(y + 0.060),
            Inches(0.050), Inches(0.050)
        )
        point.fill.solid()
        point.fill.fore_color.rgb = RGBColor.from_string(color.removeprefix("#"))
        point.line.color.rgb = RGBColor.from_string(color.removeprefix("#"))
        point.line.width = Pt(0.5)
        text = slide.shapes.add_textbox(
            Inches(x + 0.10), Inches(y), Inches(width), Inches(0.17)
        )
        frame = text.text_frame
        frame.margin_left = frame.margin_right = frame.margin_top = frame.margin_bottom = 0
        frame.vertical_anchor = MSO_ANCHOR.MIDDLE
        frame.word_wrap = False
        paragraph = frame.paragraphs[0]
        run = paragraph.add_run()
        run.text = label
        run.font.name = FONT
        run.font.size = Pt(FONT_SIZE)
        run.font.color.rgb = RGBColor.from_string(INK.removeprefix("#"))


def add_plain_text(slide, x: float, y: float, width: float, height: float,
                   text: str, *, bold: bool = False,
                   align: PP_ALIGN = PP_ALIGN.CENTER,
                   color: str = INK) -> None:
    shape = slide.shapes.add_textbox(Inches(x), Inches(y), Inches(width), Inches(height))
    frame = shape.text_frame
    frame.margin_left = frame.margin_right = frame.margin_top = frame.margin_bottom = 0
    frame.vertical_anchor = MSO_ANCHOR.MIDDLE
    frame.word_wrap = False
    paragraph = frame.paragraphs[0]
    paragraph.alignment = align
    run = paragraph.add_run()
    run.text = text
    run.font.name = FONT
    run.font.size = Pt(FONT_SIZE)
    run.font.bold = bold
    run.font.color.rgb = RGBColor.from_string(color.removeprefix("#"))


def nice_axis(cells: dict[tuple[str, int, str], tuple[float, float]], stat: str) -> tuple[float, float, float]:
    lows = []
    highs = []
    for window in WINDOWS:
        for arm, *_ in ARMS:
            mean, half = cells[stat, window, arm]
            lows.append(mean - half)
            highs.append(mean + half)
    lower = min(lows)
    upper = max(highs)
    # Preserve the same 100-us, non-broken frame while fitting the R32 campaign.
    minimum, maximum, step = 160.0, 260.0, 20.0
    if lower < minimum or upper > maximum:
        raise ValueError(
            f"verified interval [{lower:.6f}, {upper:.6f}] exceeds fixed axis "
            f"[{minimum:.1f}, {maximum:.1f}]"
        )
    return minimum, maximum, step


def chart_workbook(
    cells: dict[tuple[str, int, str], tuple[float, float]], stat: str,
    with_chart: bool,
) -> bytes:
    stream = BytesIO()
    book = xlsxwriter.Workbook(stream, {"in_memory": True})
    sheet = book.add_worksheet("Data")
    sheet.write(0, 0, "Window")
    sheet.write_column(1, 0, [f"{window}%" for window in WINDOWS])
    for index, (arm, label, _, _, _) in enumerate(ARMS):
        mean_col = 1 + 2 * index
        ci_col = mean_col + 1
        sheet.write(0, mean_col, label)
        sheet.write(0, ci_col, f"{label} CI")
        sheet.write_column(1, mean_col, [cells[stat, window, arm][0] for window in WINDOWS])
        sheet.write_column(1, ci_col, [cells[stat, window, arm][1] for window in WINDOWS])
    sheet.set_column(0, 8, 20)

    if with_chart:
        chart = book.add_chart({"type": "line"})
        for index, (arm, label, color, marker, dash) in enumerate(ARMS):
            mean_col = 1 + 2 * index
            ci_col = mean_col + 1
            options = {
                "name": ["Data", 0, mean_col],
                "categories": ["Data", 1, 0, len(WINDOWS), 0],
                "values": ["Data", 1, mean_col, len(WINDOWS), mean_col],
                "line": {"color": color, "width": 1.25, "dash_type": dash},
                "marker": {
                    "type": marker,
                    "size": 5,
                    "border": {"color": color, "width": 0.75},
                    "fill": {"color": color},
                },
            }
            ci_formula = xl_range_formula("Data", 1, ci_col, len(WINDOWS), ci_col)
            options["y_error_bars"] = {
                "type": "custom",
                "plus_values": ci_formula,
                "minus_values": ci_formula,
                "plus_data": [cells[stat, window, arm][1] for window in WINDOWS],
                "minus_data": [cells[stat, window, arm][1] for window in WINDOWS],
                "line": {"color": color, "width": 0.55},
                "end_style": 1,
            }
            chart.add_series(options)
        minimum, maximum, unit = nice_axis(cells, stat)
        common_font = {"name": FONT, "size": FONT_SIZE, "color": INK}
        chart.set_chartarea({"border": {"none": True}, "fill": {"none": True}})
        chart.set_plotarea({"border": {"color": AXIS, "width": 0.55}, "fill": {"color": "#FFFFFF"}})
        chart.set_title({"none": True})
        chart.set_legend({"none": True})
        chart.set_x_axis({
            "label_position": "low",
            "num_font": common_font,
            "line": {"color": AXIS, "width": 0.55},
            "major_tick_mark": "none",
            "minor_tick_mark": "none",
            "position_axis": "between",
        })
        chart.set_y_axis({
            "name": "p50 query latency (µs)",
            "name_font": common_font,
            "num_font": common_font,
            "min": minimum,
            "max": maximum,
            "major_unit": unit,
            "num_format": "0",
            "line": {"color": AXIS, "width": 0.55},
            "major_gridlines": {"visible": True, "line": {"color": GRID, "width": 0.35}},
            "major_tick_mark": "outside",
            "minor_tick_mark": "none",
        })
        sheet.insert_chart("K2", chart, {"x_scale": 1.0, "y_scale": 1.0})
    book.close()
    return stream.getvalue()


def make_figure(cells: dict[tuple[str, int, str], tuple[float, float]],
                output: Path) -> None:
    presentation = Presentation()
    presentation.slide_width = Inches(3.48)
    presentation.slide_height = Inches(2.22)
    slide = presentation.slides.add_slide(presentation.slide_layouts[6])
    add_legend(slide)
    add_plain_text(slide, 1.05, 2.02, 1.45, 0.18, "Time window (%)", bold=True)
    dummy = CategoryChartData()
    dummy.categories = ["10%", "20%"]
    dummy.add_series("No pruning", [0.0, 0.0])
    slide.shapes.add_chart(
        XL_CHART_TYPE.LINE_MARKERS,
        Inches(0.08), Inches(0.23), Inches(3.32), Inches(1.78), dummy,
    )
    for effect in slide._element.xpath(".//a:effectRef"):
        effect.set("idx", "0")

    stub = BytesIO()
    presentation.save(stub)
    changes: dict[str, bytes] = {}
    with ZipFile(stub) as source:
        for chart_index, stat in enumerate(("p50",), 1):
            chart_path = f"ppt/charts/chart{chart_index}.xml"
            old_chart = ET.fromstring(source.read(chart_path))
            with ZipFile(BytesIO(chart_workbook(cells, stat, True))) as workbook_zip:
                new_chart = ET.fromstring(workbook_zip.read("xl/charts/chart1.xml"))
            # LibreOffice otherwise treats an omitted OOXML smooth flag as a
            # cubic curve. Make every native line series explicitly straight.
            for series in new_chart.xpath(".//c:lineChart/c:ser", namespaces=NS):
                for existing in series.findall("c:smooth", NS):
                    series.remove(existing)
                smooth = ET.Element(f"{{{NS['c']}}}smooth")
                smooth.set("val", "0")
                extension = series.find("c:extLst", NS)
                if extension is None:
                    series.append(smooth)
                else:
                    series.insert(series.index(extension), smooth)
            external_data = old_chart.find("c:externalData", NS)
            if external_data is None:
                raise ValueError("stub chart has no external workbook relation")
            new_chart.append(deepcopy(external_data))
            changes[chart_path] = ET.tostring(
                new_chart, encoding="UTF-8", xml_declaration=True, standalone=True
            )
            rel_path = f"ppt/charts/_rels/chart{chart_index}.xml.rels"
            relationships = ET.fromstring(source.read(rel_path))
            target = relationships[0].get("Target").split("/")[-1]
            changes[f"ppt/embeddings/{target}"] = chart_workbook(cells, stat, False)
        with ZipFile(output, "w", ZIP_DEFLATED) as target:
            for item in source.infolist():
                target.writestr(item, changes.get(item.filename, source.read(item.filename)))
    normalize_result_figure_fonts(output, 909)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--summary", type=Path,
        default=INPUT / "query_pruning_ablation.csv",
    )
    parser.add_argument(
        "--output", type=Path, default=OUTPUT / "fig_query_pruning_ablation.pptx"
    )
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    make_figure(read_summary(args.summary), args.output)


if __name__ == "__main__":
    main()
