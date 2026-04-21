#!/bin/bash
#SBATCH --job-name=mpi_bench
#SBATCH --nodes=8
#SBATCH --ntasks=8
#SBATCH --ntasks-per-node=1
#SBATCH --time=00:45:00
#SBATCH --output=mpi_bench_%j.out
#SBATCH --error=mpi_bench_%j.err
#SBATCH --partition=compute

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${PROJECT_DIR:-${SLURM_SUBMIT_DIR:-${SCRIPT_DIR}}}"
if [ ! -f "${PROJECT_DIR}/makefile" ]; then
    PROJECT_DIR="${SCRIPT_DIR}"
fi
cd "${PROJECT_DIR}"
JOB_TAG="${SLURM_JOB_ID:-local_$$}"

RESULTS_DIR="${RESULTS_DIR:-${SLURM_SUBMIT_DIR:-${PROJECT_DIR}}/results}"
if ! mkdir -p "${RESULTS_DIR}" 2>/dev/null; then
    RESULTS_DIR="/tmp/${USER:-$(id -un)}/cape_results"
    mkdir -p "${RESULTS_DIR}"
fi
BUILD_DIR="${BUILD_DIR:-${SLURM_SUBMIT_DIR:-/tmp/${USER:-$(id -un)}}/cape_build_mpi_${JOB_TAG}}"
if ! mkdir -p "${BUILD_DIR}/bin" 2>/dev/null; then
    BUILD_DIR="/tmp/${USER:-$(id -un)}/cape_build_mpi_${JOB_TAG}"
    mkdir -p "${BUILD_DIR}/bin"
fi

N_VALUES_STR="${N_VALUES_STR:-3000}"
REPS="${REPS:-1}"
read -r -a N_VALUES <<< "${N_VALUES_STR}"

module purge
module load GCCcore/14.2.0
module load UCX/1.18.0-GCCcore-14.2.0
# Load an MPI stack. Prefer OpenMPI; fall back to whatever `mpicc` is on PATH.
module load OpenMPI/5.0.3-GCC-13.3.0 2>/dev/null || \
    module load OpenMPI 2>/dev/null || true

if ! command -v mpicc >/dev/null 2>&1; then
    echo "ERROR: mpicc not found on PATH. Load an MPI module before running." >&2
    exit 1
fi

SRC="${PROJECT_DIR}/src/apps/mpi/mpi_mul_manual.c"
APP="${BUILD_DIR}/bin/mpi_mul_manual"

echo "Compiling ${SRC} -> ${APP}"
mpicc -O2 -Wall -o "${APP}" "${SRC}"

SRUN_MPI_MODE="${SRUN_MPI_MODE:-pmix}"

CSV="${RESULTS_DIR}/mpi_mamult_${JOB_TAG}.csv"
echo "impl,n,rep,app_ms,job_id,nodes,ntasks" > "${CSV}"

echo "Benchmarking pure MPI matrix multiplication"
echo "N values: ${N_VALUES[*]}"
echo "Reps per N: ${REPS}"
echo "Build dir: ${BUILD_DIR}"
echo "CSV: ${CSV}"
echo "MPI launch mode: ${SRUN_MPI_MODE}"

for n in "${N_VALUES[@]}"; do
    echo ""
    echo "=== MPI n=${n} reps=${REPS} ==="

    run_log="${BUILD_DIR}/run_mpi_n${n}.log"
    : > "${run_log}"

    set +e
    srun --mpi="${SRUN_MPI_MODE}" \
         --nodes="${SLURM_JOB_NUM_NODES}" \
         --ntasks="${SLURM_NTASKS}" \
         --ntasks-per-node=1 \
         "${APP}" "${n}" "${REPS}" 2>&1 | tee -a "${run_log}"
    rc=${PIPESTATUS[0]}
    set -e

    if [ "${rc}" -ne 0 ]; then
        echo "WARN: mpi run failed for n=${n} (rc=${rc}). See ${run_log}" >&2
        continue
    fi

    awk -v impl="mpi" \
        -v job="${SLURM_JOB_ID}" \
        -v nodes="${SLURM_JOB_NUM_NODES}" \
        -v tasks="${SLURM_NTASKS}" '
        /^RESULT / {
            n=""; rep=""; ms="";
            for (i=1; i<=NF; i++) {
                split($i, kv, "=");
                if (kv[1] == "n")   n = kv[2];
                if (kv[1] == "rep") rep = kv[2];
                if (kv[1] == "ms")  ms = kv[2];
            }
            if (n != "" && rep != "" && ms != "")
                printf "%s,%s,%s,%s,%s,%s,%s\n", impl, n, rep, ms, job, nodes, tasks;
        }' "${run_log}" >> "${CSV}"
done

echo ""
echo "Done. MPI benchmark CSV: ${CSV}"
