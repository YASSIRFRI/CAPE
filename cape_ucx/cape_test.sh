#!/usr/bin/env bash
prog="$1"
prog2="$2"
prog3="$3"
prog4="$4"
max1="$5"
max2="$6"
max3="$7"
max4="$8"
source ip_config.sh

for i in `seq 1 $max1`;
do
	date '+%X'	
	echo Program $prog is executing ... $i of $max1
	mpirun --host ${master},${node1},${node2},${node3} ~/${folder}/bin/${prog} >> capenew_time_4nodes_${prog}.txt
	#mpirun --host ${master},${node1},${node2},${node3},${node4},${node5},${node6},${node7} ~/${folder}/bin/${prog} >> capenew_time_8nodes_${prog}.txt
	#mpirun --host ${master},${node1},${node2},${node3},${node4},${node5},${node6},${node7},${node8},${node9},${node10},${node11},${node12},${node13},${node14},${node15} ~/${folder}/bin/${prog} >> capenew_time_16nodes_${prog}.txt
done 

echo The execution of $prog is finished.

for i in `seq 1 $max2`;
do
	date '+%X'	
	echo Program $prog2 is executing ... $i of $max2
	mpirun --host ${master},${node1},${node2},${node3} ~/${folder}/bin/${prog2} >> capenew_time_4nodes_${prog2}.txt
	#mpirun --host ${master},${node1},${node2},${node3},${node4},${node5},${node6},${node7} ~/${folder}/bin/${prog2} >> capenew_time_8nodes_${prog2}.txt
	#mpirun --host ${master},${node1},${node2},${node3},${node4},${node5},${node6},${node7},${node8},${node9},${node10},${node11},${node12},${node13},${node14},${node15} ~/${folder}/bin/${prog2} >> capenew_time_16nodes_${prog2}.txt
done 

echo The execution of $prog2 is finished.

for i in `seq 1 $max3`;
do
	date '+%X'	
	echo Program $prog3 is executing ... $i of $max3
	mpirun --host ${master},${node1},${node2},${node3} ~/${folder}/bin/${prog3} >> capenew_time_4nodes_${prog3}.txt
	#mpirun --host ${master},${node1},${node2},${node3},${node4},${node5},${node6},${node7} ~/${folder}/bin/${prog3} >> capenew_time_8nodes_${prog3}.txt
	# mpirun --host ${master},${node1},${node2},${node3},${node4},${node5},${node6},${node7},${node8},${node9},${node10},${node11},${node12},${node13},${node14},${node15} ~/${folder}/bin/${prog3} >> capenew_time_16nodes_${prog3}.txt
done 

echo The execution of $prog3 is finished.

for i in `seq 1 $max4`;
do
	date '+%X'	
	echo Program $prog4 is executing ... $i of $max4
	mpirun --host ${master},${node1},${node2},${node3} ~/${folder}/bin/${prog4} >> capenew_time_4nodes_${prog4}.txt
	#mpirun --host ${master},${node1},${node2},${node3},${node4},${node5},${node6},${node7} ~/${folder}/bin/${prog4} >> capenew_time_8nodes_${prog4}.txt
	# mpirun --host ${master},${node1},${node2},${node3},${node4},${node5},${node6},${node7},${node8},${node9},${node10},${node11},${node12},${node13},${node14},${node15} ~/${folder}/bin/${prog4} >> capenew_time_16nodes_${prog4}.txt
done 

echo The execution of $prog4 is finished.

echo The execution of all programs are finished.
