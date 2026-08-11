#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
source "${SCRIPT_DIR}/ip_config.sh"
cd "${PROJECT_DIR}"

PACKAGE_ITEMS=(bin include src scripts transform makefile README.md)

#ssh ${uid}@${master} << EOF
#  if [ ! -d ${folder} ]; then
#		mkdir ~/${folder}
#  fi
#EOF
#scp -r bin include scripts makefile README.md ${uid}@${master}:~/${folder}/

for i in $(seq 1 $nslave_nodes)
do

eval "node=\$node$i"
ssh ${uid}@${node} << EOF
	if [ ! -d ${folder} ]; then
			mkdir ~/${folder}
	fi

EOF

scp -r "${PACKAGE_ITEMS[@]}" ${uid}@${node}:~/${folder}/

done
