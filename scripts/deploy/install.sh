#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/ip_config.sh"

ssh -t  ${uid}@${master} "sudo sysctl -w kernel.randomize_va_space=0"

for i in $(seq 1 $nslave_nodes)
do

eval "node=\$node$i"
ssh -t  ${uid}@${node} "sudo sysctl -w kernel.randomize_va_space=0"
	
done
