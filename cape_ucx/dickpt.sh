#!/bin/bash
#SBATCH --job-name=cape_dickpt_bench
#SBATCH --nodes=8
#SBATCH --ntasks=8
#SBATCH --ntasks-per-node=1
#SBATCH --time=00:45:00
#SBATCH --output=cape_dickpt_bench_%j.out
#SBATCH --error=cape_dickpt_bench_%j.err
#SBATCH --partition=compute

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${PROJECT_DIR:-${SLURM_SUBMIT_DIR:-${SCRIPT_DIR}}}"
if [ ! -f "${PROJECT_DIR}/makefile" ]; then
    PROJECT_DIR="${SCRIPT_DIR}"
fi
if [ ! -f "${PROJECT_DIR}/makefile" ]; then
    echo "ERROR: makefile not found in PROJECT_DIR='${PROJECT_DIR}'" >&2
    exit 1
fi
cd "${PROJECT_DIR}"
JOB_TAG="${SLURM_JOB_ID:-local_$$}"

RESULTS_DIR="${RESULTS_DIR:-${SLURM_SUBMIT_DIR:-${PROJECT_DIR}}/results}"
if ! mkdir -p "${RESULTS_DIR}" 2>/dev/null; then
    RESULTS_DIR="/tmp/${USER:-$(id -un)}/cape_results"
    mkdir -p "${RESULTS_DIR}"
fi
BUILD_DIR="${BUILD_DIR:-${SLURM_SUBMIT_DIR:-/tmp/${USER:-$(id -un)}}/cape_build_dickpt_${JOB_TAG}}"
if ! mkdir -p "${BUILD_DIR}/bin" "${BUILD_DIR}/obj" "${BUILD_DIR}/lib" 2>/dev/null; then
    BUILD_DIR="/tmp/${USER:-$(id -un)}/cape_build_dickpt_${JOB_TAG}"
    mkdir -p "${BUILD_DIR}/bin" "${BUILD_DIR}/obj" "${BUILD_DIR}/lib"
fi

N_VALUES_STR="${N_VALUES_STR:-3000}"
MEM_N_VALUES_STR="${MEM_N_VALUES_STR:-1048576}"
D_VALUES_STR="${D_VALUES_STR:-256}"
REPS="${REPS:-1}"
APP="${APP:-1}"
PROFILE="${PROFILE:-1}"
read -r -a N_VALUES <<< "${N_VALUES_STR}"
read -r -a MEM_N_VALUES <<< "${MEM_N_VALUES_STR}"
read -r -a D_VALUES <<< "${D_VALUES_STR}"

# APP selects which benchmark(s) to run:
#   all  -> mul_manual + matvec + gradient + memwrite
#   1    -> mul_manual    2 -> matvec    3 -> gradient    4 -> memwrite
#   1,4  -> mul_manual + memwrite
if [ "${APP}" = "all" ]; then
    APPS_LIST=(1 2 3 4)
else
    IFS=',' read -r -a APPS_LIST <<< "${APP}"
fi
BOOTSTRAP_ROOT="${BOOTSTRAP_ROOT:-${BUILD_DIR}/ucx_bootstrap}"
mkdir -p "${BOOTSTRAP_ROOT}"

module purge
module load GCCcore/14.2.0
module load UCX/1.18.0-GCCcore-14.2.0

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

_pmix_found=0
PMIX_HOME=""
for _pfx in \
    "${EBROOTPMIX:-__none__}" \
    "$(pmix_info --path prefix 2>/dev/null | awk '{print $NF}')" \
    "$(pkg-config --variable=prefix pmix 2>/dev/null)"
do
    [ "${_pfx}" = "__none__" ] && continue
    [ -z "${_pfx}" ] && continue
    if [ -f "${_pfx}/include/pmix.h" ] && [ -f "${_pfx}/lib/libpmix.so" ]; then
        PMIX_HOME="${_pfx}"
        _pmix_found=1
        break
    fi
done

if [ "${_pmix_found}" -eq 1 ]; then
    PMIX_FLAGS="-DUSE_PMIX -I${PMIX_HOME}/include"
    PMIX_LINK="-L${PMIX_HOME}/lib -lpmix -Wl,-rpath,${PMIX_HOME}/lib"
    SRUN_MPI_MODE="${SRUN_MPI_MODE:-pmix}"
else
    PMIX_FLAGS=""
    PMIX_LINK=""
    SRUN_MPI_MODE="${SRUN_MPI_MODE:-none}"
fi

MAKE_ARGS=( \
    EXE_FOLDER="${BUILD_DIR}/bin" \
    O_FOLDER="${BUILD_DIR}/obj" \
    L_FOLDER="${BUILD_DIR}/lib" \
    UCX_SRC="${UCX_INC}" \
    UCX_GEN="${UCX_INC}" \
    UCX_LIB="${UCX_LIB}" \
    "PMIX_FLAGS=${PMIX_FLAGS}" \
    "PMIX_LINK=${PMIX_LINK}" \
    CC=gcc \
)

make -C "${PROJECT_DIR}" cleanall \
    EXE_FOLDER="${BUILD_DIR}/bin" \
    O_FOLDER="${BUILD_DIR}/obj" \
    L_FOLDER="${BUILD_DIR}/lib" 2>/dev/null || true

# Build dickpt monitor (needs UCX) + dickpt app (plain binary)
make -C "${PROJECT_DIR}" dickpt PROFILE="${PROFILE}" "${MAKE_ARGS[@]}"

MONITOR="${BUILD_DIR}/bin/cape_dickpt_monitor"

CSV="${RESULTS_DIR}/dickpt_bench_${JOB_TAG}.csv"
echo "impl,app,n,d,rep,app_ms,job_id,nodes,ntasks" > "${CSV}"

echo "Benchmarking DICKPT"
echo "APPs: ${APPS_LIST[*]}"
echo "N values: ${N_VALUES[*]}"
echo "Memwrite N values: ${MEM_N_VALUES[*]}"
echo "D values (gradient only): ${D_VALUES[*]}"
echo "Reps: ${REPS}"
echo "Build dir: ${BUILD_DIR}"
echo "CSV: ${CSV}"
echo "MPI launch mode: ${SRUN_MPI_MODE}"
echo "Monitor profiling: ${PROFILE}"

run_one() {
    local app_name="$1"; shift
    local app_bin="$1"; shift
    local n="$1"; shift
    local d="$1"; shift   # may be empty
    local tag="${app_name}_n${n}${d:+_d${d}}"
    local run_log="${BUILD_DIR}/run_dickpt_${tag}.log"
    : > "${run_log}"
    local bootstrap_id="${JOB_TAG}_${tag}"
    local bootstrap_dir="${BOOTSTRAP_ROOT}/${bootstrap_id}"
    rm -rf "${bootstrap_dir}"
    mkdir -p "${bootstrap_dir}"

    echo ""
    echo "=== DICKPT ${app_name} n=${n}${d:+ d=${d}} reps=${REPS} ==="

    set +e
    if [ -n "${d}" ]; then
        CAPE_UCX_BOOTSTRAP_ID="${bootstrap_id}" \
        CAPE_UCX_BOOTSTRAP_DIR="${bootstrap_dir}" \
        srun --mpi="${SRUN_MPI_MODE}" \
             --nodes="${SLURM_JOB_NUM_NODES}" \
             --ntasks="${SLURM_NTASKS}" \
             --ntasks-per-node=1 \
             "${MONITOR}" "${app_bin}" "${n}" "${d}" "${REPS}" 2>&1 | tee -a "${run_log}"
    else
        CAPE_UCX_BOOTSTRAP_ID="${bootstrap_id}" \
        CAPE_UCX_BOOTSTRAP_DIR="${bootstrap_dir}" \
        srun --mpi="${SRUN_MPI_MODE}" \
             --nodes="${SLURM_JOB_NUM_NODES}" \
             --ntasks="${SLURM_NTASKS}" \
             --ntasks-per-node=1 \
             "${MONITOR}" "${app_bin}" "${n}" "${REPS}" 2>&1 | tee -a "${run_log}"
    fi
    local rc=${PIPESTATUS[0]}
    set -e
    rm -rf "${bootstrap_dir}"

    if [ "${rc}" -ne 0 ]; then
        echo "WARN: dickpt ${app_name} failed n=${n}${d:+ d=${d}} rc=${rc}" >&2
        return
    fi

    awk -v impl="dickpt" \
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
        1) name="mul_manual"; bin="${BUILD_DIR}/bin/dickpt_mul_manual"      ;;
        2) name="matvec";     bin="${BUILD_DIR}/bin/dickpt_matvec_manual"   ;;
        3) name="gradient";   bin="${BUILD_DIR}/bin/dickpt_gradient_manual" ;;
        4) name="memwrite";   bin="${BUILD_DIR}/bin/dickpt_memwrite_manual" ;;
        *) echo "WARN: unknown APP id '${id}'" >&2; continue ;;
    esac
    if [ ! -x "${bin}" ]; then
        echo "WARN: missing binary ${bin}" >&2
        continue
    fi
    if [ "${id}" = "4" ]; then
        RUN_N_VALUES=("${MEM_N_VALUES[@]}")
    else
        RUN_N_VALUES=("${N_VALUES[@]}")
    fi
    for n in "${RUN_N_VALUES[@]}"; do
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
echo "Done. DICKPT benchmark CSV: ${CSV}"
