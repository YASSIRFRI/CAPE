#!/bin/bash
#SBATCH --job-name=bench_heat3d_omp
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=32
#SBATCH --time=01:00:00
#SBATCH --output=bench_heat3d_omp_%j.out
#SBATCH --error=bench_heat3d_omp_%j.err
#SBATCH --hint=nomultithread

# 3D diffusion / Jacobi 7-point stencil — plain OpenMP baseline.
#
# Single node, thread sweep 1..32, identical N/iters and identical kernel to
# bench_job_heat3d_dickpt.sh, so the two CSVs are directly comparable:
#   omp   app_ms = compute only
#   cape  app_ms = compute + DICKPT checkpoint path (ckpt_ms)
# Compare with: scripts/bench/compare_heat3d_cape_vs_omp.sh <cape.csv> <omp.csv>
#
# PAPI=1 additionally builds with hardware counters and dumps per-phase
# load/store, L1/L2/L3-miss and TLB-miss totals for omp_sweep / omp_writeback,
# to be read against the monitor's generate_ckpt / par_for_lane regions.

set -euo pipefail

APP="heat3d"
BIN_NAME="omp_heat3d"
N_DIM="${N_DIM:-512}"
N_ITERS="${N_ITERS:-200}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${PROJECT_DIR:-${SLURM_SUBMIT_DIR:-${SCRIPT_DIR}}}"
[ -f "${PROJECT_DIR}/makefile" ] || PROJECT_DIR="${SCRIPT_DIR}/.."
cd "${PROJECT_DIR}"
JOB_TAG="${SLURM_JOB_ID:-local_$$}"

RESULTS_DIR="${RESULTS_DIR:-${SLURM_SUBMIT_DIR:-${PROJECT_DIR}}/results/bench_${APP}_omp_${JOB_TAG}}"
mkdir -p "${RESULTS_DIR}" 2>/dev/null || { RESULTS_DIR="/tmp/${USER}/bench_${APP}_omp_${JOB_TAG}"; mkdir -p "${RESULTS_DIR}"; }
BUILD_DIR="${BUILD_DIR:-${SLURM_SUBMIT_DIR:-/tmp/${USER}}/cape_build_${APP}_omp_${JOB_TAG}}"
mkdir -p "${BUILD_DIR}/bin" 2>/dev/null || { BUILD_DIR="/tmp/${USER}/cape_build_${APP}_omp_${JOB_TAG}"; mkdir -p "${BUILD_DIR}/bin"; }

THREADS_LIST=(${THREADS_LIST:-1 2 4 8 16 32})
REPS="${REPS:-3}"
PAPI="${PAPI:-0}"
CPUS_PER_TASK="${CPUS_PER_TASK:-${SLURM_CPUS_ON_NODE:-32}}"

set +u
module purge
module load GCCcore/14.2.0
if [ "${PAPI}" = "1" ]; then
    module load PAPI/7.1.0-GCCcore-14.2.0 2>/dev/null || module load PAPI 2>/dev/null || true
fi
set -u

MAKE_ARGS=(EXE_FOLDER="${BUILD_DIR}/bin" CC=gcc PAPI="${PAPI}")
if [ "${PAPI}" = "1" ] && [ -n "${EBROOTPAPI:-}" ]; then
    MAKE_ARGS+=(PAPI_ROOT="${EBROOTPAPI}")
fi

make -C "${PROJECT_DIR}" omp_heat3d "${MAKE_ARGS[@]}"
BIN="${BUILD_DIR}/bin/${BIN_NAME}"

CSV="${RESULTS_DIR}/bench_${APP}_omp_${JOB_TAG}.csv"
echo "impl,app,n,d,nodes,threads,rep,app_ms,sweep_ms,writeback_ms,ckpt_ms,start_ms,gen_ms,allred_ms,stop_ms,job_id" > "${CSV}"

echo "Benchmarking OpenMP ${APP} (3D diffusion, shared memory)"
echo "App: src/omp_apps/omp_heat3d.c -> ${BIN}"
echo "Threads: ${THREADS_LIST[*]}  Reps: ${REPS}  N=${N_DIM} iters=${N_ITERS}  PAPI=${PAPI}"
echo "CSV: ${CSV}"

run_one() {
    local nt="$1"
    local tag="omp_${APP}_threads${nt}"
    local log="${RESULTS_DIR}/${tag}.log"
    local rc=0

    : > "${log}"
    echo "[launch] ${tag}"
    OMP_NUM_THREADS="${nt}" CAPE_COMPUTE_THREADS="${nt}" \
    OMP_PROC_BIND=close OMP_PLACES=cores \
    srun --exclusive --nodes=1 --ntasks=1 --cpus-per-task="${CPUS_PER_TASK}" \
         --cpu-bind=none \
         "${BIN}" "${N_DIM}" "${N_ITERS}" "${REPS}" >>"${log}" 2>&1 || rc=$?
    if [ "${rc}" -ne 0 ]; then echo "[fail] ${tag} rc=${rc} log=${log}" >&2; return 0; fi

    awk -v impl="omp" -v app="${APP}" -v nt="${nt}" -v job="${JOB_TAG}" '
        /^RESULT / {
            n=""; dd=""; rep=""; ms=""; sw=""; wb="";
            for (i=1;i<=NF;i++) { split($i,kv,"=");
                if(kv[1]=="n")n=kv[2]; if(kv[1]=="d")dd=kv[2]; if(kv[1]=="rep")rep=kv[2];
                if(kv[1]=="ms")ms=kv[2]; if(kv[1]=="sweep_ms")sw=kv[2]; if(kv[1]=="writeback_ms")wb=kv[2]; }
            if (n!="" && ms!="")
                printf "%s,%s,%s,%s,1,%s,%s,%s,%s,%s,0,0,0,0,0,%s\n",
                       impl,app,n,dd,nt,rep,ms,sw,wb,job;
        }' "${log}" >> "${CSV}"
    echo "[done]   ${tag}"
}

for nt in "${THREADS_LIST[@]}"; do run_one "${nt}"; done

echo ""
echo "=== OpenMP thread scaling (avg ms, speedup vs threads=1) ==="
awk -F, -v job="${JOB_TAG}" '
    NR>1 && $16==job { n[$6]++; app[$6]+=$8; sw[$6]+=$9; wb[$6]+=$10 }
    END {
        if (!(1 in n)) { print "[skip] no threads=1 baseline"; exit }
        printf "%-8s %-10s %-10s %-10s %-10s\n","threads","app_ms","sweep_ms","wb_ms","speedup";
        m = asorti(n, idx);
        a1 = app[1]/n[1];
        for (i=1;i<=m;i++){ t=idx[i]; c=n[t]; a=app[t]/c;
            printf "%-8s %-10.0f %-10.0f %-10.0f %-10.3f\n", t, a, sw[t]/c, wb[t]/c, (a>0)?a1/a:0;
        }
    }' "${CSV}"

if [ "${PAPI}" = "1" ]; then
    echo ""
    echo "=== PAPI per-phase counters (from the run logs) ==="
    grep -h -A 14 '^\[PAPI\] region=' "${RESULTS_DIR}"/omp_${APP}_threads*.log || true
fi

echo ""
echo "Done. OpenMP ${APP} CSV: ${CSV}"
echo "Per-thread-count logs in: ${RESULTS_DIR}"
