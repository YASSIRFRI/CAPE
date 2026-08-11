#include <math.h>
#include <stdio.h>
#include <sys/time.h>
#include "../../include/cape.h"

#define NUM_STEPS 100000000
void * __get_pc () { return __builtin_return_address(0); }

unsigned long get_ms_of_day(){
	struct timeval tv;
	char st[40];
	gettimeofday(&tv, NULL); 
	sprintf(st, "%ld", tv.tv_usec);
	st[4] = '\0';
	return tv.tv_sec * 1000 + (strtol(st, NULL, 10) + 5) / 10;
}

double x , aux;
double sum, pi;
double step;
unsigned int i;

void main(){
	unsigned int t0=0, t1=0;
	t0= get_ms_of_day() ;
	
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
		cape_set_reduction(&sum, CAPE_SUM);
		ckpt_start();
		for(i=__left__; i< __right__; i++){
			x = (i+ 0.5) * step;
			aux = 4.0/(1.0 + x*x);
			sum += aux;
		}
		__pc__ = (unsigned long) __get_pc();
		cape_end(FOR, TRUE);
	}
	__pc__ = (unsigned long) __get_pc();
	cape_end(PARALLEL, FALSE);	
	pi = step * sum;	
	
	t1= get_ms_of_day() ;
	
	//printf("Node %d: sum =%lf,  pi = %lf\n", cape_get_node_num(), sum, pi );
	if (cape_get_node_num() == 0)
		printf("%ld\t%lf\t%ld\n", NUM_STEPS, pi, t1-t0);
	
	cape_finalize();
}

  
