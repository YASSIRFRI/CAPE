#!/bin/bash
#SBATCH --job-name=mpi_full_bench
#SBATCH --nodes=16
#SBATCH --ntasks=16
#SBATCH --ntasks-per-node=1
#SBATCH --time=04:00:00
#SBATCH --output=mpi_full_bench_%j.out
#SBATCH --error=mpi_full_bench_%j.err
#SBATCH --partition=compute

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${PROJECT_DIR:-${SLURM_SUBMIT_DIR:-${SCRIPT_DIR}}}"
[ -f "${PROJECT_DIR}/makefile" ] || PROJECT_DIR="${SCRIPT_DIR}"
cd "${PROJECT_DIR}"
JOB_TAG="${SLURM_JOB_ID:-local_$$}"

RESULTS_DIR="${RESULTS_DIR:-${SLURM_SUBMIT_DIR:-${PROJECT_DIR}}/results/mpi_${JOB_TAG}}"
mkdir -p "${RESULTS_DIR}" 2>/dev/null || { RESULTS_DIR="/tmp/${USER}/mpi_results_${JOB_TAG}"; mkdir -p "${RESULTS_DIR}"; }
BUILD_DIR="${BUILD_DIR:-${SLURM_SUBMIT_DIR:-/tmp/${USER}}/cape_build_mpi_${JOB_TAG}}"
mkdir -p "${BUILD_DIR}/bin" 2>/dev/null || { BUILD_DIR="/tmp/${USER}/cape_build_mpi_${JOB_TAG}"; mkdir -p "${BUILD_DIR}/bin"; }

N_MAMULT=(${N_MAMULT:-3000 6400})
N_MATVEC=(${N_MATVEC:-2048 4096})
GRADIENT_PAIRS=(${GRADIENT_PAIRS:-"4096:256" "8192:512"})
N_MEMWRITE=(${N_MEMWRITE:-1048576})
NODES_LIST=(${NODES_LIST:-4 8 16})
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

echo "Building MPI binaries"
mpicc -O2 -Wall -o "${BUILD_DIR}/bin/mpi_mul_manual" "${SRC_DIR}/mpi_mul_manual.c"
mpicc -O2 -Wall -o "${BUILD_DIR}/bin/mpi_matvec"     "${SRC_DIR}/mpi_matvec.c"
mpicc -O2 -Wall -o "${BUILD_DIR}/bin/mpi_gradient"   "${SRC_DIR}/mpi_gradient.c"
mpicc -O2 -Wall -o "${BUILD_DIR}/bin/mpi_memwrite"   "${SRC_DIR}/mpi_memwrite.c"

CSV="${RESULTS_DIR}/mpi_summary_${JOB_TAG}.csv"
echo "impl,app,n,d,nodes,rep,app_ms,job_id" > "${CSV}"

echo "Benchmarking pure MPI"
echo "Nodes: ${NODES_LIST[*]}  Reps/cell: ${REPS}"
echo "Results dir: ${RESULTS_DIR}"

TOTAL_NODES="${SLURM_JOB_NUM_NODES:-16}"

run_one() {
    local app="$1" bin="$2" n="$3" d="$4" nn="$5" rep="$6"
    local tag="mpi_${app}_n${n}${d:+_d${d}}_nodes${nn}_rep${rep}"
    local log="${RESULTS_DIR}/${tag}.log"
    : > "${log}"
    echo "[launch] ${tag}"
    local rc=0
    if [ -n "${d}" ]; then
        srun --exclusive --mpi="${SRUN_MPI_MODE}" --nodes="${nn}" --ntasks="${nn}" --ntasks-per-node=1 \
             "${bin}" "${n}" "${d}" 1 >>"${log}" 2>&1 || rc=$?
    else
        srun --exclusive --mpi="${SRUN_MPI_MODE}" --nodes="${nn}" --ntasks="${nn}" --ntasks-per-node=1 \
             "${bin}" "${n}" 1 >>"${log}" 2>&1 || rc=$?
    fi
    if [ "${rc}" -ne 0 ]; then
        echo "[fail] ${tag} rc=${rc}" >&2; return
    fi
    awk -v impl="mpi" -v app="${app}" -v nn="${nn}" -v rep="${rep}" \
        -v job="${SLURM_JOB_ID:-local}" '
        /^RESULT / {
            n=""; dd=""; ms="";
            for (i=1;i<=NF;i++) { split($i,kv,"="); if(kv[1]=="n")n=kv[2]; if(kv[1]=="d")dd=kv[2]; if(kv[1]=="ms")ms=kv[2]; }
            if (n!="" && ms!="") printf "%s,%s,%s,%s,%s,%s,%s,%s\n", impl,app,n,dd,nn,rep,ms,job;
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
        IFS='|' read -r _app _bin _n _d _rep <<< "${j}"
        run_one "${_app}" "${_bin}" "${_n}" "${_d}" "${nn}" "${_rep}" &
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
    for n in "${N_MAMULT[@]}"; do for r in $(seq 1 ${REPS}); do JOBS+=("mul_manual|${BUILD_DIR}/bin/mpi_mul_manual|${n}||${r}"); done; done
    for n in "${N_MATVEC[@]}"; do for r in $(seq 1 ${REPS}); do JOBS+=("matvec|${BUILD_DIR}/bin/mpi_matvec|${n}||${r}"); done; done
    for pair in "${GRADIENT_PAIRS[@]}"; do
        n="${pair%:*}"; d="${pair#*:}"
        for r in $(seq 1 ${REPS}); do JOBS+=("gradient|${BUILD_DIR}/bin/mpi_gradient|${n}|${d}|${r}"); done
    done
    for n in "${N_MEMWRITE[@]}"; do for r in $(seq 1 ${REPS}); do JOBS+=("memwrite|${BUILD_DIR}/bin/mpi_memwrite|${n}||${r}"); done; done
    run_batch "${nn}" "${JOBS[@]}"
done

echo ""
echo "Done. MPI summary CSV: ${CSV}"
echo "Per-experiment logs in: ${RESULTS_DIR}"
