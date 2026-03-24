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

# ── Resolve PMIx paths ────────────────────────────────────────────────────────
# Try sources in priority order; each must provide both a header and a library.
_pmix_found=0

# 1. EasyBuild PMIx module (EBROOTPMIX set by module load PMIx/...)
if [ "${_pmix_found}" -eq 0 ] && [ -n "${EBROOTPMIX:-}" ]; then
    PMIX_INC="${EBROOTPMIX}/include"
    PMIX_LIB="${EBROOTPMIX}/lib"
    _pmix_found=1
fi

# 2. pmix_info utility — available when PMIx is installed as a standalone package
if [ "${_pmix_found}" -eq 0 ] && command -v pmix_info &>/dev/null; then
    _pfx=$(pmix_info --path prefix 2>/dev/null | awk '{print $NF}')
    if [ -f "${_pfx}/include/pmix.h" ]; then
        PMIX_INC="${_pfx}/include"
        PMIX_LIB="${_pfx}/lib"
        _pmix_found=1
    fi
fi

# 3. pkg-config
if [ "${_pmix_found}" -eq 0 ] && pkg-config --exists pmix 2>/dev/null; then
    PMIX_INC=$(pkg-config --variable=includedir pmix)
    PMIX_LIB=$(pkg-config --variable=libdir pmix)
    _pmix_found=1
fi

# 4. SLURM bundles PMIx — headers are next to the SLURM install
if [ "${_pmix_found}" -eq 0 ] && command -v sinfo &>/dev/null; then
    _slurm_pfx=$(dirname "$(dirname "$(command -v sinfo)")")
    if [ -f "${_slurm_pfx}/include/pmix.h" ]; then
        PMIX_INC="${_slurm_pfx}/include"
        PMIX_LIB="${_slurm_pfx}/lib"
        _pmix_found=1
    fi
fi

# 5. System-wide fallback (/usr)
if [ "${_pmix_found}" -eq 0 ] && [ -f "/usr/include/pmix.h" ]; then
    PMIX_INC="/usr/include"
    PMIX_LIB="/usr/lib64"
    _pmix_found=1
fi

if [ "${_pmix_found}" -eq 0 ]; then
    echo "ERROR: cannot locate PMIx headers (pmix.h). Load a PMIx module or set EBROOTPMIX." >&2
    exit 1
fi

echo "========================================================"
echo " CAPE UCX test  |  Job ${SLURM_JOB_ID}  |  $(date)"
echo "========================================================"
echo " Nodes     : ${SLURM_JOB_NUM_NODES}"
echo " Tasks     : ${SLURM_NTASKS}"
echo " UCX       : ${UCX_INC}"
echo " PMIx      : ${PMIX_INC}"
echo " Node list : ${SLURM_JOB_NODELIST}"
echo "========================================================"

WORKDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${WORKDIR}"

# ── Build ─────────────────────────────────────────────────────────────────────
echo ""
echo ">>> Compiling..."
make cleanall 2>/dev/null || true

# Both UCX_SRC and UCX_GEN point to the installed include dir — the module
# ships pre-built headers so there is no separate generated tree.
make apps \
    UCX_SRC="${UCX_INC}" \
    UCX_GEN="${UCX_INC}" \
    UCX_LIB_DIR="${UCX_LIB}" \
    PMIX_HOME="${PMIX_INC}/.." \
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
