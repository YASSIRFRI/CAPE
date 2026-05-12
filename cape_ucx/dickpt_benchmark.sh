#!/bin/bash
#SBATCH --job-name=dickpt_bitmap_full_bench
#SBATCH --nodes=32
#SBATCH --ntasks=32
#SBATCH --ntasks-per-node=1
#SBATCH --time=04:00:00
#SBATCH --output=dickpt_bitmap_full_bench_%j.out
#SBATCH --error=dickpt_bitmap_full_bench_%j.err
#SBATCH --partition=compute

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${PROJECT_DIR:-${SLURM_SUBMIT_DIR:-${SCRIPT_DIR}}}"
[ -f "${PROJECT_DIR}/makefile" ] || PROJECT_DIR="${SCRIPT_DIR}"
cd "${PROJECT_DIR}"
JOB_TAG="${SLURM_JOB_ID:-local_$$}"

RESULTS_DIR="${RESULTS_DIR:-${SLURM_SUBMIT_DIR:-${PROJECT_DIR}}/results/dickpt_bitmap_${JOB_TAG}}"
mkdir -p "${RESULTS_DIR}" 2>/dev/null || { RESULTS_DIR="/tmp/${USER}/dickpt_bitmap_results_${JOB_TAG}"; mkdir -p "${RESULTS_DIR}"; }
BUILD_DIR="${BUILD_DIR:-${SLURM_SUBMIT_DIR:-/tmp/${USER}}/cape_build_dickpt_bitmap_${JOB_TAG}}"
mkdir -p "${BUILD_DIR}/bin" "${BUILD_DIR}/obj" "${BUILD_DIR}/lib" 2>/dev/null || \
  { BUILD_DIR="/tmp/${USER}/cape_build_dickpt_bitmap_${JOB_TAG}"; mkdir -p "${BUILD_DIR}/bin" "${BUILD_DIR}/obj" "${BUILD_DIR}/lib"; }
BOOTSTRAP_ROOT="${BOOTSTRAP_ROOT:-${BUILD_DIR}/ucx_bootstrap}"
mkdir -p "${BOOTSTRAP_ROOT}"

N_MAMULT=(${N_MAMULT:-3000 })
N_MATVEC=(${N_MATVEC:-4096})
GRADIENT_PAIRS=(${GRADIENT_PAIRS:-"4096:256" "8192:512"})
N_MEMWRITE=(${N_MEMWRITE:-1048576})
NODES_LIST=(${NODES_LIST:-4 8 16 32})
REPS="${REPS:-5}"
PROFILE="${PROFILE:-0}"

module purge
module load GCCcore/14.2.0
module load UCX/1.18.0-GCCcore-14.2.0
module load UCC/1.3.0-GCCcore-14.2.0 2>/dev/null || module load UCC 2>/dev/null || true
# PMIx is required for the fast in-memory bootstrap. Without it dickpt
# falls back to the shared-FS bootstrap (~30 s stall at 32 nodes).
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

MAKE_ARGS=(
    EXE_FOLDER="${BUILD_DIR}/bin" O_FOLDER="${BUILD_DIR}/obj" L_FOLDER="${BUILD_DIR}/lib"
    UCX_SRC="${UCX_INC}" UCX_GEN="${UCX_INC}" UCX_LIB="${UCX_LIB}"
    "PMIX_FLAGS=${PMIX_FLAGS}" "PMIX_LINK=${PMIX_LINK}" CC=gcc
)

if [ -n "${EBROOTUCC:-}" ]; then
    MAKE_ARGS+=(UCC_SRC="${EBROOTUCC}/include" UCC_LIB="${EBROOTUCC}/lib")
fi

make -C "${PROJECT_DIR}" cleanall \
    EXE_FOLDER="${BUILD_DIR}/bin" O_FOLDER="${BUILD_DIR}/obj" L_FOLDER="${BUILD_DIR}/lib" 2>/dev/null || true
make -C "${PROJECT_DIR}" dickpt_bitmap_monitor \
    dickpt_mul_manual dickpt_matvec_manual dickpt_gradient_manual dickpt_memwrite_manual \
    PROFILE="${PROFILE}" "${MAKE_ARGS[@]}"

MONITOR="${BUILD_DIR}/bin/cape_dickpt_bitmap_monitor"

CSV="${RESULTS_DIR}/dickpt_bitmap_summary_${JOB_TAG}.csv"
echo "impl,app,n,d,nodes,rep,app_ms,job_id" > "${CSV}"

echo "Benchmarking DICKPT(bitmap)"
echo "Monitor: src/monitor/cape_incr_bitmap.c -> ${MONITOR}"
echo "Nodes: ${NODES_LIST[*]}  Reps/cell: ${REPS}  MPI mode: ${SRUN_MPI_MODE}"
echo "Results dir: ${RESULTS_DIR}"

TOTAL_NODES="${SLURM_JOB_NUM_NODES:-32}"

run_one() {
    local app="$1" bin="$2" n="$3" d="$4" nn="$5" rep="$6"
    local tag="dickpt_bitmap_${app}_n${n}${d:+_d${d}}_nodes${nn}_rep${rep}"
    local log="${RESULTS_DIR}/${tag}.log"
    : > "${log}"
    local bid="${JOB_TAG}_${tag}"
    local bdir="${BOOTSTRAP_ROOT}/${bid}"
    rm -rf "${bdir}"; mkdir -p "${bdir}"
    echo "[launch] ${tag}"
    local rc=0
    if [ -n "${d}" ]; then
        CAPE_UCX_BOOTSTRAP_ID="${bid}" CAPE_UCX_BOOTSTRAP_DIR="${bdir}" \
        srun --exclusive --mpi="${SRUN_MPI_MODE}" --nodes="${nn}" --ntasks="${nn}" --ntasks-per-node=1 \
             "${MONITOR}" "${bin}" "${n}" "${d}" 1 >>"${log}" 2>&1 || rc=$?
    else
        CAPE_UCX_BOOTSTRAP_ID="${bid}" CAPE_UCX_BOOTSTRAP_DIR="${bdir}" \
        srun --exclusive --mpi="${SRUN_MPI_MODE}" --nodes="${nn}" --ntasks="${nn}" --ntasks-per-node=1 \
             "${MONITOR}" "${bin}" "${n}" 1 >>"${log}" 2>&1 || rc=$?
    fi
    rm -rf "${bdir}"
    if [ "${rc}" -ne 0 ]; then
        echo "[fail] ${tag} rc=${rc}" >&2; return
    fi
    awk -v impl="dickpt_bitmap" -v app="${app}" -v nn="${nn}" -v rep="${rep}" \
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
    for n in "${N_MAMULT[@]}"; do for r in $(seq 1 ${REPS}); do JOBS+=("mul_manual|${BUILD_DIR}/bin/dickpt_mul_manual|${n}||${r}"); done; done
    for n in "${N_MATVEC[@]}"; do for r in $(seq 1 ${REPS}); do JOBS+=("matvec|${BUILD_DIR}/bin/dickpt_matvec_manual|${n}||${r}"); done; done
    for pair in "${GRADIENT_PAIRS[@]}"; do
        n="${pair%:*}"; d="${pair#*:}"
        for r in $(seq 1 ${REPS}); do JOBS+=("gradient|${BUILD_DIR}/bin/dickpt_gradient_manual|${n}|${d}|${r}"); done
    done
    for n in "${N_MEMWRITE[@]}"; do for r in $(seq 1 ${REPS}); do JOBS+=("memwrite|${BUILD_DIR}/bin/dickpt_memwrite_manual|${n}||${r}"); done; done
    run_batch "${nn}" "${JOBS[@]}"
done

echo ""
echo "Done. DICKPT bitmap summary CSV: ${CSV}"
echo "Per-experiment logs in: ${RESULTS_DIR}"
