# Time-filtered retrieval on OceanBase

This adapter implements the retrieval algorithm on an OceanBase CE MySQL-mode tenant. The client loads the weighted HNSW centroid graph and its one-hop time bounds from the database into a resident FAISS worker. For each query, the worker selects up to 16 lists with `efSearch=256`. OceanBase selects postings with the composite primary key `(list_id, end_ns, id)` and the half-open interval `[start_ns, end_ns)`. One SQL statement ranks the selected lists together, keeps the best 100 candidates by weighted cosine and returns up to ten by weighted Euclidean distance. Both stages break ties by ascending ID.

The client then requests only the compressed blocks containing the winners. A resident decoder reconstructs the 18 timestamps and 54 original FP32 readings and returns records in SQL rank order. The graph, scalers, weights, postings, dictionaries and compressed blocks all reside in OceanBase after import. An open `RegimeSearch` instance reuses one connection and its two local workers. Use a separate instance per concurrent client.

The OceanBase experiment used a restart every **16 segments**, differences between adjacent scaled values at matching sample positions, a residual dictionary and Rice coding. Its time pages use TS2DIFF. The default IoTDB build in this repository uses **64-segment group-first differences between original-value keys** and timestamp templates. The commands below reproduce the OceanBase layout.

## Run on synthetic data

Complete the native and Java builds in the [repository instructions](../README.md#build), then run the following commands from the repository root. The build uses the same patched FAISS library as the standalone index.

```sh
python -m pip install 'PyMySQL[rsa]>=1.1,<2'
cmake --build build --target ob_router ob_pages build_index search pack --parallel
python experiments/prepare.py run-ob --synthetic --legacy-decimal
bash oceanbase/build_store.sh run-ob build
```

Use an empty database in your OceanBase tenant. The installer needs permission to create tables and insert and select data there. Set `OB_HOST`, `OB_PORT`, `OB_USER` and `OB_PASSWORD` in your environment. Set `OB_SSL_CA` to a trusted CA file to verify TLS. Supply the database name as the first argument.

```sh
python oceanbase/install.py regime_demo run-ob build
python oceanbase/verify.py regime_demo run-ob build
```

The installer derives list membership from the actual FAISS inverted lists and publishes a readiness marker after verifying the table counts. An interrupted import may leave incomplete tables. The reader requires the readiness marker and a retry uses a fresh empty database.

The verifier checks list choices and ordered IDs against the standalone FAISS worker across nine window widths. It compares each returned timestamp and FP32 bit pattern with the generated source and also checks an empty window.

## Query an installed database

`query.py` accepts a JSON object on standard input with integer `start_ns`, exclusive `end_ns`, and `coordinates` containing 42 raw FP32-compatible numbers. The coordinate order is the first six MP readings, all 18 IP readings and all 18 N2 readings. The adapter applies the scalers and weights that were stored at import. Each match contains its ID, 18 timestamps, and 54 channel-major readings.

```sh
python oceanbase/query.py regime_demo build < query.json
```
