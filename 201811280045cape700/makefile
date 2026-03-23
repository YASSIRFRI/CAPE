O_FOLDER=obj
L_FOLDER=lib
C_FOLDER=src
I_FOLDER=include
EXE_FOLDER=bin
DOCS_FOLDER=docs

CFLAGS=-I./$(I_FOLDER)

#Get current folder
PWD = $(shell pwd)

all:
	mpicc -w $(C_FOLDER)/monitor/cape.c $(C_FOLDER)/apps/cape_mamult.c -o $(EXE_FOLDER)/cape_mamult
	mpicc -w -D DDEBUG $(C_FOLDER)/monitor/cape.c $(C_FOLDER)/apps/cape_reduction.c -o $(EXE_FOLDER)/cape_reduction

cape_mamult: $(C_FOLDER)/monitor/cape.c	./$(I_FOLDER)/cape.h
	mpicc -w $(C_FOLDER)/monitor/cape.c $(C_FOLDER)/apps/cape_mamult.c -o $(EXE_FOLDER)/cape_mamult

cape_reduction: $(C_FOLDER)/monitor/cape.c	./$(I_FOLDER)/cape.h
	mpicc -w -D DDEBUG $(C_FOLDER)/monitor/cape.c $(C_FOLDER)/apps/cape_reduction.c -o $(EXE_FOLDER)/cape_reduction

cape_prime: $(C_FOLDER)/monitor/cape.c	./$(I_FOLDER)/cape.h
	mpicc -w -D DDEBUG $(C_FOLDER)/monitor/cape.c $(C_FOLDER)/apps/cape_prime.c -o $(EXE_FOLDER)/cape_prime
	
cape_prime_number: $(C_FOLDER)/monitor/cape.c	./$(I_FOLDER)/cape.h
	mpicc -w -D DDEBUG $(C_FOLDER)/monitor/cape.c $(C_FOLDER)/apps/cape_prime_number.c -o $(EXE_FOLDER)/cape_prime_number

cape_for_nowait: $(C_FOLDER)/monitor/cape.c	 ./$(I_FOLDER)/cape.h
	mpicc -w -D DDEBUG $(C_FOLDER)/monitor/cape.c $(C_FOLDER)/apps/cape_for_nowait.c -o $(EXE_FOLDER)/cape_for_nowait	

cape_for: $(C_FOLDER)/monitor/cape.c	 ./$(I_FOLDER)/cape.h
	mpicc -w -D DDEBUG $(C_FOLDER)/monitor/cape.c $(C_FOLDER)/apps/cape_for.c -o $(EXE_FOLDER)/cape_for	
	
cape_critical: $(C_FOLDER)/monitor/cape.c	./$(I_FOLDER)/cape.h
	mpicc -w $(C_FOLDER)/monitor/cape.c $(C_FOLDER)/apps/cape_critical.c -o $(EXE_FOLDER)/cape_critical	

cape_critical2: $(C_FOLDER)/monitor/cape.c	./$(I_FOLDER)/cape.h
	mpicc -w $(C_FOLDER)/monitor/cape.c $(C_FOLDER)/apps/cape_critical2.c -o $(EXE_FOLDER)/cape_critical2	

cape_sections_nowait: $(C_FOLDER)/monitor/cape.c ./$(I_FOLDER)/cape.h
	mpicc -w $(C_FOLDER)/monitor/cape.c $(C_FOLDER)/apps/cape_sections_nowait.c -o $(EXE_FOLDER)/cape_sections_nowait	

#Compile cmpimonitor
cmpimonitor: $(C_FOLDER)/monitor/cape.c	./$(I_FOLDER)/cape.h
	mpicc -o $(EXE_FOLDER)/cmpimonitor $(C_FOLDER)/monitor/cmpimonitor.c $(CFLAGS)

#Compile applications
APP_SRC := $(wildcard src/apps/*.c)
APP_EXE := $(addprefix $(EXE_FOLDER)/,$(notdir $(APP_SRC:.c=)))

monitor: 
	mpicc -w -c $(CFLAGS) -o $(L_FOLDER)/cape.o	$(C_FOLDER)/monitor/cape.c

apps: $(APP_EXE)

$(O_FOLDER)/%.o: $(C_FOLDER)/apps/%.c
	mpicc -w -c $(CFLAGS)	-o	$@	$<

$(EXE_FOLDER)/%: $(O_FOLDER)/%.o
	mpicc -o $@ $(L_FOLDER)/cape.o $< 
	rm $?

#Clean
cleanall:
	rm -f $(O_FOLDER)/*	$(EXE_FOLDER)/*	$(L_FOLDER)/*
