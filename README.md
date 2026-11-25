# Time-Filtered Regime Retrieval

This repository contains the retrieval index, compressed storage implementation for Apache IoTDB, benchmark drivers and plotting scripts for the paper. The example below builds both the index and the storage files from deterministic **synthetic** signals. The paper's airline inputs, fitted parameters, indexes, query traces and results are private. Supply independent data to run your own evaluation.

| Directory | Contents |
| --- | --- |
| [index](index) | FAISS IVF with a centroid HNSW, weighted-cosine routing and scoring, weighted-distance reranking, time pruning and a four-way ablation |
| [storage](storage) | Decimal scaling, First-32 differences, residual dictionaries, Rice encoder, position mapping and native random reader for timestamps and values |
| [iotdb](iotdb) | Aligned time and value pages, IoTDB resource metadata writer and SQL UDTF |
| [experiments](experiments) | Preprocessing, build driver, paired benchmark, exhaustive reference, bitwise verification, component timings and SQL bridge |
| [experiments/public_datasets](experiments/public_datasets) | 60-sample First-32 encoder and full-dataset decoder for the public-dataset value experiment |
| [figures](figures) | Sources for Figures 1–13, synthetic method examples and evaluation charts |

## Build

Use a little-endian Linux or macOS system, Python 3.10+, CMake 3.24+, a C++20 compiler, OpenMP, BLAS/LAPACK, LZ4 development files, Java 17 and Maven. The source patch is applied **to a build-directory copy** of the pinned upstream FAISS checkout. This builds the HNSW time gate into the actual FAISS search loop. The third-party FAISS license is in [index/FAISS-LICENSE](index/FAISS-LICENSE).

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r requirements.txt
git clone https://github.com/facebookresearch/faiss.git external/faiss
git -C external/faiss checkout 6692d7ab3748e95d155c49b7821dc9ef70a2611a
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DFAISS_OPT_LEVEL=generic \
  -DFAISS_SOURCE_DIR="$PWD/external/faiss"
cmake --build build --parallel
mvn -B -f iotdb/pom.xml package dependency:build-classpath \
  -Dmdep.outputFile=target/classpath.txt
```

On macOS with Homebrew add `-DOpenMP_ROOT="$(brew --prefix libomp)" -DCMAKE_PREFIX_PATH="$(brew --prefix)"` to the CMake invocation. The Java project uses the IoTDB 2.0.11 UDF API and TsFile 2.4.0.

## Run the synthetic experiment

Run these commands from the repository root in a new output directory. The build creates two native indexes, the locators, dictionaries and aligned files.

```sh
python experiments/prepare.py run --synthetic
bash experiments/build_store.sh run build
python experiments/verify.py run build
python experiments/benchmark.py run build --queries 16 --repeats 3 --out run/benchmark.csv
python experiments/figure_inputs.py run/aggregates run/benchmark.csv
REGIME_FIGURE_INPUT="$PWD/run/aggregates" REGIME_FIGURE_OUTPUT="$PWD/run/render" \
  python figures/fig08_latency.py
```

The example uses 6,144 artificial segments and generates deterministic queries from this collection for smoke testing. To evaluate held-out queries, pass `--query-file queries.npy` to the benchmark. Each row contains 42 **raw** FP32 coordinates in the order MP samples 1–6, IP samples 1–18, N2 samples 1–18. For your own history, pass `--csv history.csv` instead of `--synthetic` to the preparation script. The CSV columns are `segment,timestamp,mp,ip,n2`, with 18 distinct integer timestamps per segment, finite decimal readings, and one collection per run. Preprocessing assigns contiguous identifiers in order of first appearance. It fits one mean and sample standard deviation per channel on the historical search coordinates, and fits covariate weights by mutual information using historical samples. You can supply explicit positive coordinate weights as 42 little-endian FP64 values with `--weights`. The synthetic fixture uses unit channel weights. The example builds 128 lists, configurable through `NLIST`. The default sharded comparison uses 16 chronological shards.

### Retrieval and storage

A query admits segments whose end time falls in the half-open interval `[lo, hi)`. Each query probes 16 lists with `efSearch=256`. The worker ranks candidates globally by weighted cosine, retains up to 100, then returns up to 10 by weighted Euclidean distance. Original identifiers break ties in both stages. The exhaustive reference applies the same two scoring stages to the complete window.

| Experiment | Arms | Controlled comparison |
| --- | --- | --- |
| Single index | `weighted_row`, `weighted_slice` | Same index and First-32 store. Per-posting time checks versus an L0 one-hop time gate plus two list bounds |
| Time-sharded index | `all_scan`, `skip_scan`, `all_binary_packed`, `skip_binary_packed` | Same shard indexes and store. Toggle skipping disjoint shard graphs and binary lookup of the posting interval independently |

The native reader recovers 18 timestamps and 54 FP32 values for each selected identifier. Timestamps are chronological. Within each block the value writer starts with the earliest segment and repeatedly appends the nearest unvisited segment. Every 32 positions it writes absolute values. Within a group the encoder differences scaled integers against that group's first segment at the same sample position. It assigns dictionary keys to the differences, then Rice-codes key differences relative to the first residual key at that position. A four-byte locator links the timestamp position and the value position. The `--group` and `--reference` options of [storage/encode.py](storage/encode.py) expose the reference ablation.

Benchmark `mean_us`, `p50_us` and `p95_us` include command submission to the resident worker and receipt of the full records. `native_p50_us` is the median sum of time inside native search and native fetch. `fetch_p50_us` is decode and fetch time in the native reader. Recall is measured against the exhaustive window reference. Every timed response is checked against the original timestamp bytes and FP32 bits after stopping the timer. To build the four-way ablation chart, pass eight **independently measured** nine-window benchmark files to `experiments/figure_inputs.py OUTPUT BATCH1 ... BATCH8`. It computes per-arm means of batch medians and Student-t confidence intervals.

### IoTDB SQL interface

The SQL UDTF submits one request to a resident worker over a Unix domain socket and returns all the selected records. The DataNode and the bridge must run under accounts with access to that socket. For a standalone reader the build writes aligned pages. To also generate IoTDB resource sidecars, set `IOTDB_HOME` to an installed DataNode distribution before the initial build. The value pages use the custom decoder in this repository.

```sh
mkdir -p run/sockets
export TVM_QUERY_SOCKET_DIR="$PWD/run/sockets"
python experiments/sql_bridge.py run build "$TVM_QUERY_SOCKET_DIR/collection.sock"
```

Start the DataNode with the same `TVM_QUERY_SOCKET_DIR`. Register the compiled UDTF and call it on a series containing at least one point. Substitute the absolute JAR URI, the window bounds and a comma-separated 42-coordinate query in the SQL below.

```sql
CREATE FUNCTION regime_top10 AS 'temporal.iotdb.TemporalTop10'
USING URI 'file:///absolute/path/to/iotdb/target/regime-reader-1.0.jar';
SELECT regime_top10(x, 'dataset'='collection', 'arm'='ours',
                    'lo'='<inclusive>', 'hi'='<exclusive>', 'q'='<42 coordinates>')
FROM root.regime.trigger;
```

The returned TEXT is Base64 of a little-endian uint32 count, that many uint32 identifiers, and the records in rank order. Each record contains 18 int64 timestamps followed by 54 FP32 readings in channel-major order.

## Paper figures

```sh
python figures/render.py methods --build build --output figures/render
python figures/render.py evaluation --input /path/to/aggregate-csvs --output figures/render
```

Figures 1–6 use synthetic signals and indexes built on the caller's machine. Figures 1, 5 and 6 are portable redrawings of the paper illustrations. Figure 1 deliberately uses generated curves. Figures 7–13 render the supplied aggregate measurements. Editable charts are written as PPTX and vector plots as EPS. Figure 10 also exports its editable chart via LibreOffice, Ghostscript and `ps2eps`. Figure 2 uses `fc-match` for the numeric font metrics. Install Linux Biolinum O and STIX Two Text to match the chart typography. See [figure inputs](figures/README.md) for the required measurement fields and which charts can be populated directly by the synthetic benchmark.

Compression ratio uses eight bytes per original scalar divided by measured encoded bytes. For a combined ratio, sum scalar counts and encoded byte counts before dividing.
