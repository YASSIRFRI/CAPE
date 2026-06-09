#!/bin/bash
#SBATCH --job-name=dickpt_bitmap_task_depend_manual
#SBATCH --nodes=4
#SBATCH --ntasks=4
#SBATCH --ntasks-per-node=1
#SBATCH --time=00:30:00
#SBATCH --output=dickpt_bitmap_task_depend_manual_%j.out
#SBATCH --error=dickpt_bitmap_task_depend_manual_%j.err
#SBATCH --partition=compute

# Builds and runs the transpiler-generated OpenMP task depend() test against the
# DICKPT bitmap monitor. The app (cape_task_depend_manual.c) is produced by running
#   txl tests/test_task_depend.c omptodickpt.Txl
# in 20171001 TXL Transform/.../openmptodickpt. Set REGEN=1 to regenerate it
# here (requires `txl` on PATH); otherwise the committed copy is used.
#
# Pass criteria: the app prints "DEPEND_RESULT ... status=OK", meaning the
# diamond-DAG tasks were dispatched respecting depend(in/out/inout) — each
# task observed its predecessors' outputs (a=10,b=30,c=40,d=110). A scheduler
# that ignored depend() would read still-zero inputs and print status=FAIL.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${PROJECT_DIR:-${SLURM_SUBMIT_DIR:-${SCRIPT_DIR}}}"
[ -f "${PROJECT_DIR}/makefile" ] || PROJECT_DIR="${SCRIPT_DIR}"
cd "${PROJECT_DIR}"

JOB_TAG="${SLURM_JOB_ID:-local_$$}"
RESULTS_DIR="${RESULTS_DIR:-${SLURM_SUBMIT_DIR:-${PROJECT_DIR}}/results/dickpt_bitmap_task_depend_manual_${JOB_TAG}}"
mkdir -p "${RESULTS_DIR}" 2>/dev/null || {
    RESULTS_DIR="/tmp/${USER}/dickpt_bitmap_task_depend_manual_${JOB_TAG}"
    mkdir -p "${RESULTS_DIR}"
}

BUILD_DIR="${BUILD_DIR:-${SLURM_SUBMIT_DIR:-/tmp/${USER}}/cape_build_dickpt_task_depend_manual_${JOB_TAG}}"
mkdir -p "${BUILD_DIR}/bin" "${BUILD_DIR}/obj" "${BUILD_DIR}/lib" 2>/dev/null || {
    BUILD_DIR="/tmp/${USER}/cape_build_dickpt_task_depend_manual_${JOB_TAG}"
    mkdir -p "${BUILD_DIR}/bin" "${BUILD_DIR}/obj" "${BUILD_DIR}/lib"
}

BOOTSTRAP_ROOT="${BOOTSTRAP_ROOT:-${BUILD_DIR}/ucx_bootstrap}"
mkdir -p "${BOOTSTRAP_ROOT}"

NODES_LIST=(${NODES_LIST:-4})
REPS="${REPS:-1}"
PROFILE="${PROFILE:-1}"
REGEN="${REGEN:-0}"

# Optionally regenerate the app from OpenMP source via the TXL transpiler.
TXL_DIR="${TXL_DIR:-${PROJECT_DIR}/../20171001 TXL Transform/20171001 TXL Transform/openmptodickpt}"
APP_SRC="${PROJECT_DIR}/src/apps/cape_task_depend_manual.c"
if [ "${REGEN}" = "1" ]; then
    if ! command -v txl &>/dev/null; then
        echo "ERROR: REGEN=1 but txl not on PATH" >&2; exit 1
    fi
    echo "Regenerating ${APP_SRC} via TXL"
    ( cd "${TXL_DIR}" && txl tests/test_task_depend.c omptodickpt.Txl ) > "${APP_SRC}.tmp" 2>/dev/null
    mv "${APP_SRC}.tmp" "${APP_SRC}"
fi
[ -f "${APP_SRC}" ] || { echo "ERROR: missing ${APP_SRC} (run with REGEN=1)" >&2; exit 1; }

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

make -C "${PROJECT_DIR}" cleanall \
    EXE_FOLDER="${BUILD_DIR}/bin" O_FOLDER="${BUILD_DIR}/obj" L_FOLDER="${BUILD_DIR}/lib" 2>/dev/null || true
make -C "${PROJECT_DIR}" dickpt_bitmap_monitor dickpt_task_depend_manual PROFILE="${PROFILE}" "${MAKE_ARGS[@]}"

MONITOR="${BUILD_DIR}/bin/cape_dickpt_bitmap_monitor"
BIN="${BUILD_DIR}/bin/dickpt_task_depend_manual"
TOTAL_NODES="${SLURM_JOB_NUM_NODES:-4}"

echo "Testing DICKPT bitmap OpenMP task (transpiler-generated manual)"
echo "App:     src/apps/cape_task_depend_manual.c -> ${BIN}"
echo "Monitor: src/monitor/cape_incr_bitmap.c -> ${MONITOR}"
echo "Nodes: ${NODES_LIST[*]}  Reps: ${REPS}  MPI mode: ${SRUN_MPI_MODE}"
echo "Results dir: ${RESULTS_DIR}"

overall_rc=0

run_one() {
    local nn="$1"
    local tag="dickpt_bitmap_task_depend_manual_nodes${nn}_reps${REPS}"
    local log="${RESULTS_DIR}/${tag}.log"
    local bid="${JOB_TAG}_${tag}"
    local bdir="${BOOTSTRAP_ROOT}/${bid}"
    local rc=0

    if [ "${nn}" -gt "${TOTAL_NODES}" ]; then
        echo "[skip] ${tag}: requested ${nn} nodes but allocation has ${TOTAL_NODES}"
        return 0
    fi
    if [ "${nn}" -lt 2 ]; then
        echo "[skip] ${tag}: OpenMP task needs >= 2 nodes (1 master + >=1 worker)"
        return 0
    fi

    rm -rf "${bdir}"; mkdir -p "${bdir}"; : > "${log}"

    echo "[launch] ${tag}"
    CAPE_UCX_BOOTSTRAP_ID="${bid}" CAPE_UCX_BOOTSTRAP_DIR="${bdir}" \
    srun --exclusive --mpi="${SRUN_MPI_MODE}" --nodes="${nn}" --ntasks="${nn}" --ntasks-per-node=1 \
         "${MONITOR}" "${BIN}" "${REPS}" >>"${log}" 2>&1 || rc=$?
    rm -rf "${bdir}"

    if [ "${rc}" -ne 0 ]; then
        echo "[fail] ${tag} rc=${rc} log=${log}" >&2
        return "${rc}"
    fi

    local line
    line=$(grep -m1 "DEPEND_RESULT" "${log}" || true)
    if [ -z "${line}" ]; then
        echo "[fail] ${tag}: missing DEPEND_RESULT in ${log}" >&2
        return 1
    fi

    if grep -q "status=OK" <<<"${line}"; then
        echo "[done]   ${tag}: PASS (${line})"
    else
        echo "[fail]   ${tag}: dependency ordering violated (${line})" >&2
        return 1
    fi
}

for nn in "${NODES_LIST[@]}"; do
    run_one "${nn}" || overall_rc=1
done

echo ""
echo "Done. DICKPT bitmap task (manual) logs in: ${RESULTS_DIR}"
exit "${overall_rc}"
