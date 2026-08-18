#!/bin/bash
# Compare CAPE (cape_incr_bitmap multithreaded monitor) against plain OpenMP on
# heat3d, at matching thread counts.
#
#   scripts/bench/compare_heat3d_cape_vs_omp.sh <cape.csv> <omp.csv>
#
# Inputs are the CSVs produced by bench_job_heat3d_dickpt.sh and
# bench_job_heat3d_omp.sh (same 16-column layout).
#
# What the numbers mean
# --------------------
# omp runs the WHOLE cube on one node; each CAPE rank runs 1/nodes of the cube
# but replicates the full state every iteration via the checkpoint allreduce.
# So the honest comparisons are:
#   compute_ratio  = cape(sweep+wb) / omp(sweep+wb) * nodes
#                    ~1.0 means the clone(2) threads compute as fast as OpenMP;
#                    >1 means CAPE's compute itself is slower (cache/uffd/WP
#                    faults on the tracked cube).
#   ckpt_frac      = cape ckpt_ms / cape app_ms — the share of runtime spent in
#                    the DICKPT path. This is the overhead to attack.
#   e2e_speedup    = omp app_ms / cape app_ms — the end-to-end win (or loss)
#                    from spreading the job over `nodes` nodes.

set -euo pipefail

if [ $# -lt 2 ]; then
    echo "usage: $0 <cape_csv> <omp_csv>" >&2
    exit 1
fi
CAPE_CSV="$1"
OMP_CSV="$2"
[ -r "${CAPE_CSV}" ] || { echo "cannot read ${CAPE_CSV}" >&2; exit 1; }
[ -r "${OMP_CSV}" ]  || { echo "cannot read ${OMP_CSV}" >&2; exit 1; }

awk -F, '
    # columns: impl,app,n,d,nodes,threads,rep,app_ms,sweep,wb,ckpt,start,gen,allred,stop,job
    FNR==1 { next }
    FILENAME==ARGV[1] {
        t=$6; cn[t]++; cnodes[t]=$5; cn_dim=$3; cd=$4;
        capp[t]+=$8; csw[t]+=$9; cwb[t]+=$10; cck[t]+=$11; cgen[t]+=$13; car[t]+=$14;
        next
    }
    {
        t=$6; on[t]++; oapp[t]+=$8; osw[t]+=$9; owb[t]+=$10;
    }
    END {
        printf "heat3d  n=%s iters=%s\n\n", cn_dim, cd;
        printf "%-8s %-6s %-10s %-10s %-10s %-9s %-11s %-13s %-11s\n",
               "threads","nodes","omp_ms","cape_ms","cape_ckpt","ckpt_%","e2e_speedup","compute_ratio","gen_ms";
        m = asorti(cn, idx);
        for (i=1;i<=m;i++) {
            t = idx[i];
            if (!(t in on)) continue;
            c=cn[t]; o=on[t];
            cA=capp[t]/c; oA=oapp[t]/o;
            cC=(csw[t]+cwb[t])/c; oC=(osw[t]+owb[t])/o;
            ck=cck[t]/c;
            nn=cnodes[t]+0;
            printf "%-8s %-6d %-10.0f %-10.0f %-10.0f %-9.1f %-11.3f %-13.3f %-11.0f\n",
                   t, nn, oA, cA, ck,
                   (cA>0)? 100.0*ck/cA : 0,
                   (cA>0)? oA/cA : 0,
                   (oC>0)? (cC*nn)/oC : 0,
                   cgen[t]/c;
        }
        print "";
        print "compute_ratio ~1.0 => CAPE computes at OpenMP speed; >1 => the tracked";
        print "region itself is slower (write-protect faults / cache effects).";
        print "ckpt_% is the DICKPT overhead share; gen_ms is its parallelized part.";
    }' "${CAPE_CSV}" "${OMP_CSV}"
