#!/bin/bash
#SBATCH --job-name=bench_heat3d_dickpt
#SBATCH --nodes=16
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=32
#SBATCH --time=01:00:00
#SBATCH --output=bench_heat3d_dickpt_%j.out
#SBATCH --error=bench_heat3d_dickpt_%j.err
#SBATCH --hint=nomultithread


set -euo pipefail

APP="heat3d"
TARGET="dickpt_heat3d_manual"
BIN_NAME="dickpt_heat3d_manual"
# Cube dimension N (N x N x N, <= MAX_N=512 in the app).
# At N=128 the per-iter work (~2M cells) is so small the run finishes in
# timer noise (~tens of ms total); N=512 is 64x the cells/iter so the timing
# region is well above noise and the strong-scaling sweep is meaningful.
N_DIM="${N_DIM:-512}"
# Timing phase: enough iterations to dominate startup/noise and give a
# stable per-iter cost.
N_ITERS="${N_ITERS:-200}"
# Size phase: run long enough for the dirty front to traverse the cube.
# The front advances ~1 plane/iter, so a bit more than N planes saturates it.
N_ITERS_SIZE="${N_ITERS_SIZE:-560}"
# Log every iteration so we get the full saturation curve.
export CAPE_CKPT_SIZE_STRIDE="${CAPE_CKPT_SIZE_STRIDE:-1}"
# Monitor scratch buffer: a merged checkpoint at N=512 needs ~260 MB, above the
# 256 MB default, so bump the cap to 512 MB (overridable).
export CAPE_UCX_SCRATCH_BYTES="${CAPE_UCX_SCRATCH_BYTES:-536870912}"

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
SIZE_NODES="${SIZE_NODES:-16}"
REPS="${REPS:-1}"
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

TIME_CSV="${RESULTS_DIR}/bench_${APP}_dickpt_${JOB_TAG}.csv"
echo "impl,app,n,d,nodes,threads,rep,app_ms,sweep_ms,writeback_ms,ckpt_ms,job_id" > "${TIME_CSV}"
SIZE_CSV="${RESULTS_DIR}/bench_${APP}_ckptsize_${JOB_TAG}.csv"
echo "impl,app,nodes,rank,iter,kind,bytes,job_id" > "${SIZE_CSV}"

echo "Benchmarking DICKPT ${APP} (3D diffusion)"
echo "App:     src/apps/cape_${APP}_manual.c -> ${BIN}"
echo "Monitor: src/monitor/cape_incr_bitmap_multithreaded.c -> ${MONITOR}"
echo "Nodes: ${BENCH_NODES}  Threads: ${THREADS_LIST[*]}  Reps: ${REPS}  N=${N_DIM} iters=${N_ITERS}  MPI mode: ${SRUN_MPI_MODE}"
echo "Timing CSV: ${TIME_CSV}"
echo "Size CSV:   ${SIZE_CSV}  (single run at ${SIZE_NODES} nodes, iters=${N_ITERS_SIZE})"

# ── PHASE 1: thread-scaling sweep (fixed nodes, vary compute threads) ───────────
run_time() {
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

    echo "[launch] ${tag}  (1 rank/node, cpus/task=${CPUS_PER_TASK}, compute threads=${nt}, fault threads=${nt})"
    CAPE_COMPUTE_THREADS="${nt}" \
    CAPE_MONITOR_FAULT_THREADS="${nt}" \
    CAPE_UCX_BOOTSTRAP_ID="${bid}" CAPE_UCX_BOOTSTRAP_DIR="${bdir}" \
    srun --exclusive --mpi="${SRUN_MPI_MODE}" --nodes="${nn}" --ntasks="${nn}" \
         --ntasks-per-node=1 --cpus-per-task="${CPUS_PER_TASK}" \
         --cpu-bind=none --distribution=block:block \
         "${MONITOR}" "${BIN}" "${N_DIM}" "${N_ITERS}" "${REPS}" >>"${log}" 2>&1 || rc=$?
    rm -rf "${bdir}"
    if [ "${rc}" -ne 0 ]; then echo "[fail] ${tag} rc=${rc} log=${log}" >&2; return 0; fi

    awk -v impl="dickpt" -v app="${APP}" -v nn="${nn}" -v nt="${nt}" -v job="${JOB_TAG}" '
        /^RESULT / {
            n=""; dd=""; rep=""; ms=""; sw=""; wb=""; ck="";
            for (i=1;i<=NF;i++) { split($i,kv,"="); if(kv[1]=="n")n=kv[2]; if(kv[1]=="d")dd=kv[2]; if(kv[1]=="rep")rep=kv[2]; if(kv[1]=="ms")ms=kv[2]; if(kv[1]=="sweep_ms")sw=kv[2]; if(kv[1]=="writeback_ms")wb=kv[2]; if(kv[1]=="ckpt_ms")ck=kv[2]; }
            if (n!="" && ms!="") printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n", impl,app,n,dd,nn,nt,rep,ms,sw,wb,ck,job;
        }' "${log}" >> "${TIME_CSV}"
    echo "[done]   ${tag}"
}

echo ""
echo "=== PHASE 1: execution time (thread sweep) ==="
for nt in "${THREADS_LIST[@]}"; do run_time "${nt}"; done

# ── PHASE 2: per-iteration checkpoint size (logged once) ───────────────────────
run_size() {
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

    echo "[launch] ${tag}  (CAPE_CKPT_SIZE_LOG=1, REPS=1, iters=${N_ITERS_SIZE}, cpus/task=${CPUS_PER_TASK})"
    CAPE_CKPT_SIZE_LOG=1 \
    CAPE_UCX_BOOTSTRAP_ID="${bid}" CAPE_UCX_BOOTSTRAP_DIR="${bdir}" \
    srun --exclusive --export=ALL --mpi="${SRUN_MPI_MODE}" --nodes="${nn}" --ntasks="${nn}" \
         --ntasks-per-node=1 --cpus-per-task="${CPUS_PER_TASK}" \
         --cpu-bind=none --distribution=block:block \
         "${MONITOR}" "${BIN}" "${N_DIM}" "${N_ITERS_SIZE}" 1 >>"${log}" 2>&1 || rc=$?
    rm -rf "${bdir}"
    if [ "${rc}" -ne 0 ]; then echo "[fail] ${tag} rc=${rc} log=${log}" >&2; return 0; fi

    awk -v impl="dickpt" -v app="${APP}" -v nn="${nn}" -v job="${JOB_TAG}" '
        /^CKPT_SIZE / {
            rank=""; iter=""; kind=""; bytes="";
            for (i=1;i<=NF;i++) { split($i,kv,"="); if(kv[1]=="rank")rank=kv[2]; if(kv[1]=="iter")iter=kv[2]; if(kv[1]=="kind")kind=kv[2]; if(kv[1]=="bytes")bytes=kv[2]; }
            if (rank!="" && iter!="" && bytes!="") printf "%s,%s,%s,%s,%s,%s,%s,%s\n", impl,app,nn,rank,iter,kind,bytes,job;
        }' "${log}" >> "${SIZE_CSV}"
    echo "[done]   ${tag}"
}

echo ""
echo "=== PHASE 2: checkpoint size (logged once) ==="
run_size "${SIZE_NODES}"

echo ""
echo "Done. DICKPT ${APP}"
echo "Timing CSV: ${TIME_CSV}"
echo "Size CSV:   ${SIZE_CSV}"
echo "Per-node logs in: ${RESULTS_DIR}"
echo "Tip: rank 0 owns the i==0 face with the hot patch, so its early-iteration bytes start"
echo "     smallest and grow as the diffusion front spreads through the cube."
