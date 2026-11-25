# Figure sources and input contracts

Run `python figures/render.py methods --build build --output run/render` from the repository root for Figures 1–6. The scripts regenerate the synthetic collection, centroid graph, posting lists, query winners and exact reference residuals. Figure 5 shows an actual neighborhood in that generated graph and the posting interval from a probed list. The synthetic example uses its own fixed illustration weights.

For Figures 7–13, point `REGIME_FIGURE_INPUT` at your aggregate tables and `REGIME_FIGURE_OUTPUT` at a separate output directory. Each chart also runs individually as a Python script. Aggregate file names follow the evaluation pipeline, so a file name and its printed figure number can differ.

| Paper figure | Entry point | Aggregate input and main fields |
| --- | --- | --- |
| 7 | `fig07_encodings.py` | `fig8_codec_ratios.csv` (reference, column, codec, ratio) and `fig8_codec_decode.csv` (reference, component, codec, p50_us, p95_us) |
| 8 | `fig08_latency.py` | `fig8_query_windows.csv` (route, window, p50_us, p95_us). The benchmark adapter writes this file. |
| 9 | `fig09_reference.py` | `value_difference_reference_sweep.csv` (db, method, interval, compression_ratio, query_p50_us). The pooled series uses the label PRSOV. |
| 10 | `fig10_order.py` | `storage_order_windows.csv` (layout, window, value_cr, query_p50_us) |
| 11 | `fig11_pruning.py` | `query_pruning_ablation.csv` (statistic, window, arm, batches, mean_latency_us, ci95_half_us). The adapter writes this from eight measured batches. |
| 12 | `fig12_sql.py` | `system_sql_latency.csv` (system, window, p50_ms, p95_ms) |
| 13 | `fig13_public_datasets.py` | `figure10_ratios.csv` (dataset, reference, column, codec, ratio) and `figure10_decode.csv` (dataset, reference, codec, p50_us, p95_us) |

The synthetic benchmark supplies inputs for Figures 8 and 11. The other evaluation charts require measurements from the indicated codec, layout or SQL comparison. Axis ranges and layout defaults correspond to the paper. Configure them for different workloads as needed.
