#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

TFLM_SRC="${TFLM_SRC:-/home/duydonv/src/tflite-micro}"
TFLM_EXPECTED_COMMIT="${TFLM_EXPECTED_COMMIT:-ac1fae36}"
TFLM_TREE="${TFLM_TREE:-${REPO_ROOT}/third_party/tflm_tree}"
TFLM_TMP_TREE="${TFLM_TMP_TREE:-/tmp/tflm-tree-riscv}"

if [ ! -d "${TFLM_SRC}/.git" ]; then
    echo "missing upstream TFLM clone: ${TFLM_SRC}" >&2
    exit 1
fi

actual_commit="$(git -C "${TFLM_SRC}" rev-parse --short=8 HEAD)"
if [ "${actual_commit}" != "${TFLM_EXPECTED_COMMIT}" ]; then
    echo "unexpected TFLM commit: ${actual_commit}, expected ${TFLM_EXPECTED_COMMIT}" >&2
    echo "set TFLM_EXPECTED_COMMIT=${actual_commit} only if this version change is intentional" >&2
    exit 1
fi

rm -rf "${TFLM_TMP_TREE}"
python3 "${TFLM_SRC}/tensorflow/lite/micro/tools/project_generation/create_tflm_tree.py" \
    -e hello_world "${TFLM_TMP_TREE}"

rm -rf "${TFLM_TREE}"
mkdir -p "$(dirname "${TFLM_TREE}")"
cp -a "${TFLM_TMP_TREE}" "${TFLM_TREE}"

echo "generated ${TFLM_TREE} from ${TFLM_SRC}@${actual_commit}"
