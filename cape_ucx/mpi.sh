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
D_VALUES_STR="${D_VALUES_STR:-256}"
REPS="${REPS:-1}"
APP="${APP:-1}"
read -r -a N_VALUES <<< "${N_VALUES_STR}"
read -r -a D_VALUES <<< "${D_VALUES_STR}"

# APP selects which benchmark(s) to run:
#   all -> mul_manual + matvec + gradient + memwrite
#   1=mul_manual  2=matvec  3=gradient  4=memwrite
#   comma-separated combos are ok: 1,4
if [ "${APP}" = "all" ]; then
    APPS_LIST=(1 2 3 4)
else
    IFS=',' read -r -a APPS_LIST <<< "${APP}"
fi

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

SRC_DIR="${PROJECT_DIR}/src/apps/mpi"
SRUN_MPI_MODE="${SRUN_MPI_MODE:-pmix}"

build_one() {
    local src="$1"
    local out="$2"
    echo "Compiling ${src} -> ${out}"
    mpicc -O2 -Wall -o "${out}" "${src}"
}

for id in "${APPS_LIST[@]}"; do
    case "${id}" in
        1) build_one "${SRC_DIR}/mpi_mul_manual.c" "${BUILD_DIR}/bin/mpi_mul_manual" ;;
        2) build_one "${SRC_DIR}/mpi_matvec.c"     "${BUILD_DIR}/bin/mpi_matvec"     ;;
        3) build_one "${SRC_DIR}/mpi_gradient.c"   "${BUILD_DIR}/bin/mpi_gradient"   ;;
        4) build_one "${SRC_DIR}/mpi_memwrite.c"   "${BUILD_DIR}/bin/mpi_memwrite"   ;;
        *) echo "WARN: unknown APP id '${id}', skipping build" >&2 ;;
    esac
done

CSV="${RESULTS_DIR}/mpi_bench_${JOB_TAG}.csv"
echo "impl,app,n,d,rep,app_ms,job_id,nodes,ntasks" > "${CSV}"

echo "Benchmarking pure MPI"
echo "APPs: ${APPS_LIST[*]}"
echo "N values: ${N_VALUES[*]}"
echo "D values (gradient only): ${D_VALUES[*]}"
echo "Reps: ${REPS}"
echo "Build dir: ${BUILD_DIR}"
echo "CSV: ${CSV}"
echo "MPI launch mode: ${SRUN_MPI_MODE}"

run_one() {
    local app_name="$1"; shift
    local app_bin="$1"; shift
    local n="$1"; shift
    local d="$1"; shift   # may be empty
    local tag="${app_name}_n${n}${d:+_d${d}}"
    local run_log="${BUILD_DIR}/run_mpi_${tag}.log"
    : > "${run_log}"

    echo ""
    echo "=== MPI ${app_name} n=${n}${d:+ d=${d}} reps=${REPS} ==="

    set +e
    if [ -n "${d}" ]; then
        srun --mpi="${SRUN_MPI_MODE}" \
             --nodes="${SLURM_JOB_NUM_NODES}" \
             --ntasks="${SLURM_NTASKS}" \
             --ntasks-per-node=1 \
             "${app_bin}" "${n}" "${d}" "${REPS}" 2>&1 | tee -a "${run_log}"
    else
        srun --mpi="${SRUN_MPI_MODE}" \
             --nodes="${SLURM_JOB_NUM_NODES}" \
             --ntasks="${SLURM_NTASKS}" \
             --ntasks-per-node=1 \
             "${app_bin}" "${n}" "${REPS}" 2>&1 | tee -a "${run_log}"
    fi
    local rc=${PIPESTATUS[0]}
    set -e

    if [ "${rc}" -ne 0 ]; then
        echo "WARN: mpi ${app_name} failed n=${n}${d:+ d=${d}} rc=${rc}" >&2
        return
    fi

    awk -v impl="mpi" \
        -v app="${app_name}" \
        -v job="${SLURM_JOB_ID}" \
        -v nodes="${SLURM_JOB_NUM_NODES}" \
        -v tasks="${SLURM_NTASKS}" '
        /^RESULT / {
            nn=""; dd=""; rep=""; ms="";
            for (i=1; i<=NF; i++) {
                split($i, kv, "=");
                if (kv[1] == "n")   nn = kv[2];
                if (kv[1] == "d")   dd = kv[2];
                if (kv[1] == "rep") rep = kv[2];
                if (kv[1] == "ms")  ms = kv[2];
            }
            if (nn != "" && rep != "" && ms != "")
                printf "%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
                       impl, app, nn, dd, rep, ms, job, nodes, tasks;
        }' "${run_log}" >> "${CSV}"
}

for id in "${APPS_LIST[@]}"; do
    case "${id}" in
        1) name="mul_manual"; bin="${BUILD_DIR}/bin/mpi_mul_manual" ;;
        2) name="matvec";     bin="${BUILD_DIR}/bin/mpi_matvec"     ;;
        3) name="gradient";   bin="${BUILD_DIR}/bin/mpi_gradient"   ;;
        4) name="memwrite";   bin="${BUILD_DIR}/bin/mpi_memwrite"   ;;
        *) continue ;;
    esac
    if [ ! -x "${bin}" ]; then
        echo "WARN: missing binary ${bin}" >&2
        continue
    fi
    for n in "${N_VALUES[@]}"; do
        if [ "${id}" = "3" ]; then
            for d in "${D_VALUES[@]}"; do
                run_one "${name}" "${bin}" "${n}" "${d}"
            done
        else
            run_one "${name}" "${bin}" "${n}" ""
        fi
    done
done

echo ""
echo "Done. MPI benchmark CSV: ${CSV}"
