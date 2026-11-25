#!/usr/bin/env python3
"""Generate the editable one-column R=32 pruning figure from fresh aggregates."""
from pathlib import Path
import csv
import math
import sys

from pptx import Presentation
from pptx.chart.data import CategoryChartData
from pptx.dml.color import RGBColor
from pptx.enum.chart import XL_CHART_TYPE, XL_MARKER_STYLE, XL_TICK_MARK
from pptx.enum.shapes import MSO_CONNECTOR, MSO_SHAPE
from pptx.enum.text import PP_ALIGN, MSO_ANCHOR
from pptx.oxml.xmlchemy import OxmlElement
from pptx.util import Inches, Pt


from paths import INPUT, OUTPUT
from paper_result_figure_fonts import normalize_result_figure_fonts

FONT = "STIX Two Text"
INK = RGBColor.from_string("202D35")
ROUTES = (
    ("row", "query without pruning", "3568A8", XL_MARKER_STYLE.CIRCLE),
    ("pruned", "query with pruning", "C86A21", XL_MARKER_STYLE.SQUARE),
)
WINDOWS = tuple(range(10, 100, 10))


with (INPUT / "fig8_query_windows.csv").open() as handle:
    source = list(csv.DictReader(handle))
ROWS = {(row["route"], int(row["window"])): row for row in source}
assert set(ROWS) == {(route, window) for route, *_ in ROUTES for window in WINDOWS}


def rgb(value):
    return RGBColor.from_string(value)


presentation = Presentation()
presentation.slide_width = Inches(3.48)
presentation.slide_height = Inches(1.78)
slide = presentation.slides.add_slide(presentation.slide_layouts[6])


def text(x, y, width, height, value, size, bold=False, align=PP_ALIGN.CENTER):
    frame = slide.shapes.add_textbox(*map(Inches, (x, y, width, height))).text_frame
    frame.margin_top = frame.margin_bottom = frame.margin_left = frame.margin_right = 0
    frame.vertical_anchor = MSO_ANCHOR.MIDDLE
    frame.word_wrap = False
    paragraph = frame.paragraphs[0]
    paragraph.alignment = align
    run = paragraph.add_run()
    run.text = value
    run.font.name = FONT
    run.font.size = Pt(size)
    run.font.bold = bold
    run.font.color.rgb = INK


for (route, name, color, _), x in zip(ROUTES, (0.24, 1.92)):
    line = slide.shapes.add_connector(
        MSO_CONNECTOR.STRAIGHT, Inches(x), Inches(0.099), Inches(x + 0.24), Inches(0.099)
    )
    line.line.color.rgb = rgb(color)
    line.line.width = Pt(1.1)
    marker = slide.shapes.add_shape(
        MSO_SHAPE.OVAL if route == "row" else MSO_SHAPE.RECTANGLE,
        *map(Inches, (x + 0.102, 0.075, 0.048, 0.048)),
    )
    marker.fill.solid()
    marker.fill.fore_color.rgb = rgb(color)
    marker.line.fill.background()
    text(x + 0.28, 0, 1.27, 0.195, name, 8.4, align=PP_ALIGN.LEFT)


for index, statistic in enumerate(("p50", "p95")):
    x = 0.02 + index * 1.73
    data = CategoryChartData()
    data.categories = [str(window) for window in WINDOWS]
    for route, name, _, _ in ROUTES:
        data.add_series(name, [float(ROWS[route, window][statistic + "_us"]) for window in WINDOWS])
    chart = slide.shapes.add_chart(
        XL_CHART_TYPE.LINE_MARKERS, *map(Inches, (x, 0.39, 1.70, 1.18)), data
    ).chart
    chart.has_legend = False
    chart.has_title = False
    chart.font.name = FONT
    chart.font.size = Pt(7.6)
    for series, (_, _, color, marker_style) in zip(chart.series, ROUTES):
        series.smooth = False
        series.format.line.color.rgb = rgb(color)
        series.format.line.width = Pt(1.1)
        series.marker.style = marker_style
        series.marker.size = 3
        series.marker.format.fill.solid()
        series.marker.format.fill.fore_color.rgb = rgb(color)
        series.marker.format.line.color.rgb = rgb(color)
    category_axis = chart.category_axis
    value_axis = chart.value_axis
    values = [
        float(ROWS[route, window][statistic + "_us"])
        for route, *_ in ROUTES
        for window in WINDOWS
    ]
    unit = 50 if max(values) - min(values) < 300 else 100
    value_axis.minimum_scale = unit * math.floor(min(values) / unit)
    value_axis.maximum_scale = unit * math.ceil(max(values) / unit)
    value_axis.major_unit = unit
    for axis in (category_axis, value_axis):
        axis.has_major_gridlines = False
        axis.major_tick_mark = XL_TICK_MARK.NONE
        axis.minor_tick_mark = XL_TICK_MARK.NONE
        axis.tick_labels.font.name = FONT
        axis.tick_labels.font.size = Pt(7.7)
        axis.tick_labels.font.color.rgb = INK
        axis.format.line.color.rgb = rgb("70777E")
        axis.format.line.width = Pt(0.45)
    value_axis.tick_labels.number_format = "0"
    category_axis.tick_labels.offset = 55
    skip = OxmlElement("c:tickLblSkip")
    skip.set("val", "2")
    category_axis._element.insert_element_before(
        skip, "c:tickMarkSkip", "c:noMultiLvlLbl", "c:extLst"
    )
    text(x, 0.205, 1.70, 0.18, f"({'ab'[index]}) {statistic} latency (µs)", 8.6, True)

text(0.32, 1.57, 2.84, 0.20, "Time window (%)", 8.4)
for effect in slide._element.xpath(".//a:effectRef"):
    effect.set("idx", "0")
slide.notes_slide.notes_text_frame.text = (
    "Fresh matched measurements on four PRSOV partitions. Both curves use the same "
    "group-first R=32 value layout and return 18 timestamps and 54 FP32 readings for "
    "each Top-k result. The unpruned route scans posting end times in the selected lists. "
    "The pruned route applies HNSW time pruning and IVF binary search. Each point pools "
    "1,152 formal complete-query observations from 96 distinct queries."
)

output = OUTPUT / "fig_pruning_latency.pptx"
presentation.save(output)
normalize_result_figure_fonts(output, 911)
