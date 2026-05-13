#!/bin/bash
#SBATCH --job-name=cape_verify_private
#SBATCH --nodes=2
#SBATCH --ntasks=2
#SBATCH --ntasks-per-node=1
#SBATCH --time=00:10:00
#SBATCH --output=verify_private_%j.out
#SBATCH --error=verify_private_%j.err
#SBATCH --partition=compute
#
# verify_private.sh — end-to-end correctness check for the private-memory
# masking added in cape_incr_bitmap.c (is_address_shared + whitelist).
#
# Pipeline:
#   1. Transpile  verify_private.c   ->  verify_private_dickpt.c   (via TXL)
#   2. Build      cape_dickpt_bitmap_monitor  AND  verify_private_dickpt
#   3. Launch     monitor verify_private_dickpt   on N nodes (default 2)
#   4. Assert     EVERY rank printed "status=OK" exactly once.
#
# Failure modes the script catches:
#   - TXL transformation failed                            -> exit 10
#   - Compilation failed                                   -> exit 11
#   - A rank reported "status=FAIL ..." (private clobber)  -> exit 12
#   - Wrong number of OK lines (missing rank / crash)      -> exit 13
#
# Slurm-aware but works locally too: if `srun` is unavailable we launch
# NRANKS monitor processes against a shared CAPE_UCX_BOOTSTRAP_DIR.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${PROJECT_DIR:-${SCRIPT_DIR}}"
TXL_DIR="${TXL_DIR:-${PROJECT_DIR}/../20171001 TXL Transform/20171001 TXL Transform/openmptodickpt}"
NRANKS="${NRANKS:-2}"
JOB_TAG="${SLURM_JOB_ID:-local_$$}"

RESULTS_DIR="${RESULTS_DIR:-${PROJECT_DIR}/results/verify_private_${JOB_TAG}}"
mkdir -p "${RESULTS_DIR}"
BUILD_DIR="${BUILD_DIR:-/tmp/${USER}/cape_verify_private_${JOB_TAG}}"
mkdir -p "${BUILD_DIR}/bin" "${BUILD_DIR}/obj" "${BUILD_DIR}/lib"
BOOTSTRAP_ROOT="${BOOTSTRAP_ROOT:-${BUILD_DIR}/ucx_bootstrap}"
mkdir -p "${BOOTSTRAP_ROOT}"
LOG="${RESULTS_DIR}/verify_private.log"
: > "${LOG}"

log() { echo "[verify] $*" | tee -a "${LOG}" >&2; }

# ─── Step 1: TXL transpile ────────────────────────────────────────────────────
log "step 1: transpiling verify_private.c (TXL)"
SRC="${TXL_DIR}/verify_private.c"
OUT="${TXL_DIR}/verify_private_dickpt.c"
if [ ! -f "${SRC}" ]; then
    log "ERROR: source ${SRC} not found"
    exit 10
fi
( cd "${TXL_DIR}" && txl verify_private.c omptodickpt.Txl -o verify_private_dickpt.c ) >>"${LOG}" 2>&1 \
    || { log "TXL failed"; exit 10; }

# ─── Step 2: Build monitor + dickpt binary ───────────────────────────────────
log "step 2: building monitor + transpiled binary"
make -C "${PROJECT_DIR}" dickpt_bitmap_monitor \
    EXE_FOLDER="${BUILD_DIR}/bin" O_FOLDER="${BUILD_DIR}/obj" L_FOLDER="${BUILD_DIR}/lib" \
    >>"${LOG}" 2>&1 || { log "monitor build failed"; exit 11; }

BIN="${BUILD_DIR}/bin/verify_private_dickpt"
# Compile the transpiled .c the same way DICKPT_COMPILE compiles apps.
cc -w -O2 -mno-red-zone -I"${PROJECT_DIR}/include" -no-pie -fno-pie \
    "${OUT}" "${PROJECT_DIR}/src/apps/cape_dickpt_runtime.c" \
    -o "${BIN}" >>"${LOG}" 2>&1 || { log "app build failed"; exit 11; }

MONITOR="${BUILD_DIR}/bin/cape_dickpt_bitmap_monitor"
[ -x "${MONITOR}" ] || { log "monitor binary missing"; exit 11; }
[ -x "${BIN}" ]     || { log "app binary missing";     exit 11; }

# ─── Step 3: Launch ──────────────────────────────────────────────────────────
BID="${JOB_TAG}_verify_private"
BDIR="${BOOTSTRAP_ROOT}/${BID}"
rm -rf "${BDIR}"; mkdir -p "${BDIR}"
RUN_LOG="${RESULTS_DIR}/run.log"
: > "${RUN_LOG}"

if command -v srun >/dev/null 2>&1 && [ -n "${SLURM_JOB_ID:-}" ]; then
    log "step 3: launching under srun on ${NRANKS} ranks"
    CAPE_UCX_BOOTSTRAP_ID="${BID}" CAPE_UCX_BOOTSTRAP_DIR="${BDIR}" \
        srun --mpi="${SRUN_MPI_MODE:-none}" \
             --nodes="${NRANKS}" --ntasks="${NRANKS}" --ntasks-per-node=1 \
             "${MONITOR}" "${BIN}" >>"${RUN_LOG}" 2>&1 \
        || { log "srun returned non-zero (continuing to validate output)"; }
else
    log "step 3: launching ${NRANKS} local ranks (no slurm)"
    pids=()
    for ((r=0; r<NRANKS; r++)); do
        CAPE_UCX_BOOTSTRAP_ID="${BID}" CAPE_UCX_BOOTSTRAP_DIR="${BDIR}" \
            CAPE_UCX_BOOTSTRAP_RANK="${r}" \
            CAPE_UCX_BOOTSTRAP_SIZE="${NRANKS}" \
            "${MONITOR}" "${BIN}" >>"${RUN_LOG}" 2>&1 &
        pids+=($!)
    done
    rc_any=0
    for p in "${pids[@]}"; do wait "${p}" || rc_any=1; done
    [ "${rc_any}" -eq 0 ] || log "at least one local rank returned non-zero (continuing to validate output)"
fi
rm -rf "${BDIR}"

# ─── Step 4: Validate ────────────────────────────────────────────────────────
log "step 4: validating output (${RUN_LOG})"
ok_count=$(grep -c "status=OK"   "${RUN_LOG}" || true)
fail_count=$(grep -c "status=FAIL" "${RUN_LOG}" || true)

log "OK=${ok_count}  FAIL=${fail_count}  expected_OK=${NRANKS}"

if [ "${fail_count}" -gt 0 ]; then
    log "FAIL: at least one rank reported private-memory corruption"
    grep "status=FAIL\|CLOBBERED" "${RUN_LOG}" | head -20 | tee -a "${LOG}"
    exit 12
fi

if [ "${ok_count}" -ne "${NRANKS}" ]; then
    log "FAIL: expected ${NRANKS} OK lines, got ${ok_count} (rank crashed or missing)"
    tail -40 "${RUN_LOG}" | tee -a "${LOG}"
    exit 13
fi

log "PASS: all ${NRANKS} ranks reported status=OK"
