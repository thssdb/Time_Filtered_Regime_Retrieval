# Time-Filtered Regime Retrieval

This repository contains the retrieval index, compressed storage implementation for Apache IoTDB, benchmark drivers and plotting scripts for the paper. The example below builds both the index and the storage files from deterministic **synthetic** signals. The paper's airline inputs, fitted parameters, indexes, query traces and results are private. Supply independent data to run your own evaluation.

| Directory | Contents |
| --- | --- |
| [index](index) | FAISS IVF with a centroid HNSW, weighted-cosine routing and scoring, weighted-distance reranking, time pruning and a four-way ablation |
| [storage](storage) | Start-time templates, physical position mapping, the eight-byte PLAIN baseline and the legacy decimal codec |
| [iotdb](iotdb) | Seekable Group-64 Rice encoder and reader, aligned time and value pages, resource metadata writer and SQL UDTF |
| [experiments](experiments) | Preprocessing, build driver, paired benchmark, exhaustive reference, bitwise verification, component timings and SQL bridge |
| [experiments/public_datasets](experiments/public_datasets) | Public-data block ordering and the archived First-32 comparison |
| [figures](figures) | Existing plotting utilities and synthetic method examples; filenames retain their earlier numbering |

## Build

Set `JAVA_HOME` to a JDK installation so CMake can find the JNI headers. Use a little-endian Linux or macOS system, Python 3.10+, CMake 3.24+, a C++20 compiler, OpenMP, BLAS/LAPACK, LZ4 development files, Java 17 and Maven. The source patch is applied **to a build-directory copy** of the pinned upstream FAISS checkout. This builds the HNSW time gate into the actual FAISS search loop. The third-party FAISS license is in [index/FAISS-LICENSE](index/FAISS-LICENSE).

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
| Single index | `weighted_row`, `weighted_slice` | Same index and Group-64 store. Per-posting time checks versus an L0 one-hop time gate plus two list bounds |
| Time-sharded index | `all_scan`, `skip_scan`, `all_binary_packed`, `skip_binary_packed` | Same shard indexes and store. Toggle skipping disjoint shard graphs and binary lookup of the posting interval independently |

The resident reader recovers all 18 timestamps and 54 original FP32 readings for each selected segment. For time, the store writes a segment start and a two-bit identifier of its sample-offset template. If more than four templates occur, consecutive segments share local tables of up to four identifiers into a common template pool. The native reader handles the private TIM1 time-page tag (0xfe).

For values, each signal has a sorted dictionary of its original FP32 bit patterns. Within each chronological block the writer starts at the earliest segment and repeatedly selects the nearest unvisited segment. In each group of up to 64 segments the writer stores the first segment's dictionary keys. At each sample position it ZigZag- and Rice-encodes the signed difference from the corresponding first key. A segment offset locates its codewords directly. The reader unpacks the first keys once and decodes at most one nonfirst payload per signal for a selected segment. A four-byte locator maps a retrieval rank to its time and value positions. Dictionaries, first keys, offsets, and codes are embedded in the aligned data files. The SQL bridge uses this reader on a newly built store.

[BuildValuePages](iotdb/BuildValuePages.java) accepts the physical placement, group size and reference (`first` or `previous`) so the reference and ordering comparisons can share one encoder. [SeekPack](iotdb/SeekPack.java) includes full reconstruction and sparse-seek checks. The former scaled-decimal First-32 encoder remains available in [storage/encode.py](storage/encode.py) with inputs from `experiments/prepare.py --legacy-decimal`. Existing First-32 stores can still be queried.

Benchmark `mean_us`, `p50_us` and `p95_us` include command submission to the resident worker and receipt of the full records. `native_p50_us` is the median of index search time plus record fetch time inside the two workers. `fetch_p50_us` is decode and fetch time in the resident reader. Recall is measured against the exhaustive window reference. Every timed response is checked against the original timestamp bytes and FP32 bits after stopping the timer. To build the four-way ablation chart, pass eight **independently measured** nine-window benchmark files to `experiments/figure_inputs.py OUTPUT BATCH1 ... BATCH8`. It computes per-arm means of batch medians and Student-t confidence intervals.

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

Figures 1–6 use synthetic signals and indexes built on the caller's machine. Figures 1, 5 and 6 are portable redrawings of the paper illustrations. Figure 1 deliberately uses generated curves. The evaluation scripts render caller-supplied aggregate measurements and retain their original figure numbering. Editable charts are written as PPTX and vector plots as EPS. Figure 10 also exports its editable chart via LibreOffice, Ghostscript and `ps2eps`. Figure 2 uses `fc-match` for the numeric font metrics. Install Linux Biolinum O and STIX Two Text to match the chart typography. See [figure inputs](figures/README.md) for the required measurement fields and which charts can be populated directly by the synthetic benchmark.

## Rebuild the storage comparisons

For a prepared public dataset with 60 sample positions, run the following command. It uses the same seekable Group-64 encoder as the SQL reader. Supply a directory named `NASA_battery`, `Hydraulic_system`, or `metropt3` containing channel-major `raw54.f32`, `point_time_ns.i64`, `business_order.u32`, and `tail_offsets.u64`. The writer verifies the value packs and the timestamp column on readback.

```sh
python storage/build_public_store.py /path/to/NASA_battery run-public
python storage/plain_columns.py run/prepared/collection run/templates/collection.placement.tsv run-plain
```

The compression ratio divides eight bytes per original scalar by the measured payload bytes. The PLAIN comparison writes one INT64 per timestamp and one FP64 per reading, giving 1× for the payload. For the public-data combined ratio, the denominator sums the timestamp column and the self-contained signal packs, including dictionaries and segment offsets. Intermediate build files are excluded. SQL data-file comparisons additionally count container metadata. Decompression time measures recovery with the winning positions already known. SQL latency also includes retrieval, transport and delivery to the client.
