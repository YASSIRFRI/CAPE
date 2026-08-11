#include <omp.h>
#include <stdio.h>
#include <stdlib.h>

#define N 1000

int   i;
double a[N], sum; 


int main (int argc, char *argv[]) 
{	

	/* Some initializations */	
	for (i=0; i < N; i++)
		a[i] = 1.0;
	sum = 0.0;
	
	#pragma omp parallel for private(i) reduction(+: sum)
	for (i= 0; i < N ; i++)
			sum = sum + a[i];

	printf("Node %d:    Sum = %lf\n", omp_get_thread_num(), sum);

}
