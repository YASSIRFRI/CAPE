#!/bin/bash
#SBATCH --job-name=cape_ucx_test
#SBATCH --nodes=4
#SBATCH --ntasks=4
#SBATCH --ntasks-per-node=1
#SBATCH --time=00:30:00
#SBATCH --output=cape_ucx_%j.out
#SBATCH --error=cape_ucx_%j.err
# Adjust the partition name to match your cluster:
#SBATCH --partition=compute

set -euo pipefail

# ── Modules ───────────────────────────────────────────────────────────────────
module purge
module load GCCcore/13.2.0
module load UCX/1.15.0-GCCcore-13.2.0

# ── Resolve UCX paths from the loaded module ──────────────────────────────────
# EasyBuild sets EBROOTUCX; fall back to ucx-config if the module doesn't.
if [ -n "${EBROOTUCX:-}" ]; then
    UCX_INC="${EBROOTUCX}/include"
    UCX_LIB="${EBROOTUCX}/lib"
elif command -v ucx_info &>/dev/null; then
    UCX_PREFIX=$(ucx_info -v 2>/dev/null | awk '/^# Library/ {print $NF}' | sed 's|/lib.*||')
    UCX_INC="${UCX_PREFIX}/include"
    UCX_LIB="${UCX_PREFIX}/lib"
else
    echo "ERROR: cannot locate UCX installation." >&2
    exit 1
fi

# ── Resolve PMIx paths (optional) ────────────────────────────────────────────
# PMIx is used for address exchange when available; falls back to shared-fs.
_pmix_found=0
PMIX_HOME=""

for _pfx in \
    "${EBROOTPMIX:-__none__}" \
    "$(pmix_info --path prefix 2>/dev/null | awk '{print $NF}')" \
    "$(pkg-config --variable=prefix pmix 2>/dev/null)" \
    "$(dirname "$(dirname "$(command -v sinfo 2>/dev/null)")")"
do
    [ "${_pfx}" = "__none__" ] && continue
    [ -z "${_pfx}" ]           && continue
    if [ -f "${_pfx}/include/pmix.h" ] && [ -f "${_pfx}/lib/libpmix.so" ]; then
        PMIX_HOME="${_pfx}"
        _pmix_found=1
        break
    fi
done

if [ "${_pmix_found}" -eq 1 ]; then
    PMIX_FLAGS="-DUSE_PMIX -I${PMIX_HOME}/include"
    PMIX_LINK="-L${PMIX_HOME}/lib -lpmix -Wl,-rpath,${PMIX_HOME}/lib"
    echo "PMIx      : ${PMIX_HOME} (enabled)"
else
    PMIX_FLAGS=""
    PMIX_LINK=""
    echo "PMIx      : not found — using SLURM env vars + shared filesystem"
fi

echo "========================================================"
echo " CAPE UCX test  |  Job ${SLURM_JOB_ID}  |  $(date)"
echo "========================================================"
echo " Nodes     : ${SLURM_JOB_NUM_NODES}"
echo " Tasks     : ${SLURM_NTASKS}"
echo " UCX       : ${UCX_INC}"
echo " Node list : ${SLURM_JOB_NODELIST}"
echo "========================================================"

# SLURM_SUBMIT_DIR is the directory sbatch was called from — always on shared NFS
cd "${SLURM_SUBMIT_DIR}"

# ── Build ─────────────────────────────────────────────────────────────────────
echo ""
echo ">>> Compiling from $(pwd)..."
mkdir -p bin obj lib
make cleanall 2>/dev/null || true

# UCX module ships installed headers, so UCX_SRC and UCX_GEN both point to
# the same include directory — no separate generated tree needed.
make apps \
    UCX_SRC="${UCX_INC}" \
    UCX_GEN="${UCX_INC}" \
    UCX_LIB="${UCX_LIB}" \
    "PMIX_FLAGS=${PMIX_FLAGS}" \
    "PMIX_LINK=${PMIX_LINK}" \
    CC=gcc

echo ">>> Build OK"

# ── Helper: run one test and print timing ─────────────────────────────────────
# Usage: run_test <binary> <description> [extra args...]
run_test() {
    local bin=$1
    local desc=$2
    shift 2

    if [ ! -x "bin/${bin}" ]; then
        echo "  SKIP  ${bin} (binary not found)"
        return
    fi

    echo ""
    echo "--- ${desc} ---"
    local start end elapsed
    start=$(date +%s%3N)

    srun --mpi=pmix \
         --nodes="${SLURM_JOB_NUM_NODES}" \
         --ntasks="${SLURM_NTASKS}" \
         --ntasks-per-node=1 \
         "bin/${bin}" "$@"

    end=$(date +%s%3N)
    elapsed=$(( end - start ))
    echo "    Wall time: ${elapsed} ms"
}

# ── Tests ─────────────────────────────────────────────────────────────────────
echo ""
echo "======== Running tests with ${SLURM_NTASKS} MPI tasks ========"

run_test cape_mamult      "Matrix multiplication (100x100)"
run_test cape_pi          "Pi approximation"
run_test cape_prime       "Prime counting"
run_test cape_reduction   "Reduction"
run_test cape_for         "Parallel for"
run_test cape_vector1     "Vector operation 1"
run_test cape_vector2     "Vector operation 2"

echo ""
echo "======== All tests complete ========"
echo "Output file: cape_ucx_${SLURM_JOB_ID}.out"
