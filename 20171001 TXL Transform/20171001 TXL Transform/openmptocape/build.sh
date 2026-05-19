#!/bin/bash
# Transpile an OpenMP source file to CAPE, then compile against the
# cape_bitmap library backend (which is the in-process userfaultfd
# variant — same cape.h interface as cape.c).
#
# Usage:
#   ./build.sh verify_private.c             # produces verify_private_cape.c + verify_private binary
#   ./build.sh verify_private.c [outname]   # custom output binary name
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${1:?usage: build.sh <omp-source.c> [outbinary]}"
SRC_BASE="$(basename "${SRC}" .c)"
OUT="${2:-${SRC_BASE}}"
CAPE_DIR="${CAPE_DIR:-${SCRIPT_DIR}/../../../cape_ucx}"

# 1) Transpile (TXL).
TXL_BIN="${TXL_BIN:-txl}"
if ! command -v "${TXL_BIN}" >/dev/null 2>&1; then
    echo "ERROR: txl not on PATH. Set TXL_BIN=/path/to/txl or install TXL." >&2
    exit 1
fi
CAPE_SRC="${SRC_BASE}_cape.c"
"${TXL_BIN}" -o "${CAPE_SRC}" "${SRC}" "${SCRIPT_DIR}/omptocape.Txl"
echo "[txl] ${SRC} -> ${CAPE_SRC}"

# 2) Compile cape_bitmap.c + the transpiled app into one binary.
CC="${CC:-gcc}"
UCX_INC="${UCX_INC:-${EBROOTUCX:-/usr}/include}"
UCX_LIB="${UCX_LIB:-${EBROOTUCX:-/usr}/lib}"
PMIX_FLAGS="${PMIX_FLAGS:-}"
PMIX_LINK="${PMIX_LINK:-}"
CFLAGS="-O2 -I${CAPE_DIR}/include -I${UCX_INC} ${PMIX_FLAGS} -DCAPE_PROFILE"
LDFLAGS="-L${UCX_LIB} -lucp -lucs -lpthread ${PMIX_LINK} -Wl,-rpath,${UCX_LIB}"

"${CC}" -w ${CFLAGS} \
    "${CAPE_DIR}/src/monitor/cape_bitmap.c" \
    "${CAPE_SRC}" \
    -o "${OUT}" ${LDFLAGS}
echo "[cc]  -> ${OUT}"
