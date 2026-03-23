#include <math.h>
#include <stdio.h>
#include "mpi.h"
#include <sys/time.h>
#include "../../include/cape.h"
void * __get_pc () { return __builtin_return_address(0); }

#define N 100
int A[N][N], B[N][N], C[N][N];

int i, j,k;
long sum;

void main(){	
	cape_declare_variable(&A, CAPE_INT, N*N, 0);
	cape_declare_variable(&B, CAPE_INT, N*N, 0);
	cape_declare_variable(&C, CAPE_INT, N*N, 0);
	cape_declare_variable(&i, CAPE_INT, 1, 0);
	cape_declare_variable(&j, CAPE_INT, 1, 0);
	cape_declare_variable(&k, CAPE_INT, 1, 0);
	cape_declare_variable(&sum, CAPE_LONG, 1, 0);
	cape_init();
	
	//load data		
	for(i=0;i<N;i++)
		for(j=0;j<N;j++){
                  A[i][j]= 1;   
                  B[i][j]= 1;                             
    }
	cape_begin(PARALLEL_FOR, 0, N);	
	ckpt_start();	
	for(i = __left__; i < __right__; i ++)
	{
		for(j = 0; j < N; j++ )
			{
				sum = 0 ;
				for ( k = 0; k < N; k++)
				{
					sum = sum + A[i][k] * B[k][j];
				}

				C[i][j] = sum;
			}			
	} 
	__pc__ = (unsigned long) __get_pc();
	cape_end(PARALLEL_FOR, FALSE); //TRUE: if exist reduction clause, FALSE: if it does not
    cape_finalize();
}
