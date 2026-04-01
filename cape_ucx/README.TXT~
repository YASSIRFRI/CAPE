Cape 7.0.0 - update 19/04/2018
______________________________________________________________________________
This version can work with Data-Sharing attribute clauses, such as: 
threadprivate, shared, private, firstprivate, lastprivate, copyin, reduction...

No longer use driver
Monitor and application is using same process
Used doublication shared variables machenism

**********************************************************************************
Folder structure:
+ bin: contains binary code of monitors and applications 
+ lib: contains cape library
+ include: contains all header files (*.h)
+ src: source code files of user lever: monitors and applications
------+ monitor: contains all monitors source code
------+ apps: contains all test application
+ ip_config.sh: IP config file. Contains the IP of all nodes
+ makefile: using to compile monitor, application....
+ and shell program to run CAPE applications

**********************************************************************************
Steps to configurate, compile and run cape program
1. Configurate the network
2. Declare IP address and Number of Slave nodes in ip_config.sh
3. Copy CAPE (bin, include folders) to all nodes
   Move to capefolder and run: $ ./deploy_cape.sh
4. run programs: 
   ./cape_test.sh <program>


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
	

		


