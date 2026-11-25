#!/usr/bin/env bash
# Build the aligned store and both index variants from a prepared collection.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
run=$(cd "${1:?prepared collection directory}" && pwd)
build=$(cd "${2:?CMake build directory}" && pwd)
db=collection
classdir="$repo/iotdb/target"
cp="$classdir/classes:$(cat "$classdir/classpath.txt")"
export OMP_NUM_THREADS=${OMP_NUM_THREADS:-1}
export OPENBLAS_NUM_THREADS=${OPENBLAS_NUM_THREADS:-1}
java -cp "$cp" WriteBlocks similarity "$db" root.regime "$run/templates" "$run/prepared"
python3 "$repo/storage/locator.py" "$run"
packs="$run/packs/first64/$db"
store="$run/first64/$db"
java -cp "$cp" BuildValuePages "$run/prepared/$db/raw54.f32" "$run/templates/$db.placement.tsv" "$packs" 3 18 64 first
python3 "$repo/storage/time_templates.py" "$run" "$db" "$packs"
mkdir -p "$store"
if [[ -n "${IOTDB_HOME:-}" ]]; then
    java -cp "$classdir/classes:$IOTDB_HOME/lib/*" Embed "$run/templates" "$packs" "$store" 3
else
    java -cp "$cp" Embed "$run/templates" "$packs" "$store" 3 --pages-only
fi
"$build/build_index" "$run/time/$db/time_route.f32" "${NLIST:-128}" "$run/index.faiss"
"$build/build_shards" "$run/time/$db/time_route.f32" "$run/time/$db/times.i64" "${NLIST:-128}" "$run/shards"
