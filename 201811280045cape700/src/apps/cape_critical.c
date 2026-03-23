#include <math.h>
#include <stdio.h>
#include "mpi.h"
#include <sys/time.h>
#include "../../include/cape.h"
void * __get_pc () { return __builtin_return_address(0); }

#define NUM_STEPS 10

int x;

main(int argc, char *argv[]) {
 int i;
 cape_declare_variable(&x, CAPE_INT, 1, 0);
 cape_init();
 x = 0;
  //#pragma omp parallel shared(x) 
 cape_begin(PARALLEL, 0, 0); 
 ckpt_start(); 
 {	
	for (i = 0; i< NUM_STEPS; i++){
		for(__i__ = 0; __i__ < cape_get_num_nodes(); __i__++){
			if(__i__ == cape_get_node_num()){
				x = x + 1; 		
				__time_stamp__ = __i__;
			}
			printf("Before: Node %d: x = %d \n", cape_get_node_num(), x);			
			cape_flush();
		}
	}
  }  /* end of parallel region */
  __pc__ = (unsigned int)__get_pc();
  cape_end(PARALLEL, FALSE);

 printf("Node %d: x = %d \n", cape_get_node_num(), x);
 cape_finalize();

 }
  
