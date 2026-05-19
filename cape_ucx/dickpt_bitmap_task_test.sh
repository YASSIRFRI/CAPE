#!/bin/bash
#SBATCH --job-name=dickpt_bitmap_task_test
#SBATCH --nodes=4
#SBATCH --ntasks=4
#SBATCH --ntasks-per-node=1
#SBATCH --time=00:30:00
#SBATCH --output=dickpt_bitmap_task_test_%j.out
#SBATCH --error=dickpt_bitmap_task_test_%j.err
#SBATCH --partition=compute

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${PROJECT_DIR:-${SLURM_SUBMIT_DIR:-${SCRIPT_DIR}}}"
[ -f "${PROJECT_DIR}/makefile" ] || PROJECT_DIR="${SCRIPT_DIR}"
cd "${PROJECT_DIR}"

JOB_TAG="${SLURM_JOB_ID:-local_$$}"
RESULTS_DIR="${RESULTS_DIR:-${SLURM_SUBMIT_DIR:-${PROJECT_DIR}}/results/dickpt_bitmap_task_test_${JOB_TAG}}"
mkdir -p "${RESULTS_DIR}" 2>/dev/null || {
    RESULTS_DIR="/tmp/${USER}/dickpt_bitmap_task_test_${JOB_TAG}"
    mkdir -p "${RESULTS_DIR}"
}

BUILD_DIR="${BUILD_DIR:-${SLURM_SUBMIT_DIR:-/tmp/${USER}}/cape_build_dickpt_bitmap_task_${JOB_TAG}}"
mkdir -p "${BUILD_DIR}/bin" "${BUILD_DIR}/obj" "${BUILD_DIR}/lib" 2>/dev/null || {
    BUILD_DIR="/tmp/${USER}/cape_build_dickpt_bitmap_task_${JOB_TAG}"
    mkdir -p "${BUILD_DIR}/bin" "${BUILD_DIR}/obj" "${BUILD_DIR}/lib"
}

BOOTSTRAP_ROOT="${BOOTSTRAP_ROOT:-${BUILD_DIR}/ucx_bootstrap}"
mkdir -p "${BOOTSTRAP_ROOT}"

NODES_LIST=(${NODES_LIST:-2 3 4})
REPS="${REPS:-1}"
PROFILE="${PROFILE:-1}"
USE_PMIX="${USE_PMIX:-1}"
SRUN_RESOURCE_MODE="${SRUN_RESOURCE_MODE:-shared}"  # shared|exclusive|overlap
SRUN_STEP_TIMEOUT="${SRUN_STEP_TIMEOUT:-180}"
SRUN_LABEL="${SRUN_LABEL:-1}"

module purge
module load GCCcore/14.2.0
module load UCX/1.18.0-GCCcore-14.2.0
module load PMIx/5.0.6-GCCcore-14.2.0 2>/dev/null || module load PMIx 2>/dev/null || true

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

PMIX_FLAGS=""
PMIX_LINK=""
SRUN_MPI_MODE="${SRUN_MPI_MODE:-none}"
if [ "${USE_PMIX}" != "0" ]; then
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
fi


MAKE_ARGS=(
    EXE_FOLDER="${BUILD_DIR}/bin" O_FOLDER="${BUILD_DIR}/obj" L_FOLDER="${BUILD_DIR}/lib"
    UCX_SRC="${UCX_INC}" UCX_GEN="${UCX_INC}" UCX_LIB="${UCX_LIB}"
    "PMIX_FLAGS=${PMIX_FLAGS}" "PMIX_LINK=${PMIX_LINK}" CC=gcc
)

make -C "${PROJECT_DIR}" cleanall \
    EXE_FOLDER="${BUILD_DIR}/bin" O_FOLDER="${BUILD_DIR}/obj" L_FOLDER="${BUILD_DIR}/lib" 2>/dev/null || true
make -C "${PROJECT_DIR}" dickpt_bitmap_monitor dickpt_task_example PROFILE="${PROFILE}" "${MAKE_ARGS[@]}"

MONITOR="${BUILD_DIR}/bin/cape_dickpt_bitmap_monitor"
BIN="${BUILD_DIR}/bin/dickpt_task_example"
TOTAL_NODES="${SLURM_JOB_NUM_NODES:-4}"

echo "Testing DICKPT bitmap OpenMP task scheduling"
echo "DICKPT:  src/apps/cape_task_example_dickpt.c -> ${BIN}"
echo "Monitor: src/monitor/cape_incr_bitmap.c -> ${MONITOR}"
echo "Nodes: ${NODES_LIST[*]}  Reps: ${REPS}  MPI mode: ${SRUN_MPI_MODE}"
echo "Launch: resource=${SRUN_RESOURCE_MODE} timeout=${SRUN_STEP_TIMEOUT}s"
echo "Results dir: ${RESULTS_DIR}"

run_one() {
    local nn="$1"
    local tag="dickpt_bitmap_task_nodes${nn}_reps${REPS}"
    local log="${RESULTS_DIR}/${tag}.log"
    local bid="${JOB_TAG}_${tag}"
    local bdir="${BOOTSTRAP_ROOT}/${bid}"
    local rc=0
    local srun_args=()
    local timeout_cmd=()

    if [ "${nn}" -gt "${TOTAL_NODES}" ]; then
        echo "[skip] ${tag}: requested ${nn} nodes but allocation has ${TOTAL_NODES}"
        return 0
    fi

    rm -rf "${bdir}"
    mkdir -p "${bdir}"
    : > "${log}"

    echo "[launch] ${tag}"
    srun_args=(--mpi="${SRUN_MPI_MODE}" --nodes="${nn}" --ntasks="${nn}" --ntasks-per-node=1 --kill-on-bad-exit=1)
    if [ "${SRUN_LABEL}" != "0" ]; then
        srun_args+=(--label)
    fi
    case "${SRUN_RESOURCE_MODE}" in
        shared|"")
            ;;
        exclusive)
            srun_args=(--exclusive "${srun_args[@]}")
            ;;
        overlap)
            srun_args=(--overlap "${srun_args[@]}")
            ;;
        *)
            echo "[fail] invalid SRUN_RESOURCE_MODE=${SRUN_RESOURCE_MODE}; use shared, exclusive, or overlap" >&2
            return 1
            ;;
    esac
    if command -v timeout >/dev/null 2>&1 && [ "${SRUN_STEP_TIMEOUT}" -gt 0 ]; then
        timeout_cmd=(timeout --foreground --kill-after=15s "${SRUN_STEP_TIMEOUT}s")
    fi
    {
        echo "[start] $(date -Is)"
        echo "[cmd] CAPE_UCX_BOOTSTRAP_ID=${bid} CAPE_UCX_BOOTSTRAP_DIR=${bdir} ${timeout_cmd[*]} srun ${srun_args[*]} ${MONITOR} ${BIN} ${REPS}"
    } >>"${log}"
    CAPE_UCX_BOOTSTRAP_ID="${bid}" CAPE_UCX_BOOTSTRAP_DIR="${bdir}" \
    "${timeout_cmd[@]}" srun "${srun_args[@]}" \
         "${MONITOR}" "${BIN}" "${REPS}" >>"${log}" 2>&1 || rc=$?
    rm -rf "${bdir}"

    if [ "${rc}" -ne 0 ]; then
        if [ "${rc}" -eq 124 ]; then
            echo "[fail] ${tag} timed out after ${SRUN_STEP_TIMEOUT}s log=${log}" >&2
        else
            echo "[fail] ${tag} rc=${rc} log=${log}" >&2
        fi
        tail -n 40 "${log}" >&2 || true
        return "${rc}"
    fi
    if ! grep -q "TASK_RESULT" "${log}"; then
        echo "[fail] ${tag}: missing TASK_RESULT in ${log}" >&2
        return 1
    fi

    echo "[done]   ${tag}"
}

for nn in "${NODES_LIST[@]}"; do
    run_one "${nn}"
done

echo ""
echo "Done. DICKPT bitmap task logs in: ${RESULTS_DIR}"
