#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 3 ]]; then
  echo "usage: $0 BINARY OUTPUT_DIR CXX_DEAD_ARGUMENT..." >&2
  exit 64
fi

binary=$1
output_dir=$2
shift 2
mkdir -p "${output_dir}"

for frontend in ast-json libtooling; do
  report="${output_dir}/${frontend}.json"
  metrics="${output_dir}/${frontend}.metrics"
  /usr/bin/time -v "${binary}" "$@" \
    --frontend "${frontend}" --format json --output "${report}" --verbose \
    2>"${metrics}"
done

jq -S . "${output_dir}/ast-json.json" >"${output_dir}/ast-json.sorted.json"
jq -S . "${output_dir}/libtooling.json" >"${output_dir}/libtooling.sorted.json"
diff -u "${output_dir}/ast-json.sorted.json" "${output_dir}/libtooling.sorted.json" \
  >"${output_dir}/report.diff" || true

for frontend in ast-json libtooling; do
  rg '^cxx-dead-index-metrics' "${output_dir}/${frontend}.metrics"
done

echo "report diff: ${output_dir}/report.diff"
