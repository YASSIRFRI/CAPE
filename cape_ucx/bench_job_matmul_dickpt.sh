#!/bin/bash
#SBATCH --job-name=bench_matmul_dickpt
#SBATCH --nodes=16
#SBATCH --time=01:00:00
#SBATCH --output=bench_matmul_dickpt_%j.out
#SBATCH --error=bench_matmul_dickpt_%j.err
#SBATCH --partition=compute
#SBATCH --hint=nomultithread

# Distributed dense matrix multiply (compute-bound N^3) — DICKPT.
# Thread-scaling sweep: fixed 16 nodes, vary compute threads/rank 1..32.
# CSV: impl,app,n,d,nodes,threads,rep,app_ms,compute_ms,ckpt_ms,job_id

set -euo pipefail

APP="matmul"
TARGET="dickpt_matmul_manual"
BIN_NAME="dickpt_matmul_manual"
# Matrix dimension N (square). Must be <= MAX_N (2048) in the app.
N_DIM="${N_DIM:-2048}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${PROJECT_DIR:-${SLURM_SUBMIT_DIR:-${SCRIPT_DIR}}}"
[ -f "${PROJECT_DIR}/makefile" ] || PROJECT_DIR="${SCRIPT_DIR}"
cd "${PROJECT_DIR}"
JOB_TAG="${SLURM_JOB_ID:-local_$$}"

RESULTS_DIR="${RESULTS_DIR:-${SLURM_SUBMIT_DIR:-${PROJECT_DIR}}/results/bench_${APP}_dickpt_${JOB_TAG}}"
mkdir -p "${RESULTS_DIR}" 2>/dev/null || { RESULTS_DIR="/tmp/${USER}/bench_${APP}_dickpt_${JOB_TAG}"; mkdir -p "${RESULTS_DIR}"; }
BUILD_DIR="${BUILD_DIR:-${SLURM_SUBMIT_DIR:-/tmp/${USER}}/cape_build_${APP}_dickpt_${JOB_TAG}}"
mkdir -p "${BUILD_DIR}/bin" "${BUILD_DIR}/obj" "${BUILD_DIR}/lib" 2>/dev/null || \
  { BUILD_DIR="/tmp/${USER}/cape_build_${APP}_dickpt_${JOB_TAG}"; mkdir -p "${BUILD_DIR}/bin" "${BUILD_DIR}/obj" "${BUILD_DIR}/lib"; }
BOOTSTRAP_ROOT="${BOOTSTRAP_ROOT:-${BUILD_DIR}/ucx_bootstrap}"
mkdir -p "${BOOTSTRAP_ROOT}"

# Thread-scaling sweep: vary compute threads/rank from 1 up to 32.
THREADS_LIST=(${THREADS_LIST:-1 2 4 8 16 32})
BENCH_NODES="${BENCH_NODES:-16}"
REPS="${REPS:-10}"
PROFILE="${PROFILE:-0}"

# One monitor/rank/checkpoint per node; give the task all cores so the compute
# threads have physical cores to land on (max thread count in the sweep).
CPUS_PER_TASK="${CPUS_PER_TASK:-${SLURM_CPUS_ON_NODE:-32}}"

export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
export LD_PRELOAD="${LD_PRELOAD:-}"
export MANPATH="${MANPATH:-}"
export MODULEPATH="${MODULEPATH:-}"

set +u
module purge
module load GCCcore/14.2.0
module load UCX/1.18.0-GCCcore-14.2.0
module load UCC/1.3.0-GCCcore-14.2.0 2>/dev/null || module load UCC 2>/dev/null || true
module load PMIx/5.0.6-GCCcore-14.2.0 2>/dev/null || module load PMIx 2>/dev/null || true
set -u

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
make -C "${PROJECT_DIR}" dickpt_bitmap_multithreaded_monitor "${TARGET}" PROFILE="${PROFILE}" "${MAKE_ARGS[@]}"

MONITOR="${BUILD_DIR}/bin/cape_dickpt_bitmap_multithreaded_monitor"
BIN="${BUILD_DIR}/bin/${BIN_NAME}"
TOTAL_NODES="${SLURM_JOB_NUM_NODES:-128}"

CSV="${RESULTS_DIR}/bench_${APP}_dickpt_${JOB_TAG}.csv"
echo "impl,app,n,d,nodes,threads,rep,app_ms,compute_ms,ckpt_ms,job_id" > "${CSV}"

echo "Benchmarking DICKPT ${APP} (compute-bound N^3)"
echo "App:     src/apps/cape_${APP}_manual.c -> ${BIN}"
echo "Monitor: src/monitor/cape_incr_bitmap_multithreaded.c -> ${MONITOR}"
echo "Nodes: ${BENCH_NODES}  Threads: ${THREADS_LIST[*]}  Reps: ${REPS}  N=${N_DIM}  MPI mode: ${SRUN_MPI_MODE}"
echo "CSV: ${CSV}"

run_one() {
    local nt="$1"
    local nn="${BENCH_NODES}"
    local tag="dickpt_${APP}_threads${nt}"
    local log="${RESULTS_DIR}/${tag}.log"
    local bid="${JOB_TAG}_${tag}"
    local bdir="${BOOTSTRAP_ROOT}/${bid}"
    local rc=0

    if [ "${nn}" -gt "${TOTAL_NODES}" ]; then
        echo "[skip] ${tag}: requested ${nn} nodes but allocation has ${TOTAL_NODES}"; return 0
    fi
    rm -rf "${bdir}"; mkdir -p "${bdir}"; : > "${log}"

    echo "[launch] ${tag}  (1 rank/node, cpus/task=${CPUS_PER_TASK}, compute threads=${nt})"
    CAPE_COMPUTE_THREADS="${nt}" \
    CAPE_UCX_BOOTSTRAP_ID="${bid}" CAPE_UCX_BOOTSTRAP_DIR="${bdir}" \
    srun --exclusive --mpi="${SRUN_MPI_MODE}" --nodes="${nn}" --ntasks="${nn}" \
         --ntasks-per-node=1 --cpus-per-task="${CPUS_PER_TASK}" \
         --cpu-bind=none --distribution=block:block \
         "${MONITOR}" "${BIN}" "${N_DIM}" "${REPS}" >>"${log}" 2>&1 || rc=$?
    rm -rf "${bdir}"
    if [ "${rc}" -ne 0 ]; then echo "[fail] ${tag} rc=${rc} log=${log}" >&2; return 0; fi

    awk -v impl="dickpt" -v app="${APP}" -v nn="${nn}" -v nt="${nt}" -v job="${JOB_TAG}" '
        /^RESULT / {
            n=""; dd=""; rep=""; ms=""; cp=""; ck="";
            for (i=1;i<=NF;i++) { split($i,kv,"="); if(kv[1]=="n")n=kv[2]; if(kv[1]=="d")dd=kv[2]; if(kv[1]=="rep")rep=kv[2]; if(kv[1]=="ms")ms=kv[2]; if(kv[1]=="compute_ms")cp=kv[2]; if(kv[1]=="ckpt_ms")ck=kv[2]; }
            if (n!="" && ms!="") printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n", impl,app,n,dd,nn,nt,rep,ms,cp,ck,job;
        }' "${log}" >> "${CSV}"
    echo "[done]   ${tag}"
}

for nt in "${THREADS_LIST[@]}"; do run_one "${nt}"; done

echo ""
echo "Done. DICKPT ${APP} CSV: ${CSV}"
echo "Per-node logs in: ${RESULTS_DIR}"
