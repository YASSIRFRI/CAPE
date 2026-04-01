#!/usr/bin/env bash
source ip_config.sh

for i in $(seq 1 $nslave_nodes)
do

eval "node=\$node$i"
ssh -t  ${uid}@${node} "sudo reboot"
	
done

ssh -t  ${uid}@${master} "sudo reboot"

