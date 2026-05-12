#!/bin/bash
#SBATCH --job-name=mpi_game_of_life
#SBATCH --nodes=64
#SBATCH --ntasks=64
#SBATCH --ntasks-per-node=1
#SBATCH --time=04:00:00
#SBATCH --output=mpi_game_of_life_%j.out
#SBATCH --error=mpi_game_of_life_%j.err
#SBATCH --partition=compute
#SBATCH --mem=8G

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${PROJECT_DIR:-${SLURM_SUBMIT_DIR:-${SCRIPT_DIR}}}"
[ -f "${PROJECT_DIR}/makefile" ] || PROJECT_DIR="${SCRIPT_DIR}"
cd "${PROJECT_DIR}"
JOB_TAG="${SLURM_JOB_ID:-local_$$}"

RESULTS_DIR="${RESULTS_DIR:-${SLURM_SUBMIT_DIR:-${PROJECT_DIR}}/results/mpi_game_of_life_${JOB_TAG}}"
mkdir -p "${RESULTS_DIR}" 2>/dev/null || { RESULTS_DIR="/tmp/${USER}/mpi_game_of_life_${JOB_TAG}"; mkdir -p "${RESULTS_DIR}"; }
BUILD_DIR="${BUILD_DIR:-${SLURM_SUBMIT_DIR:-/tmp/${USER}}/cape_build_mpi_gol_${JOB_TAG}}"
mkdir -p "${BUILD_DIR}/bin" 2>/dev/null || { BUILD_DIR="/tmp/${USER}/cape_build_mpi_gol_${JOB_TAG}"; mkdir -p "${BUILD_DIR}/bin"; }

GRID_BYTES_LIST=(${GRID_BYTES_LIST:-2147483648})
LIVE_CELLS_LIST=(${LIVE_CELLS_LIST:-5000})
PHASES_LIST=(${PHASES_LIST:-1})
NODES_LIST=(${NODES_LIST:-4 8 16 32 64})
REPS="${REPS:-5}"

module purge
module load GCCcore/14.2.0
module load UCX/1.18.0-GCCcore-14.2.0
module load OpenMPI/5.0.3-GCC-13.3.0 2>/dev/null || module load OpenMPI 2>/dev/null || true

if ! command -v mpicc >/dev/null 2>&1; then
    echo "ERROR: mpicc not found on PATH." >&2; exit 1
fi

SRC_DIR="${PROJECT_DIR}/src/apps/mpi"
SRUN_MPI_MODE="${SRUN_MPI_MODE:-pmix}"

echo "Building MPI Game of Life benchmark"
mpicc -O2 -Wall -o "${BUILD_DIR}/bin/mpi_game_of_life" "${SRC_DIR}/mpi_game_of_life.c"

BIN="${BUILD_DIR}/bin/mpi_game_of_life"
CSV="${RESULTS_DIR}/mpi_game_of_life_summary_${JOB_TAG}.csv"
echo "impl,app,grid_bytes,live_per_rank,phases,nodes,rep,app_ms,job_id" > "${CSV}"

echo "Benchmarking MPI game_of_life"
echo "Nodes: ${NODES_LIST[*]}  Grid bytes: ${GRID_BYTES_LIST[*]}  Live/rank: ${LIVE_CELLS_LIST[*]}  Phases: ${PHASES_LIST[*]}  Reps: ${REPS}"
echo "Results dir: ${RESULTS_DIR}"

TOTAL_NODES="${SLURM_JOB_NUM_NODES:-64}"

run_one() {
    local grid_bytes="$1" live_cells="$2" phases="$3" nn="$4" rep="$5"
    local tag="mpi_game_of_life_g${grid_bytes}_l${live_cells}_p${phases}_nodes${nn}_rep${rep}"
    local log="${RESULTS_DIR}/${tag}.log"
    : > "${log}"
    echo "[launch] ${tag}"
    local rc=0
    srun --exclusive --mpi="${SRUN_MPI_MODE}" --nodes="${nn}" --ntasks="${nn}" --ntasks-per-node=1 \
         "${BIN}" "${grid_bytes}" "${live_cells}" "${phases}" 1 >>"${log}" 2>&1 || rc=$?
    if [ "${rc}" -ne 0 ]; then
        echo "[fail] ${tag} rc=${rc}" >&2; return
    fi
    awk -v impl="mpi" -v app="game_of_life" -v nn="${nn}" -v rep="${rep}" \
        -v job="${SLURM_JOB_ID:-local}" '
        /^RESULT / {
            gb=""; live=""; ph=""; ms="";
            for (i=1;i<=NF;i++) {
                split($i,kv,"=");
                if(kv[1]=="grid_bytes")gb=kv[2];
                if(kv[1]=="live_per_rank")live=kv[2];
                if(kv[1]=="phases")ph=kv[2];
                if(kv[1]=="ms")ms=kv[2];
            }
            if (gb!="" && live!="" && ms!="") printf "%s,%s,%s,%s,%s,%s,%s,%s,%s\n", impl,app,gb,live,ph,nn,rep,ms,job;
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
        IFS='|' read -r _gb _live _p _rep <<< "${j}"
        run_one "${_gb}" "${_live}" "${_p}" "${nn}" "${_rep}" &
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
    for gb in "${GRID_BYTES_LIST[@]}"; do
        for live in "${LIVE_CELLS_LIST[@]}"; do
            for ph in "${PHASES_LIST[@]}"; do
                for r in $(seq 1 ${REPS}); do
                    JOBS+=("${gb}|${live}|${ph}|${r}")
                done
            done
        done
    done
    run_batch "${nn}" "${JOBS[@]}"
done

echo ""
echo "Done. MPI game_of_life summary CSV: ${CSV}"
echo "Per-experiment logs in: ${RESULTS_DIR}"
