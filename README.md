Cape 7.0.0 - update 19/04/2018
______________________________________________________________________________
This version can work with Data-Sharing attribute clauses, such as: 
threadprivate, shared, private, firstprivate, lastprivate, copyin, reduction...

No longer use driver
Monitor and application is using same process
Used doublication shared variables machenism

**********************************************************************************
Folder structure:
+ bin: contains binary code of monitors and applications (created by make)
+ lib: contains cape library
+ include: contains all header files (*.h)
+ src: source code files of user lever: monitors and applications
------+ monitor: contains all monitors source code
------+ apps: contains all test application
+ tests: standalone / vmm test programs
+ scripts/deploy: ip_config.sh, deploy_cape.sh and other cluster provisioning scripts
+ scripts/bench: SLURM job scripts and benchmark/verification runners
+ makefile: using to compile monitor, application....
+ transform: TXL-based OpenMP -> CAPE/DICKPT source transformers (dompcc, txl)
+ docs: usage docs, benchmark data and figures
+ archive/legacy: superseded code kept for reference only

**********************************************************************************
Steps to configurate, compile and run cape program
1. Configurate the network
2. Declare IP address and Number of Slave nodes in scripts/deploy/ip_config.sh
3. Copy CAPE (bin, include folders) to all nodes
   From the repo root, run: $ ./scripts/deploy/deploy_cape.sh
4. run programs: 
   ./scripts/bench/<script>.sh <program>


**********************************************************************************
For developer:
1. CAPE Monitor:
	Source code: src/monitor/cape.c
	Compile monitor: make monitor
2. CAPE Apps
	Source code: src/apps/*.c
	Developer can write new apps
3. Steps to compile and execute new apps
3.1 Write new apps and put them in src/apps/
3.2 Compile: ./make apps
3.4 Deploy apps to all nodes: ./deploy_cape.sh
3.4 run ./cape_test.sh <app_name>
	

		


