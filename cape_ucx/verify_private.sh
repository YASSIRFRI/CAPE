#!/bin/bash
#SBATCH --job-name=cape_verify_private
#SBATCH --nodes=4
#SBATCH --ntasks=4
#SBATCH --ntasks-per-node=1
#SBATCH --time=00:10:00
#SBATCH --output=verify_private_%j.out
#SBATCH --error=verify_private_%j.err
#SBATCH --partition=compute
#
# verify_private.sh — end-to-end correctness check for the private-memory
# masking added in cape_incr_bitmap.c (is_address_shared + whitelist).
#
# Pre-transpiled input: verify_private_dickpt.c (committed alongside this
# script; produced offline by running TXL on verify_private.c). TXL is NOT
# required on the cluster.
#
# What this job does:
#   1. Build  cape_dickpt_bitmap_monitor
#   2. Build  verify_private_dickpt   (the transpiled binary)
#   3. Launch monitor verify_private_dickpt   on 4 nodes via srun
#   4. Assert every rank printed "status=OK" exactly once.
#
# Exit codes:
#   11 - build failed
#   12 - a rank reported "status=FAIL ..." (private memory was clobbered)
#   13 - missing OK lines (rank crashed / hung)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${PROJECT_DIR:-${SLURM_SUBMIT_DIR:-${SCRIPT_DIR}}}"
[ -f "${PROJECT_DIR}/makefile" ] || PROJECT_DIR="${SCRIPT_DIR}"
cd "${PROJECT_DIR}"

# Pre-transpiled source lives in the TXL workspace (sibling to cape_ucx).
DICKPT_SRC_DEFAULT="${PROJECT_DIR}/../20171001 TXL Transform/20171001 TXL Transform/openmptodickpt/verify_private_dickpt.c"
DICKPT_SRC="${DICKPT_SRC:-${DICKPT_SRC_DEFAULT}}"

NRANKS="${NRANKS:-4}"
JOB_TAG="${SLURM_JOB_ID:-local_$$}"

RESULTS_DIR="${RESULTS_DIR:-${SLURM_SUBMIT_DIR:-${PROJECT_DIR}}/results/verify_private_${JOB_TAG}}"
mkdir -p "${RESULTS_DIR}" 2>/dev/null || { RESULTS_DIR="/tmp/${USER}/verify_private_${JOB_TAG}"; mkdir -p "${RESULTS_DIR}"; }
BUILD_DIR="${BUILD_DIR:-${SLURM_SUBMIT_DIR:-/tmp/${USER}}/cape_build_verify_private_${JOB_TAG}}"
mkdir -p "${BUILD_DIR}/bin" "${BUILD_DIR}/obj" "${BUILD_DIR}/lib" 2>/dev/null \
    || { BUILD_DIR="/tmp/${USER}/cape_build_verify_private_${JOB_TAG}"; mkdir -p "${BUILD_DIR}/bin" "${BUILD_DIR}/obj" "${BUILD_DIR}/lib"; }
BOOTSTRAP_ROOT="${BOOTSTRAP_ROOT:-${BUILD_DIR}/ucx_bootstrap}"
mkdir -p "${BOOTSTRAP_ROOT}"
LOG="${RESULTS_DIR}/verify_private.log"
: > "${LOG}"

log() { echo "[verify] $*" | tee -a "${LOG}" >&2; }

# ─── Module / UCX setup (mirrors dickpt_bitmap_write_stress.sh) ──────────────
module purge 2>/dev/null || true
module load GCCcore/14.2.0 2>/dev/null || true
module load UCX/1.18.0-GCCcore-14.2.0 2>/dev/null || true
module load UCC/1.3.0-GCCcore-14.2.0 2>/dev/null || true
module load PMIx/5.0.6-GCCcore-14.2.0 2>/dev/null || module load PMIx 2>/dev/null || true

if [ -n "${EBROOTUCX:-}" ]; then
    UCX_INC="${EBROOTUCX}/include"; UCX_LIB="${EBROOTUCX}/lib"
elif command -v ucx_info >/dev/null 2>&1; then
    UCX_PREFIX=$(ucx_info -v 2>/dev/null | awk '/^# Library/ {print $NF}' | sed 's|/lib.*||')
    UCX_INC="${UCX_PREFIX}/include"; UCX_LIB="${UCX_PREFIX}/lib"
else
    log "ERROR: cannot locate UCX installation"; exit 11
fi

SRUN_MPI_MODE="${SRUN_MPI_MODE:-none}"

# ─── Step 1: locate pre-transpiled source ────────────────────────────────────
if [ ! -f "${DICKPT_SRC}" ]; then
    log "ERROR: transpiled source not found: ${DICKPT_SRC}"
    log "       (run TXL offline, or set DICKPT_SRC=/path/to/verify_private_dickpt.c)"
    exit 11
fi
log "using transpiled source: ${DICKPT_SRC}"

# ─── Step 2: build monitor + transpiled binary ──────────────────────────────
log "building cape_dickpt_bitmap_monitor"
make -C "${PROJECT_DIR}" cleanall \
    EXE_FOLDER="${BUILD_DIR}/bin" O_FOLDER="${BUILD_DIR}/obj" L_FOLDER="${BUILD_DIR}/lib" \
    >>"${LOG}" 2>&1 || true
make -C "${PROJECT_DIR}" dickpt_bitmap_monitor \
    EXE_FOLDER="${BUILD_DIR}/bin" O_FOLDER="${BUILD_DIR}/obj" L_FOLDER="${BUILD_DIR}/lib" \
    >>"${LOG}" 2>&1 || { log "monitor build failed"; exit 11; }

MONITOR="${BUILD_DIR}/bin/cape_dickpt_bitmap_monitor"
BIN="${BUILD_DIR}/bin/verify_private_dickpt"

# Compile the transpiled .c the same way DICKPT_COMPILE compiles apps
# (-mno-red-zone -no-pie -fno-pie; link the dickpt runtime).
log "building transpiled app -> ${BIN}"
cc -w -O2 -mno-red-zone -I"${PROJECT_DIR}/include" -no-pie -fno-pie \
    "${DICKPT_SRC}" "${PROJECT_DIR}/src/apps/cape_dickpt_runtime.c" \
    -o "${BIN}" >>"${LOG}" 2>&1 || { log "app build failed"; exit 11; }

[ -x "${MONITOR}" ] || { log "monitor binary missing"; exit 11; }
[ -x "${BIN}" ]     || { log "app binary missing";     exit 11; }

# ─── Step 3: launch on 4 nodes ───────────────────────────────────────────────
BID="${JOB_TAG}_verify_private"
BDIR="${BOOTSTRAP_ROOT}/${BID}"
rm -rf "${BDIR}"; mkdir -p "${BDIR}"
RUN_LOG="${RESULTS_DIR}/run.log"
: > "${RUN_LOG}"

log "launching srun on ${NRANKS} nodes"
CAPE_UCX_BOOTSTRAP_ID="${BID}" CAPE_UCX_BOOTSTRAP_DIR="${BDIR}" \
    srun --exclusive --mpi="${SRUN_MPI_MODE}" \
         --nodes="${NRANKS}" --ntasks="${NRANKS}" --ntasks-per-node=1 \
         "${MONITOR}" "${BIN}" >>"${RUN_LOG}" 2>&1 \
    || log "srun returned non-zero (continuing to validate output)"
rm -rf "${BDIR}"

# ─── Step 4: validate ────────────────────────────────────────────────────────
log "validating ${RUN_LOG}"
ok_count=$(grep -c "status=OK"   "${RUN_LOG}" || true)
fail_count=$(grep -c "status=FAIL" "${RUN_LOG}" || true)
log "OK=${ok_count}  FAIL=${fail_count}  expected_OK=${NRANKS}"

if [ "${fail_count}" -gt 0 ]; then
    log "FAIL: at least one rank reported private-memory corruption"
    grep -E "status=FAIL|CLOBBERED" "${RUN_LOG}" | head -20 | tee -a "${LOG}"
    exit 12
fi

if [ "${ok_count}" -ne "${NRANKS}" ]; then
    log "FAIL: expected ${NRANKS} OK lines, got ${ok_count} (rank crashed or missing)"
    tail -40 "${RUN_LOG}" | tee -a "${LOG}"
    exit 13
fi

log "PASS: all ${NRANKS} ranks reported status=OK"
