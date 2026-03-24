#!/bin/bash
#SBATCH --job-name=cape_ucx_bench
#SBATCH --nodes=4
#SBATCH --ntasks=4
#SBATCH --ntasks-per-node=1
#SBATCH --time=00:45:00
#SBATCH --output=cape_ucx_bench_%j.out
#SBATCH --error=cape_ucx_bench_%j.err
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
BUILD_DIR="${BUILD_DIR:-${SLURM_SUBMIT_DIR:-/tmp/${USER:-$(id -un)}}/cape_build_ucx_${JOB_TAG}}"
if ! mkdir -p "${BUILD_DIR}/bin" "${BUILD_DIR}/obj" "${BUILD_DIR}/lib" 2>/dev/null; then
    BUILD_DIR="/tmp/${USER:-$(id -un)}/cape_build_ucx_${JOB_TAG}"
    mkdir -p "${BUILD_DIR}/bin" "${BUILD_DIR}/obj" "${BUILD_DIR}/lib"
fi

N_VALUES_STR="${N_VALUES_STR:-64 96 128 160 192 224 256 320 384 448 512}"
REPS="${REPS:-5}"
RUN_TIMEOUT_SEC="${RUN_TIMEOUT_SEC:-600}"
read -r -a N_VALUES <<< "${N_VALUES_STR}"

module purge
module load GCCcore/13.2.0
module load UCX/1.15.0-GCCcore-13.2.0

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

make -C "${PROJECT_DIR}" cleanall \
    EXE_FOLDER="${BUILD_DIR}/bin" \
    O_FOLDER="${BUILD_DIR}/obj" \
    L_FOLDER="${BUILD_DIR}/lib" 2>/dev/null || true
make -C "${PROJECT_DIR}" cape_mamult \
    EXE_FOLDER="${BUILD_DIR}/bin" \
    O_FOLDER="${BUILD_DIR}/obj" \
    L_FOLDER="${BUILD_DIR}/lib" \
    UCX_SRC="${UCX_INC}" \
    UCX_GEN="${UCX_INC}" \
    UCX_LIB="${UCX_LIB}" \
    "PMIX_FLAGS=${PMIX_FLAGS}" \
    "PMIX_LINK=${PMIX_LINK}" \
    CC=gcc

CSV="${RESULTS_DIR}/ucx_mamult_${JOB_TAG}.csv"
echo "impl,n,rep,app_ms,job_id,nodes,ntasks" > "${CSV}"

echo "Benchmarking UCX cape_mamult"
echo "N values: ${N_VALUES[*]}"
echo "Reps per N: ${REPS}"
echo "Per-N timeout (s): ${RUN_TIMEOUT_SEC}"
echo "Build dir: ${BUILD_DIR}"
echo "CSV: ${CSV}"

for n in "${N_VALUES[@]}"; do
    echo ""
    echo "=== UCX n=${n} reps=${REPS} ==="

    run_log="${BUILD_DIR}/run_ucx_n${n}.log"
    : > "${run_log}"

    set +e
    if command -v timeout >/dev/null 2>&1; then
        UCX_RNDV_THRESH="${UCX_RNDV_THRESH:-65536}" \
        timeout "${RUN_TIMEOUT_SEC}" \
        srun --mpi=pmix \
             --nodes="${SLURM_JOB_NUM_NODES}" \
             --ntasks="${SLURM_NTASKS}" \
             --ntasks-per-node=1 \
             "${BUILD_DIR}/bin/cape_mamult" "${n}" "${REPS}" 2>&1 | tee -a "${run_log}"
        rc=${PIPESTATUS[0]}
    else
        UCX_RNDV_THRESH="${UCX_RNDV_THRESH:-65536}" \
        srun --mpi=pmix \
             --nodes="${SLURM_JOB_NUM_NODES}" \
             --ntasks="${SLURM_NTASKS}" \
             --ntasks-per-node=1 \
             "${BUILD_DIR}/bin/cape_mamult" "${n}" "${REPS}" 2>&1 | tee -a "${run_log}"
        rc=${PIPESTATUS[0]}
    fi
    set -e

    if [ "${rc}" -ne 0 ]; then
        echo "WARN: UCX run failed or timed out for n=${n} (rc=${rc}). See ${run_log}" >&2
        continue
    fi

    awk -v impl="ucx" \
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
echo "Done. UCX benchmark CSV: ${CSV}"
