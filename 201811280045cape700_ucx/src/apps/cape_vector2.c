#include <math.h>
#include <stdio.h>
#include <sys/time.h>
#include "../../include/cape.h"
void * __get_pc () { return __builtin_return_address(0); }

#define N 1000000
#define M 1600000

int n, m;
float a[N], b [N], y[M], z[M];

unsigned long get_ms_of_day(){
	struct timeval tv;
	char st[40];
	gettimeofday(&tv, NULL); 
	sprintf(st, "%ld", tv.tv_usec);
	st[4] = '\0';
	return tv.tv_sec * 1000 + (strtol(st, NULL, 10) + 5) / 10;
}

void init_data(){
	int i;
	for (i=0; i<n; i++){
		a[i] = i * 0.1;
		z[i] = i * i * 0.2;
	}
}

void print_data(){
	int i;
	printf("\nNode %d: \n", cape_get_node_num());
	
	for(i=0; i<n; i++ )
		printf("%.2f\t", b[i]);
	printf("\n--------------- \n");
	for(i=0; i<m; i++ )
		printf("%.2f\t", y[i]);
	printf("\n--------------- \n");
}


void main()
{
	
	unsigned int t0=0, t1=0;
	t0= get_ms_of_day() ;
	
	cape_declare_variable(&n, CAPE_INT, 1, 0);
	cape_declare_variable(&m, CAPE_INT, 1, 0);	
	cape_declare_variable(&a, CAPE_FLOAT, N, 0);
	cape_declare_variable(&b, CAPE_FLOAT, N, 0);
	cape_declare_variable(&y, CAPE_FLOAT, M, 0);
	cape_declare_variable(&z, CAPE_FLOAT, M, 0);	
	
	int i, j;
	n= N;
	m = M;
	init_data();
	
	cape_init();
	
	cape_begin(PARALLEL, 0, 0);
	ckpt_start();
	{
		//#pragma omp for nowait
		cape_begin(FOR_NOWAIT,1, n);
		for (i=__left__; i<__right__; i++)
		{
			for(j=0; j<n ; j+=20)
				a[j] = a[j] + 10.25 ;
			b[i] = (a[i] + a[i-1]) / 2.0;
		}
		cape_end(FOR_NOWAIT, FALSE);	
		
		//#pragma omp for nowait
		cape_begin(FOR_NOWAIT, 0, m);
		for (i=__left__; i<__right__; i++)
		{
			for(j=0; j<m ; j+=20)
				z[j] = z[j] * 0.025 ;
			y[i] = z[i] * i;
		}
		cape_end(FOR_NOWAIT, FALSE);
	}
	__pc__ = (unsigned long)__get_pc();
	cape_end(PARALLEL, FALSE);
	
	t1= get_ms_of_day() ;	
	if (cape_get_node_num() == 0)
		printf("%ld\t%ld\t%ld\n", N, M, t1-t0);
	
	//if (cape_get_node_num()==0)
	//print_data();
	
	cape_finalize();
}

