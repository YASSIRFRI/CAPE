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

REPS="${REPS:-5}"

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
else
    PMIX_FLAGS=""
    PMIX_LINK=""
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
make -C "${PROJECT_DIR}" dickpt "${MAKE_ARGS[@]}"

MONITOR="${BUILD_DIR}/bin/cape_dickpt_monitor"
APP="${BUILD_DIR}/bin/dickpt_mul_manual"

CSV="${RESULTS_DIR}/dickpt_mamult_${JOB_TAG}.csv"
echo "impl,rep,app_ms,job_id,nodes,ntasks" > "${CSV}"

echo "Benchmarking DICKPT cape_mul_manual"
echo "Reps: ${REPS}"
echo "Build dir: ${BUILD_DIR}"
echo "CSV: ${CSV}"

for rep in $(seq 1 "${REPS}"); do
    echo ""
    echo "=== DICKPT rep=${rep}/${REPS} ==="

    run_log="${BUILD_DIR}/run_dickpt_rep${rep}.log"
    : > "${run_log}"

    set +e
    srun --mpi=pmix \
         --nodes="${SLURM_JOB_NUM_NODES}" \
         --ntasks="${SLURM_NTASKS}" \
         --ntasks-per-node=1 \
         "${MONITOR}" "${APP}" 2>&1 | tee -a "${run_log}"
    rc=${PIPESTATUS[0]}
    set -e

    if [ "${rc}" -ne 0 ]; then
        echo "WARN: dickpt run failed for rep=${rep} (rc=${rc}). See ${run_log}" >&2
        continue
    fi

    awk -v impl="dickpt" \
        -v rep="${rep}" \
        -v job="${SLURM_JOB_ID}" \
        -v nodes="${SLURM_JOB_NUM_NODES}" \
        -v tasks="${SLURM_NTASKS}" '
        /^RESULT / {
            ms="";
            for (i=1; i<=NF; i++) {
                split($i, kv, "=");
                if (kv[1] == "ms") ms = kv[2];
            }
            if (ms != "")
                printf "%s,%s,%s,%s,%s,%s\n", impl, rep, ms, job, nodes, tasks;
        }' "${run_log}" >> "${CSV}"
done

echo ""
echo "Done. DICKPT benchmark CSV: ${CSV}"
