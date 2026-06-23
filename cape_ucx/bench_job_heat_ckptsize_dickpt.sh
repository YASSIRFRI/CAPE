#!/bin/bash
#SBATCH --job-name=bench_heat_ckptsize_dickpt
#SBATCH --nodes=64
#SBATCH --ntasks=64
#SBATCH --ntasks-per-node=1
#SBATCH --time=04:00:00
#SBATCH --output=bench_heat_ckptsize_dickpt_%j.out
#SBATCH --error=bench_heat_ckptsize_dickpt_%j.err
#SBATCH --partition=compute

# Per-iteration DICKPT checkpoint size for the 2D heat-diffusion solver.
#
# For diffusion from a hot edge, only cells near the front change early on, so
# the incremental checkpoint (the dirty delta DICKPT actually ships) should be
# TINY in the first iterations and grow as the front propagates across the
# grid. This job runs the heat solver with the monitor's per-iteration size
# logging on (CAPE_CKPT_SIZE_LOG=1), REPS=1 so iter maps 1:1, at 8/16/32/64
# nodes, and writes one CSV: impl,app,nodes,rank,iter,kind,bytes,job_id
# (kind=local: per-rank delta; kind=merged: global union shipped, rank 0).
# Iterations are sampled (dense for the first 16, then every
# CAPE_CKPT_SIZE_STRIDE=10) so long runs stay readable while still showing
# the full evolution.

set -euo pipefail

APP="heat"
TARGET="dickpt_heat_manual"
BIN_NAME="dickpt_heat_manual"
# Grid dimension N (square, <= MAX_N=8192) and number of Jacobi iterations.
# Use enough iterations to watch the dirty front sweep across the grid.
N_DIM="${N_DIM:-1024}"
N_ITERS="${N_ITERS:-200}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${PROJECT_DIR:-${SLURM_SUBMIT_DIR:-${SCRIPT_DIR}}}"
[ -f "${PROJECT_DIR}/makefile" ] || PROJECT_DIR="${SCRIPT_DIR}"
cd "${PROJECT_DIR}"
JOB_TAG="${SLURM_JOB_ID:-local_$$}"

RESULTS_DIR="${RESULTS_DIR:-${SLURM_SUBMIT_DIR:-${PROJECT_DIR}}/results/bench_${APP}_ckptsize_${JOB_TAG}}"
mkdir -p "${RESULTS_DIR}" 2>/dev/null || { RESULTS_DIR="/tmp/${USER}/bench_${APP}_ckptsize_${JOB_TAG}"; mkdir -p "${RESULTS_DIR}"; }
BUILD_DIR="${BUILD_DIR:-${SLURM_SUBMIT_DIR:-/tmp/${USER}}/cape_build_${APP}_ckptsize_${JOB_TAG}}"
mkdir -p "${BUILD_DIR}/bin" "${BUILD_DIR}/obj" "${BUILD_DIR}/lib" 2>/dev/null || \
  { BUILD_DIR="/tmp/${USER}/cape_build_${APP}_ckptsize_${JOB_TAG}"; mkdir -p "${BUILD_DIR}/bin" "${BUILD_DIR}/obj" "${BUILD_DIR}/lib"; }
BOOTSTRAP_ROOT="${BOOTSTRAP_ROOT:-${BUILD_DIR}/ucx_bootstrap}"
mkdir -p "${BOOTSTRAP_ROOT}"

NODES_LIST=(${NODES_LIST:-8 16 32 64})
# One run per node count is enough to characterise the size curve.
REPS=1
PROFILE="${PROFILE:-0}"

module purge
module load GCCcore/14.2.0
module load UCX/1.18.0-GCCcore-14.2.0
module load UCC/1.3.0-GCCcore-14.2.0 2>/dev/null || module load UCC 2>/dev/null || true
module load PMIx/5.0.6-GCCcore-14.2.0 2>/dev/null || module load PMIx 2>/dev/null || true

if [ -n "${EBROOTUCX:-}" ]; then
    UCX_INC="${EBROOTUCX}/include"; UCX_LIB="${EBROOTUCX}/lib"
elif command -v ucx_info &>/dev/null; then
    UCX_PREFIX=$(ucx_info -v 2>/dev/null | awk '/^# Library/ {print $NF}' | sed 's|/lib.*||')
    UCX_INC="${UCX_PREFIX}/include"; UCX_LIB="${UCX_PREFIX}/lib"
else
    echo "ERROR: cannot locate UCX installation." >&2; exit 1
fi

PMIX_FLAGS=""; PMIX_LINK=""; SRUN_MPI_MODE="${SRUN_MPI_MODE:-none}"
for _pfx in "${EBROOTPMIX:-__none__}" \
            "$(pmix_info --path prefix 2>/dev/null | awk '{print $NF}')" \
            "$(pkg-config --variable=prefix pmix 2>/dev/null)"; do
    [ "${_pfx}" = "__none__" ] && continue
    [ -z "${_pfx}" ] && continue
    if [ -f "${_pfx}/include/pmix.h" ] && [ -f "${_pfx}/lib/libpmix.so" ]; then
        PMIX_FLAGS="-DUSE_PMIX -I${_pfx}/include"
        PMIX_LINK="-L${_pfx}/lib -lpmix -Wl,-rpath,${_pfx}/lib"
        SRUN_MPI_MODE="pmix"
        break
    fi
done

MAKE_ARGS=(
    EXE_FOLDER="${BUILD_DIR}/bin" O_FOLDER="${BUILD_DIR}/obj" L_FOLDER="${BUILD_DIR}/lib"
    UCX_SRC="${UCX_INC}" UCX_GEN="${UCX_INC}" UCX_LIB="${UCX_LIB}"
    "PMIX_FLAGS=${PMIX_FLAGS}" "PMIX_LINK=${PMIX_LINK}" CC=gcc
)
if [ -n "${EBROOTUCC:-}" ]; then
    MAKE_ARGS+=(UCC_SRC="${EBROOTUCC}/include" UCC_LIB="${EBROOTUCC}/lib")
fi

make -C "${PROJECT_DIR}" cleanall \
    EXE_FOLDER="${BUILD_DIR}/bin" O_FOLDER="${BUILD_DIR}/obj" L_FOLDER="${BUILD_DIR}/lib" 2>/dev/null || true
make -C "${PROJECT_DIR}" dickpt_bitmap_monitor "${TARGET}" PROFILE="${PROFILE}" "${MAKE_ARGS[@]}"

MONITOR="${BUILD_DIR}/bin/cape_dickpt_bitmap_monitor"
BIN="${BUILD_DIR}/bin/${BIN_NAME}"
TOTAL_NODES="${SLURM_JOB_NUM_NODES:-64}"

CSV="${RESULTS_DIR}/bench_${APP}_ckptsize_${JOB_TAG}.csv"
# kind=local : this rank's per-iteration delta. kind=merged : global union
# (total bytes shipped), reported once by rank 0.
echo "impl,app,nodes,rank,iter,kind,bytes,job_id" > "${CSV}"

echo "Per-iteration DICKPT checkpoint size for ${APP}"
echo "App:     src/apps/cape_${APP}_manual.c -> ${BIN}"
echo "Monitor: src/monitor/cape_incr_bitmap.c -> ${MONITOR}  (CAPE_CKPT_SIZE_LOG=1)"
echo "Nodes: ${NODES_LIST[*]}  N=${N_DIM} iters=${N_ITERS}  MPI mode: ${SRUN_MPI_MODE}"
echo "CSV: ${CSV}"

run_one() {
    local nn="$1"
    local tag="dickpt_${APP}_ckptsize_nodes${nn}"
    local log="${RESULTS_DIR}/${tag}.log"
    local bid="${JOB_TAG}_${tag}"
    local bdir="${BOOTSTRAP_ROOT}/${bid}"
    local rc=0

    if [ "${nn}" -gt "${TOTAL_NODES}" ]; then
        echo "[skip] ${tag}: requested ${nn} nodes but allocation has ${TOTAL_NODES}"; return 0
    fi
    rm -rf "${bdir}"; mkdir -p "${bdir}"; : > "${log}"

    echo "[launch] ${tag}"
    CAPE_CKPT_SIZE_LOG=1 \
    CAPE_UCX_BOOTSTRAP_ID="${bid}" CAPE_UCX_BOOTSTRAP_DIR="${bdir}" \
    srun --exclusive --export=ALL --mpi="${SRUN_MPI_MODE}" --nodes="${nn}" --ntasks="${nn}" --ntasks-per-node=1 \
         "${MONITOR}" "${BIN}" "${N_DIM}" "${N_ITERS}" "${REPS}" >>"${log}" 2>&1 || rc=$?
    rm -rf "${bdir}"
    if [ "${rc}" -ne 0 ]; then echo "[fail] ${tag} rc=${rc} log=${log}" >&2; return 0; fi

    awk -v impl="dickpt" -v app="${APP}" -v nn="${nn}" -v job="${JOB_TAG}" '
        /^CKPT_SIZE / {
            rank=""; iter=""; kind=""; bytes="";
            for (i=1;i<=NF;i++) { split($i,kv,"="); if(kv[1]=="rank")rank=kv[2]; if(kv[1]=="iter")iter=kv[2]; if(kv[1]=="kind")kind=kv[2]; if(kv[1]=="bytes")bytes=kv[2]; }
            if (rank!="" && iter!="" && bytes!="") printf "%s,%s,%s,%s,%s,%s,%s,%s\n", impl,app,nn,rank,iter,kind,bytes,job;
        }' "${log}" >> "${CSV}"
    echo "[done]   ${tag}"
}

for nn in "${NODES_LIST[@]}"; do run_one "${nn}"; done

echo ""
echo "Done. DICKPT ${APP} checkpoint-size CSV: ${CSV}"
echo "Per-node logs in: ${RESULTS_DIR}"
echo "Tip: rank 0 owns the hot top edge, so its early-iteration bytes start"
echo "     smallest and grow as the diffusion front spreads."
