#!/usr/bin/env bash
# Input is a fresh collection created by experiments/prepare.py --legacy-decimal.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
run=$(cd "${1:?prepared collection directory}" && pwd)
build=$(cd "${2:?CMake build directory}" && pwd)
db=collection
target="$repo/iotdb/target"
cp="$target/classes:$(cat "$target/classpath.txt")"
export OMP_NUM_THREADS=1
export OPENBLAS_NUM_THREADS=1
lib="$build/libpack.so"
if [[ ! -f "$lib" ]]; then lib="$build/libpack.dylib"; fi
if [[ ! -f "$run/inputs/grid.json" || -e "$run/templates/$db.placement.tsv" ]]; then
    echo 'Use a fresh directory prepared with --legacy-decimal.' >&2
    exit 1
fi
java -cp "$cp" WriteBlocks similarity "$db" root.regime "$run/templates" "$run/prepared"
python3 "$repo/storage/locator.py" "$run"
python3 "$repo/storage/encode.py" "$run" "$run/inputs" "$lib" "$run" --group 16 --reference previous
mkdir -p "$run/previous16/$db"
java -cp "$cp" Embed "$run/templates" "$run/packs/previous16/$db" "$run/previous16/$db" 3 --pages-only
"$build/build_index" "$run/time/$db/time_route.f32" "${NLIST:-128}" "$run/index.faiss"
