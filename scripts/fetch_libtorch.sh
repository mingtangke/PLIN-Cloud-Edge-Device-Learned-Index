#!/usr/bin/env bash
# Download libtorch CPU (cxx11 ABI) into third_party/libtorch/.
# Usage: bash scripts/fetch_libtorch.sh [VERSION]
# Default version matches the PyTorch used by hot_lstm/train.py (>=2.1).

set -euo pipefail

VERSION="${1:-2.3.1}"
URL="https://download.pytorch.org/libtorch/cpu/libtorch-cxx11-abi-shared-with-deps-${VERSION}%2Bcpu.zip"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${HERE}/third_party"
ZIP="${DEST}/libtorch-${VERSION}.zip"

if [[ -d "${DEST}/libtorch" ]]; then
  echo "libtorch already present at ${DEST}/libtorch. Delete it to re-download."
  exit 0
fi

mkdir -p "${DEST}"
echo "Downloading ${URL}"
curl -L -o "${ZIP}" "${URL}"
echo "Extracting into ${DEST}"
unzip -q "${ZIP}" -d "${DEST}"
rm -f "${ZIP}"
echo "Done. libtorch installed at ${DEST}/libtorch"
