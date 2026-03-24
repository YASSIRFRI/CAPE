#!/bin/bash
#SBATCH --job-name=cape_mpi_bench
#SBATCH --nodes=4
#SBATCH --ntasks=4
#SBATCH --ntasks-per-node=1
#SBATCH --time=00:45:00
#SBATCH --output=cape_mpi_bench_%j.out
#SBATCH --error=cape_mpi_bench_%j.err
#SBATCH --partition=compute

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

RESULTS_DIR="${RESULTS_DIR:-${SLURM_SUBMIT_DIR:-${SCRIPT_DIR}}/results}"
if ! mkdir -p "${RESULTS_DIR}" 2>/dev/null; then
    RESULTS_DIR="/tmp/${USER:-$(id -un)}/cape_results"
    mkdir -p "${RESULTS_DIR}"
fi

N_VALUES_STR="${N_VALUES_STR:-64 96 128 160 192 224 256 320 384 448 512}"
REPS="${REPS:-5}"
SRUN_MPI_FLAG="${SRUN_MPI_FLAG:---mpi=pmix}"
read -r -a N_VALUES <<< "${N_VALUES_STR}"

if command -v module >/dev/null 2>&1; then
    module purge || true
    module load GCCcore/13.2.0 || true
    module load OpenMPI/4.1.6-GCC-13.2.0 || module load OpenMPI || true
fi

mkdir -p bin obj lib
make cleanall 2>/dev/null || true
make cape_mamult

CSV="${RESULTS_DIR}/mpi_mamult_${SLURM_JOB_ID}.csv"
echo "impl,n,rep,app_ms,job_id,nodes,ntasks" > "${CSV}"

echo "Benchmarking MPI cape_mamult"
echo "N values: ${N_VALUES[*]}"
echo "Reps per N: ${REPS}"
echo "CSV: ${CSV}"

for n in "${N_VALUES[@]}"; do
    echo ""
    echo "=== MPI n=${n} reps=${REPS} ==="

    run_out=$(
        srun ${SRUN_MPI_FLAG} \
             --nodes="${SLURM_JOB_NUM_NODES}" \
             --ntasks="${SLURM_NTASKS}" \
             --ntasks-per-node=1 \
             bin/cape_mamult "${n}" "${REPS}"
    )

    echo "${run_out}"

    awk -v impl="mpi" \
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
        }' <<< "${run_out}" >> "${CSV}"
done

echo ""
echo "Done. MPI benchmark CSV: ${CSV}"
