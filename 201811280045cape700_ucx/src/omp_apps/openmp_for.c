#include <math.h>
#include <omp.h>
#define NUM_STEPS 1000

double x , aux;
double sum, pi;
double step;
int i;

void main(){
	x = 0;
	sum = 0.0;
	step = 1.0 / (double) NUM_STEPS;
	
	#pragma omp parallel private(i,x,aux) shared(sum)
	{
		#pragma omp for reduction(+ : sum)
		for(i=0; i< NUM_STEPS; i++){
			x = (i+ 0.5) * step;
			aux = 4.0/(1.0 + x*x);
			sum += aux;
		}		
	}	
	pi = step * sum;	
	printf("Node %d: pi = %lf\n", omp_get_thread_num(), pi );
}

  
