#!/bin/bash
#SBATCH --job-name=cape_verify_private
#SBATCH --nodes=4
#SBATCH --ntasks=4
#SBATCH --ntasks-per-node=1
#SBATCH --time=00:30:00
#SBATCH --output=cape_verify_private_%j.out
#SBATCH --error=cape_verify_private_%j.err
#SBATCH --partition=compute
#SBATCH --mem=2G

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${PROJECT_DIR:-${SLURM_SUBMIT_DIR:-${SCRIPT_DIR}}}"
[ -f "${PROJECT_DIR}/makefile" ] || PROJECT_DIR="${SCRIPT_DIR}"
cd "${PROJECT_DIR}"
JOB_TAG="${SLURM_JOB_ID:-local_$$}"

# Pre-transpiled CAPE source (TXL is run off-cluster; only C is built here).
APP_SRC="${APP_SRC:-${PROJECT_DIR}/../20171001 TXL Transform/20171001 TXL Transform/openmptocape/verify_private_cape.c}"
if [ ! -f "${APP_SRC}" ]; then
    echo "ERROR: transpiled source not found: ${APP_SRC}" >&2
    echo "       Run the OMP->CAPE transpiler first (openmptocape/build.sh or txl)." >&2
    exit 1
fi

RESULTS_DIR="${RESULTS_DIR:-${SLURM_SUBMIT_DIR:-${PROJECT_DIR}}/results/cape_verify_private_${JOB_TAG}}"
mkdir -p "${RESULTS_DIR}" 2>/dev/null || { RESULTS_DIR="/tmp/${USER}/cape_verify_private_${JOB_TAG}"; mkdir -p "${RESULTS_DIR}"; }
BUILD_DIR="${BUILD_DIR:-${SLURM_SUBMIT_DIR:-/tmp/${USER}}/cape_build_verify_${JOB_TAG}}"
mkdir -p "${BUILD_DIR}/bin" 2>/dev/null || { BUILD_DIR="/tmp/${USER}/cape_build_verify_${JOB_TAG}"; mkdir -p "${BUILD_DIR}/bin"; }
BOOTSTRAP_ROOT="${BOOTSTRAP_ROOT:-${BUILD_DIR}/ucx_bootstrap}"
mkdir -p "${BOOTSTRAP_ROOT}"

NODES_LIST=(${NODES_LIST:-4})
REPS="${REPS:-1}"

module purge
module load GCCcore/14.2.0
module load UCX/1.18.0-GCCcore-14.2.0
module load PMIx/5.0.6-GCCcore-14.2.0 2>/dev/null || module load PMIx 2>/dev/null || true

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

# Compile cape_bitmap.c (library) + the transpiled app into one binary.
BIN="${BUILD_DIR}/bin/verify_private"
echo "Compiling verify_private (CAPE bitmap backend)"
echo "  app:     ${APP_SRC}"
echo "  library: ${PROJECT_DIR}/src/monitor/cape_bitmap.c"
gcc -w -O2 \
    -I"${PROJECT_DIR}/include" -I"${UCX_INC}" ${PMIX_FLAGS} -DCAPE_PROFILE \
    "${PROJECT_DIR}/src/monitor/cape_bitmap.c" \
    "${APP_SRC}" \
    -o "${BIN}" \
    -L"${UCX_LIB}" -lucp -lucs -lpthread ${PMIX_LINK} -Wl,-rpath,"${UCX_LIB}"

echo "Built: ${BIN}"
echo "Nodes: ${NODES_LIST[*]}  Reps: ${REPS}  MPI mode: ${SRUN_MPI_MODE}"
echo "Results dir: ${RESULTS_DIR}"

TOTAL_NODES="${SLURM_JOB_NUM_NODES:-4}"

run_one() {
    local nn="$1" rep="$2"
    local tag="cape_verify_private_nodes${nn}_rep${rep}"
    local log="${RESULTS_DIR}/${tag}.log"
    local bid="${JOB_TAG}_${tag}"
    local bdir="${BOOTSTRAP_ROOT}/${bid}"
    local rc=0

    if [ "${nn}" -gt "${TOTAL_NODES}" ]; then
        echo "[skip] ${tag}: requested ${nn} nodes but allocation has ${TOTAL_NODES}"
        return 0
    fi

    rm -rf "${bdir}"; mkdir -p "${bdir}"; : > "${log}"
    echo "[launch] ${tag}"
    CAPE_UCX_BOOTSTRAP_ID="${bid}" CAPE_UCX_BOOTSTRAP_DIR="${bdir}" \
    srun --exclusive --mpi="${SRUN_MPI_MODE}" --nodes="${nn}" --ntasks="${nn}" --ntasks-per-node=1 \
         "${BIN}" >>"${log}" 2>&1 || rc=$?
    rm -rf "${bdir}"

    if [ "${rc}" -ne 0 ]; then
        echo "[fail] ${tag} rc=${rc} log=${log}" >&2
        return "${rc}"
    fi
    if grep -q "status=FAIL" "${log}"; then
        echo "[fail] ${tag}: verification failed (see ${log})" >&2
        return 1
    fi
    if ! grep -q "status=OK" "${log}"; then
        echo "[fail] ${tag}: no RESULT line in ${log}" >&2
        return 1
    fi
    echo "[done]   ${tag}"
}

for nn in "${NODES_LIST[@]}"; do
    for r in $(seq 1 ${REPS}); do
        run_one "${nn}" "${r}"
    done
done

echo ""
echo "Done. verify_private logs in: ${RESULTS_DIR}"
