#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/ip_config.sh"
filename=cape60_test_timer_4nodes_20170720.txt

#scp ${uid}@${master}:~/${folder}/${filename} ~/${folder}/data/master_${filename}

cp ~/${folder}/${filename} ~/${folder}/data/master_${filename}

for i in $(seq 1 $nslave_nodes)
do

eval "node=\$node$i"
scp ${uid}@${node}:~/${folder}/${filename} ~/${folder}/data/slave${i}_${filename}

done
