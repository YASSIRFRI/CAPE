#!/bin/bash
#SBATCH --job-name=dickpt_bitmap_write_sparce
#SBATCH --nodes=32
#SBATCH --ntasks=32
#SBATCH --ntasks-per-node=1
#SBATCH --time=04:00:00
#SBATCH --output=dickpt_bitmap_write_sparce_%j.out
#SBATCH --error=dickpt_bitmap_write_sparce_%j.err
#SBATCH --partition=compute
#SBATCH --mem=8G

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${PROJECT_DIR:-${SLURM_SUBMIT_DIR:-${SCRIPT_DIR}}}"
[ -f "${PROJECT_DIR}/makefile" ] || PROJECT_DIR="${SCRIPT_DIR}"
cd "${PROJECT_DIR}"
JOB_TAG="${SLURM_JOB_ID:-local_$$}"

RESULTS_DIR="${RESULTS_DIR:-${SLURM_SUBMIT_DIR:-${PROJECT_DIR}}/results/dickpt_bitmap_write_sparce_${JOB_TAG}}"
mkdir -p "${RESULTS_DIR}" 2>/dev/null || { RESULTS_DIR="/tmp/${USER}/dickpt_bitmap_write_sparce_${JOB_TAG}"; mkdir -p "${RESULTS_DIR}"; }
BUILD_DIR="${BUILD_DIR:-${SLURM_SUBMIT_DIR:-/tmp/${USER}}/cape_build_dickpt_sparse_${JOB_TAG}}"
mkdir -p "${BUILD_DIR}/bin" "${BUILD_DIR}/obj" "${BUILD_DIR}/lib" 2>/dev/null || \
  { BUILD_DIR="/tmp/${USER}/cape_build_dickpt_sparse_${JOB_TAG}"; mkdir -p "${BUILD_DIR}/bin" "${BUILD_DIR}/obj" "${BUILD_DIR}/lib"; }
BOOTSTRAP_ROOT="${BOOTSTRAP_ROOT:-${BUILD_DIR}/ucx_bootstrap}"
mkdir -p "${BOOTSTRAP_ROOT}"

GRID_BYTES_LIST=(${GRID_BYTES_LIST:-2147483648})
UPDATES_LIST=(${UPDATES_LIST:-5000})
PHASES_LIST=(${PHASES_LIST:-1})
NODES_LIST=(${NODES_LIST:-4 8 16 32})
REPS="${REPS:-5}"
PROFILE="${PROFILE:-1}"
CAPE_UCX_SCRATCH_BYTES="${CAPE_UCX_SCRATCH_BYTES:-268435456}"

module purge
module load GCCcore/14.2.0
module load UCX/1.18.0-GCCcore-14.2.0
module load UCC/1.3.0-GCCcore-14.2.0
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

if [ -n "${EBROOTUCC:-}" ]; then
    UCC_INC="${EBROOTUCC}/include"; UCC_LIB="${EBROOTUCC}/lib"
else
    echo "ERROR: cannot locate UCC installation (EBROOTUCC unset)." >&2; exit 1
fi

MAKE_ARGS=(
    EXE_FOLDER="${BUILD_DIR}/bin" O_FOLDER="${BUILD_DIR}/obj" L_FOLDER="${BUILD_DIR}/lib"
    UCX_SRC="${UCX_INC}" UCX_GEN="${UCX_INC}" UCX_LIB="${UCX_LIB}"
    UCC_SRC="${UCC_INC}" UCC_LIB="${UCC_LIB}"
    "PMIX_FLAGS=${PMIX_FLAGS}" "PMIX_LINK=${PMIX_LINK}" CC=gcc
)

make -C "${PROJECT_DIR}" cleanall \
    EXE_FOLDER="${BUILD_DIR}/bin" O_FOLDER="${BUILD_DIR}/obj" L_FOLDER="${BUILD_DIR}/lib" 2>/dev/null || true
make -C "${PROJECT_DIR}" dickpt_bitmap_monitor dickpt_bitmap_write_space_manual PROFILE="${PROFILE}" "${MAKE_ARGS[@]}"

MONITOR="${BUILD_DIR}/bin/cape_dickpt_bitmap_monitor"
BIN="${BUILD_DIR}/bin/dickpt_bitmap_write_space_manual"

CSV="${RESULTS_DIR}/dickpt_bitmap_write_sparce_summary_${JOB_TAG}.csv"
echo "impl,app,grid_bytes,updates,phases,nodes,rep,app_ms,job_id" > "${CSV}"

echo "Benchmarking DICKPT(bitmap) write_sparce"
echo "Nodes: ${NODES_LIST[*]}  Grid bytes: ${GRID_BYTES_LIST[*]}  Updates: ${UPDATES_LIST[*]}  Phases: ${PHASES_LIST[*]}  Reps: ${REPS}  MPI mode: ${SRUN_MPI_MODE}"
echo "CAPE_UCX_SCRATCH_BYTES=${CAPE_UCX_SCRATCH_BYTES}"
echo "Results dir: ${RESULTS_DIR}"

TOTAL_NODES="${SLURM_JOB_NUM_NODES:-32}"

run_one() {
    local grid_bytes="$1" updates="$2" phases="$3" nn="$4" rep="$5"
    local tag="dickpt_bitmap_write_sparce_g${grid_bytes}_u${updates}_p${phases}_nodes${nn}_rep${rep}"
    local log="${RESULTS_DIR}/${tag}.log"
    : > "${log}"
    local bid="${JOB_TAG}_${tag}"
    local bdir="${BOOTSTRAP_ROOT}/${bid}"
    rm -rf "${bdir}"; mkdir -p "${bdir}"
    echo "[launch] ${tag}"
    local rc=0
    CAPE_UCX_BOOTSTRAP_ID="${bid}" CAPE_UCX_BOOTSTRAP_DIR="${bdir}" \
    CAPE_UCX_SCRATCH_BYTES="${CAPE_UCX_SCRATCH_BYTES}" \
    srun --exclusive --mpi="${SRUN_MPI_MODE}" --nodes="${nn}" --ntasks="${nn}" --ntasks-per-node=1 \
         "${MONITOR}" "${BIN}" "${grid_bytes}" "${updates}" "${phases}" 1 >>"${log}" 2>&1 || rc=$?
    rm -rf "${bdir}"
    if [ "${rc}" -ne 0 ]; then
        echo "[fail] ${tag} rc=${rc}" >&2; return
    fi
    awk -v impl="dickpt_bitmap" -v app="write_sparce" -v nn="${nn}" -v rep="${rep}" \
        -v job="${SLURM_JOB_ID:-local}" '
        /^RESULT / {
            gb=""; up=""; ph=""; ms="";
            for (i=1;i<=NF;i++) {
                split($i,kv,"=");
                if(kv[1]=="grid_bytes")gb=kv[2];
                if(kv[1]=="updates")up=kv[2];
                if(kv[1]=="phases")ph=kv[2];
                if(kv[1]=="ms")ms=kv[2];
            }
            if (gb!="" && up!="" && ms!="") printf "%s,%s,%s,%s,%s,%s,%s,%s,%s\n", impl,app,gb,up,ph,nn,rep,ms,job;
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
        IFS='|' read -r _gb _u _p _rep <<< "${j}"
        run_one "${_gb}" "${_u}" "${_p}" "${nn}" "${_rep}" &
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
        for u in "${UPDATES_LIST[@]}"; do
            for ph in "${PHASES_LIST[@]}"; do
                for r in $(seq 1 ${REPS}); do
                    JOBS+=("${gb}|${u}|${ph}|${r}")
                done
            done
        done
    done
    run_batch "${nn}" "${JOBS[@]}"
done

echo ""
echo "Done. DICKPT bitmap write_sparce summary CSV: ${CSV}"
echo "Per-experiment logs in: ${RESULTS_DIR}"
