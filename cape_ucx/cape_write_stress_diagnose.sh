#!/bin/bash
#SBATCH --job-name=cape_ws_diag
#SBATCH --nodes=32
#SBATCH --ntasks=32
#SBATCH --ntasks-per-node=1
#SBATCH --time=04:00:00
#SBATCH --output=cape_write_stress_diagnose_%j.out
#SBATCH --error=cape_write_stress_diagnose_%j.err
#SBATCH --partition=compute

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${PROJECT_DIR:-${SLURM_SUBMIT_DIR:-${SCRIPT_DIR}}}"
[ -f "${PROJECT_DIR}/makefile" ] || PROJECT_DIR="${SCRIPT_DIR}"
cd "${PROJECT_DIR}"
JOB_TAG="${SLURM_JOB_ID:-local_$$}"

RESULTS_DIR="${RESULTS_DIR:-${SLURM_SUBMIT_DIR:-${PROJECT_DIR}}/results/cape_write_stress_diagnose_${JOB_TAG}}"
mkdir -p "${RESULTS_DIR}" 2>/dev/null || { RESULTS_DIR="/tmp/${USER}/cape_write_stress_diagnose_${JOB_TAG}"; mkdir -p "${RESULTS_DIR}"; }
BUILD_DIR="${BUILD_DIR:-${SLURM_SUBMIT_DIR:-/tmp/${USER}}/cape_build_ucx_ws_diag_${JOB_TAG}}"
mkdir -p "${BUILD_DIR}/bin" "${BUILD_DIR}/obj" "${BUILD_DIR}/lib" 2>/dev/null || \
  { BUILD_DIR="/tmp/${USER}/cape_build_ucx_ws_diag_${JOB_TAG}"; mkdir -p "${BUILD_DIR}/bin" "${BUILD_DIR}/obj" "${BUILD_DIR}/lib"; }

N="${N:-1048576}"
PHASES="${PHASES:-8}"
NODES="${NODES:-32}"
REPS="${REPS:-5}"
PROFILE="${PROFILE:-1}"
UCX_RNDV_THRESH_VALUE="${UCX_RNDV_THRESH:-65536}"
CAPE_UCX_DIAG="${CAPE_UCX_DIAG:-1}"
CAPE_UCX_DIAG_SLOW_MS="${CAPE_UCX_DIAG_SLOW_MS:-1000}"

module purge
module load GCCcore/14.2.0
module load UCX/1.18.0-GCCcore-14.2.0

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

make -C "${PROJECT_DIR}" cleanall \
    EXE_FOLDER="${BUILD_DIR}/bin" O_FOLDER="${BUILD_DIR}/obj" L_FOLDER="${BUILD_DIR}/lib" 2>/dev/null || true
make -C "${PROJECT_DIR}" cape_write_stress PROFILE="${PROFILE}" "${MAKE_ARGS[@]}"

BIN="${BUILD_DIR}/bin/cape_write_stress"
CSV="${RESULTS_DIR}/cape_write_stress_diagnose_summary_${JOB_TAG}.csv"
PROFILE_FOCUS="${RESULTS_DIR}/cape_write_stress_diagnose_profile_focus_${JOB_TAG}.txt"
META="${RESULTS_DIR}/cape_write_stress_diagnose_meta_${JOB_TAG}.txt"
HOSTS="${RESULTS_DIR}/cape_write_stress_diagnose_hosts_${JOB_TAG}.txt"

echo "impl,app,n,phases,nodes,rep,app_ms,job_id" > "${CSV}"
: > "${PROFILE_FOCUS}"

{
    echo "date=$(date -Is)"
    echo "project_dir=${PROJECT_DIR}"
    echo "build_dir=${BUILD_DIR}"
    echo "results_dir=${RESULTS_DIR}"
    echo "bin=${BIN}"
    echo "n=${N}"
    echo "phases=${PHASES}"
    echo "nodes=${NODES}"
    echo "reps=${REPS}"
    echo "profile=${PROFILE}"
    echo "cape_ucx_diag=${CAPE_UCX_DIAG}"
    echo "cape_ucx_diag_slow_ms=${CAPE_UCX_DIAG_SLOW_MS}"
    echo "srun_mpi_mode=${SRUN_MPI_MODE}"
    echo "ucx_inc=${UCX_INC}"
    echo "ucx_lib=${UCX_LIB}"
    echo "ucx_rndv_thresh=${UCX_RNDV_THRESH_VALUE}"
    echo ""
    echo "=== module list ==="
    module list 2>&1 || true
    echo ""
    echo "=== ucx_info -v ==="
    ucx_info -v 2>&1 || true
    echo ""
    echo "=== selected ucx_info -c ==="
    ucx_info -c 2>&1 | grep -E '^(UCX_(TLS|NET_DEVICES|SHM_DEVICES|RNDV|PROTO|IB_|RC_|UD_|REG_|MEMTYPE|MAX_RNDV|SOCKADDR|WIREUP))' || true
    echo ""
    echo "=== selected environment ==="
    env | sort | grep -E '^(CAPE_UCX|UCX|SLURM|PMI|PMIX|OMPI|EBROOT|PROFILE|N=|PHASES=|REPS=|NODES=|SRUN_MPI_MODE=)' || true
} > "${META}" 2>&1

srun --mpi="${SRUN_MPI_MODE}" --nodes="${NODES}" --ntasks="${NODES}" --ntasks-per-node=1 \
     hostname 2>/dev/null | sort | uniq -c > "${HOSTS}" || true

echo "Diagnosing CAPE write_stress"
echo "Nodes: ${NODES}  N: ${N}  Phases: ${PHASES}  Reps: ${REPS}  PROFILE=${PROFILE}  MPI mode: ${SRUN_MPI_MODE}"
echo "UCX_RNDV_THRESH=${UCX_RNDV_THRESH_VALUE}"
echo "CAPE_UCX_DIAG=${CAPE_UCX_DIAG}  CAPE_UCX_DIAG_SLOW_MS=${CAPE_UCX_DIAG_SLOW_MS}"
echo "Results dir: ${RESULTS_DIR}"

extract_profile_focus() {
    local tag="$1" log="$2"
    {
        echo ""
        echo "===== ${tag} ====="
        awk '
            /^\[CAPE UCX DIAG\]/ ||
            /^\[CAPE PROFILE\] Node/ ||
            /allreduce    \(total\)/ ||
            /size exchange \(sendrecv\)/ ||
            /data exchange \(sendrecv\)/ ||
            /merge_checkpoint/ ||
            /other \(malloc\/free\/overhead\)/ ||
            /UCX recv wait/ ||
            /UCX send wait/ ||
            /-- UCX init\/bootstrap --/ ||
            /cape_init   \(total\)/ ||
            /ucp_config_read/ ||
            /ucp_init/ ||
            /ucp_worker_create/ ||
            /ucp_worker_get_address/ ||
            /fs bootstrap total/ ||
            /write_addr=/ ||
            /pmix bootstrap total/ ||
            /put_commit=/ ||
            /ucp_ep_create total/ ||
            /allreduce arrival \(realtime\)/ ||
            /cape_end     \(wall, full\)/ ||
            /cape_sync_ckpt \(allreduce\+inj\)/ ||
            /USER COMPUTE between ckpts/ ||
            /step [0-9]+ partner=/ { print }
        ' "${log}"
    } >> "${PROFILE_FOCUS}"
}

run_one() {
    local rep="$1"
    local tag="cape_write_stress_diagnose_n${N}_p${PHASES}_nodes${NODES}_rep${rep}"
    local log="${RESULTS_DIR}/${tag}.log"
    : > "${log}"
    echo "[launch] ${tag}"
    local rc=0
    UCX_RNDV_THRESH="${UCX_RNDV_THRESH_VALUE}" \
    CAPE_UCX_DIAG="${CAPE_UCX_DIAG}" \
    CAPE_UCX_DIAG_SLOW_MS="${CAPE_UCX_DIAG_SLOW_MS}" \
    srun --exclusive --mpi="${SRUN_MPI_MODE}" --nodes="${NODES}" --ntasks="${NODES}" --ntasks-per-node=1 \
         "${BIN}" "${N}" "${PHASES}" 1 >>"${log}" 2>&1 || rc=$?
    extract_profile_focus "${tag}" "${log}"
    if [ "${rc}" -ne 0 ]; then
        echo "[fail] ${tag} rc=${rc}" >&2
        return
    fi
    awk -v impl="cape" -v app="write_stress" -v nn="${NODES}" -v rep="${rep}" \
        -v job="${SLURM_JOB_ID:-local}" '
        /^RESULT / {
            n=""; ph=""; ms="";
            for (i=1;i<=NF;i++) { split($i,kv,"="); if(kv[1]=="n")n=kv[2]; if(kv[1]=="phases")ph=kv[2]; if(kv[1]=="ms")ms=kv[2]; }
            if (n!="" && ms!="") printf "%s,%s,%s,%s,%s,%s,%s,%s\n", impl,app,n,ph,nn,rep,ms,job;
        }' "${log}" >> "${CSV}"
    echo "[done]   ${tag}"
}

for rep in $(seq 1 "${REPS}"); do
    run_one "${rep}"
done

echo ""
echo "Done. CAPE diagnose summary CSV: ${CSV}"
echo "Focused profile snippets: ${PROFILE_FOCUS}"
echo "Job metadata: ${META}"
echo "Host allocation: ${HOSTS}"
echo "Per-rep logs in: ${RESULTS_DIR}"
