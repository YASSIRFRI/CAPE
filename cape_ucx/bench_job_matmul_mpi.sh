#!/bin/bash
#SBATCH --job-name=bench_matmul_mpi
#SBATCH --qos=large-cpu
#SBATCH --nodes=128
#SBATCH --ntasks=128
#SBATCH --ntasks-per-node=1
#SBATCH --time=04:00:00
#SBATCH --output=bench_matmul_mpi_%j.out
#SBATCH --error=bench_matmul_mpi_%j.err
#SBATCH --partition=compute

# Distributed dense matrix multiply (block / compute-bound) — pure MPI.
# Sweeps nodes = 8,16,32,64,128, REPS runs each. CSV: impl,app,n,d,nodes,rep,app_ms,job_id

set -euo pipefail

APP="matmul"
SRC_NAME="mpi_matmul.c"
BIN_NAME="mpi_matmul"
# Matrix dimension N (square). Must be <= MAX_N (2048) in the app.
N_DIM="${N_DIM:-2048}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${PROJECT_DIR:-${SLURM_SUBMIT_DIR:-${SCRIPT_DIR}}}"
[ -f "${PROJECT_DIR}/makefile" ] || PROJECT_DIR="${SCRIPT_DIR}"
cd "${PROJECT_DIR}"
JOB_TAG="${SLURM_JOB_ID:-local_$$}"

RESULTS_DIR="${RESULTS_DIR:-${SLURM_SUBMIT_DIR:-${PROJECT_DIR}}/results/bench_${APP}_mpi_${JOB_TAG}}"
mkdir -p "${RESULTS_DIR}" 2>/dev/null || { RESULTS_DIR="/tmp/${USER}/bench_${APP}_mpi_${JOB_TAG}"; mkdir -p "${RESULTS_DIR}"; }
BUILD_DIR="${BUILD_DIR:-${SLURM_SUBMIT_DIR:-/tmp/${USER}}/cape_build_${APP}_mpi_${JOB_TAG}}"
mkdir -p "${BUILD_DIR}/bin" 2>/dev/null || { BUILD_DIR="/tmp/${USER}/cape_build_${APP}_mpi_${JOB_TAG}"; mkdir -p "${BUILD_DIR}/bin"; }

NODES_LIST=(${NODES_LIST:-8 16 32 64 128})
REPS="${REPS:-10}"

module purge
module load GCCcore/14.2.0
module load UCX/1.18.0-GCCcore-14.2.0
module load OpenMPI/5.0.3-GCC-13.3.0 2>/dev/null || module load OpenMPI 2>/dev/null || true

if ! command -v mpicc >/dev/null 2>&1; then
    echo "ERROR: mpicc not found on PATH." >&2; exit 1
fi

SRC_DIR="${PROJECT_DIR}/src/apps/mpi"
SRUN_MPI_MODE="${SRUN_MPI_MODE:-pmix}"

echo "Building MPI ${APP}"
mpicc -O2 -Wall -o "${BUILD_DIR}/bin/${BIN_NAME}" "${SRC_DIR}/${SRC_NAME}" -lm

BIN="${BUILD_DIR}/bin/${BIN_NAME}"
TOTAL_NODES="${SLURM_JOB_NUM_NODES:-128}"

CSV="${RESULTS_DIR}/bench_${APP}_mpi_${JOB_TAG}.csv"
echo "impl,app,n,d,nodes,rep,app_ms,job_id" > "${CSV}"

echo "Benchmarking MPI ${APP}"
echo "App: ${SRC_DIR}/${SRC_NAME} -> ${BIN}"
echo "Nodes: ${NODES_LIST[*]}  Reps: ${REPS}  N=${N_DIM}"
echo "CSV: ${CSV}"

run_one() {
    local nn="$1"
    local tag="mpi_${APP}_nodes${nn}"
    local log="${RESULTS_DIR}/${tag}.log"
    local rc=0

    if [ "${nn}" -gt "${TOTAL_NODES}" ]; then
        echo "[skip] ${tag}: requested ${nn} nodes but allocation has ${TOTAL_NODES}"; return 0
    fi
    : > "${log}"

    echo "[launch] ${tag}"
    srun --exclusive --mpi="${SRUN_MPI_MODE}" --nodes="${nn}" --ntasks="${nn}" --ntasks-per-node=1 \
         "${BIN}" "${N_DIM}" "${REPS}" >>"${log}" 2>&1 || rc=$?
    if [ "${rc}" -ne 0 ]; then echo "[fail] ${tag} rc=${rc} log=${log}" >&2; return 0; fi

    awk -v impl="mpi" -v app="${APP}" -v nn="${nn}" -v job="${JOB_TAG}" '
        /^RESULT / {
            n=""; dd=""; rep=""; ms="";
            for (i=1;i<=NF;i++) { split($i,kv,"="); if(kv[1]=="n")n=kv[2]; if(kv[1]=="d")dd=kv[2]; if(kv[1]=="rep")rep=kv[2]; if(kv[1]=="ms")ms=kv[2]; }
            if (n!="" && ms!="") printf "%s,%s,%s,%s,%s,%s,%s,%s\n", impl,app,n,dd,nn,rep,ms,job;
        }' "${log}" >> "${CSV}"
    echo "[done]   ${tag}"
}

for nn in "${NODES_LIST[@]}"; do run_one "${nn}"; done

echo ""
echo "Done. MPI ${APP} CSV: ${CSV}"
echo "Per-node logs in: ${RESULTS_DIR}"
