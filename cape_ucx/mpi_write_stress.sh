#!/bin/bash
#SBATCH --job-name=mpi_write_stress
#SBATCH --nodes=32
#SBATCH --ntasks=32
#SBATCH --ntasks-per-node=1
#SBATCH --time=04:00:00
#SBATCH --output=mpi_write_stress_%j.out
#SBATCH --error=mpi_write_stress_%j.err
#SBATCH --partition=compute

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${PROJECT_DIR:-${SLURM_SUBMIT_DIR:-${SCRIPT_DIR}}}"
[ -f "${PROJECT_DIR}/makefile" ] || PROJECT_DIR="${SCRIPT_DIR}"
cd "${PROJECT_DIR}"
JOB_TAG="${SLURM_JOB_ID:-local_$$}"

RESULTS_DIR="${RESULTS_DIR:-${SLURM_SUBMIT_DIR:-${PROJECT_DIR}}/results/mpi_write_stress_${JOB_TAG}}"
mkdir -p "${RESULTS_DIR}" 2>/dev/null || { RESULTS_DIR="/tmp/${USER}/mpi_write_stress_${JOB_TAG}"; mkdir -p "${RESULTS_DIR}"; }
BUILD_DIR="${BUILD_DIR:-${SLURM_SUBMIT_DIR:-/tmp/${USER}}/cape_build_mpi_ws_${JOB_TAG}}"
mkdir -p "${BUILD_DIR}/bin" 2>/dev/null || { BUILD_DIR="/tmp/${USER}/cape_build_mpi_ws_${JOB_TAG}"; mkdir -p "${BUILD_DIR}/bin"; }

N_LIST=(${N_LIST:-1048576})
PHASES_LIST=(${PHASES_LIST:-8})
NODES_LIST=(${NODES_LIST:-4 8 16 32})
REPS="${REPS:-5}"
MPI_WRITE_STRESS_PROFILE="${MPI_WRITE_STRESS_PROFILE:-1}"

module purge
module load GCCcore/14.2.0
module load UCX/1.18.0-GCCcore-14.2.0
module load OpenMPI/5.0.3-GCC-13.3.0 2>/dev/null || module load OpenMPI 2>/dev/null || true

if ! command -v mpicc >/dev/null 2>&1; then
    echo "ERROR: mpicc not found on PATH." >&2; exit 1
fi

SRC_DIR="${PROJECT_DIR}/src/apps/mpi"
SRUN_MPI_MODE="${SRUN_MPI_MODE:-pmix}"

echo "Building MPI write_stress binary"
mpicc -O2 -Wall -o "${BUILD_DIR}/bin/mpi_write_stress" "${SRC_DIR}/mpi_write_stress.c"

BIN="${BUILD_DIR}/bin/mpi_write_stress"

CSV="${RESULTS_DIR}/mpi_write_stress_summary_${JOB_TAG}.csv"
echo "impl,app,n,phases,nodes,rep,app_ms,job_id" > "${CSV}"

echo "Benchmarking MPI write_stress"
echo "Nodes: ${NODES_LIST[*]}  N: ${N_LIST[*]}  Phases: ${PHASES_LIST[*]}  Reps: ${REPS}"
echo "MPI_WRITE_STRESS_PROFILE=${MPI_WRITE_STRESS_PROFILE}"
echo "Results dir: ${RESULTS_DIR}"

TOTAL_NODES="${SLURM_JOB_NUM_NODES:-32}"

run_one() {
    local n="$1" phases="$2" nn="$3" rep="$4"
    local tag="mpi_write_stress_n${n}_p${phases}_nodes${nn}_rep${rep}"
    local log="${RESULTS_DIR}/${tag}.log"
    : > "${log}"
    echo "[launch] ${tag}"
    local rc=0
    MPI_WRITE_STRESS_PROFILE="${MPI_WRITE_STRESS_PROFILE}" \
    srun --exclusive --mpi="${SRUN_MPI_MODE}" --nodes="${nn}" --ntasks="${nn}" --ntasks-per-node=1 \
         "${BIN}" "${n}" "${phases}" 1 >>"${log}" 2>&1 || rc=$?
    if [ "${rc}" -ne 0 ]; then
        echo "[fail] ${tag} rc=${rc}" >&2; return
    fi
    awk -v impl="mpi" -v app="write_stress" -v nn="${nn}" -v rep="${rep}" \
        -v job="${SLURM_JOB_ID:-local}" '
        /^RESULT / {
            n=""; ph=""; ms="";
            for (i=1;i<=NF;i++) { split($i,kv,"="); if(kv[1]=="n")n=kv[2]; if(kv[1]=="phases")ph=kv[2]; if(kv[1]=="ms")ms=kv[2]; }
            if (n!="" && ms!="") printf "%s,%s,%s,%s,%s,%s,%s,%s\n", impl,app,n,ph,nn,rep,ms,job;
        }' "${log}" >> "${CSV}"
    echo "[done]   ${tag}"
}

run_batch() {
    local nn="$1"; shift
    local slots=$(( TOTAL_NODES / nn ))
    [ "${slots}" -lt 1 ] && slots=1
    echo ""
    echo "### Phase nodes=${nn}  parallel=${slots}  jobs=$#"
    local active=0
    for j in "$@"; do
        IFS='|' read -r _n _p _rep <<< "${j}"
        run_one "${_n}" "${_p}" "${nn}" "${_rep}" &
        active=$(( active + 1 ))
        if [ "${active}" -ge "${slots}" ]; then
            wait -n
            active=$(( active - 1 ))
        fi
    done
    wait
}

for nn in "${NODES_LIST[@]}"; do
    JOBS=()
    for n in "${N_LIST[@]}"; do
        for ph in "${PHASES_LIST[@]}"; do
            for r in $(seq 1 ${REPS}); do
                JOBS+=("${n}|${ph}|${r}")
            done
        done
    done
    run_batch "${nn}" "${JOBS[@]}"
done

echo ""
echo "Done. MPI write_stress summary CSV: ${CSV}"
echo "Per-experiment logs in: ${RESULTS_DIR}"
