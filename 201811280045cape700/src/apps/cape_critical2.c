#include <math.h>
#include <stdio.h>
#include "mpi.h"
#include <sys/time.h>
#include "../../include/cape.h"
void * __get_pc () { return __builtin_return_address(0); }

#define NUM_STEPS 10

double x , aux;
double sum, pi;
double step;
int i;

void main(){
	
	cape_declare_variable(&x, CAPE_DOUBLE, 1, 0);
	cape_declare_variable(&aux, CAPE_DOUBLE, 1, 0);
	cape_declare_variable(&sum, CAPE_DOUBLE, 1, 0);
	cape_declare_variable(&pi, CAPE_DOUBLE, 1, 0);
	cape_declare_variable(&step, CAPE_DOUBLE, 1, 0);
	cape_declare_variable(&i, CAPE_INT, 1, 0);	
	cape_init();
	
	x = 0;
	sum = 0.0;
	step = 1.0 / (double) NUM_STEPS;
	
	//#pragma omp parallel private(i,x,aux) shared(sum)
	cape_begin(PARALLEL, 0, 0);
	cape_set_private(&i);
	cape_set_private(&x);
	cape_set_private(&aux);
	cape_set_shared(&sum);
	ckpt_start();
	{			
		cape_begin(FOR, 0, NUM_STEPS);
		ckpt_start();
		for(i=__left__; i< __right__; i++){
			x = (i+ 0.5) * step;
			aux = 4.0/(1.0 + x*x);			
			for(__i__ = 0; __i__ < cape_get_num_nodes(); __i__++){
				if(__i__ == cape_get_node_num()){
					sum = sum + aux ; 		
					__time_stamp__ = __i__;
				}
				cape_flush();
			}
		}
		__pc__ = (unsigned long)__get_pc();
		cape_end(FOR, FALSE);
		printf("Before Parallel: Node %d: sum = %lf\n", cape_get_node_num(), sum );		
	}
	__pc__ = (unsigned long) __get_pc();
	cape_end(PARALLEL, FALSE);
	pi = step * sum;	
	printf("Node %d: sum =%lf,  pi = %lf\n", cape_get_node_num(), sum, pi );
	
	cape_finalize();
}

  
