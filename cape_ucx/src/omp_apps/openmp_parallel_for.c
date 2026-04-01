#include <omp.h>
#include <stdio.h>
#include <stdlib.h>

#define N 1000
int A[N][N], B[N][N], C[N][N];

void main(){
	int i, j,k;
	long sum;
	
	for(i=0;i<N;i++)
		for(j=0;j<N;j++){
                  A[i][j]= 1;   
                  B[i][j]= 1;                             
    }	
	
	#pragma omp parallel for		
	for(i = 0; i < N ; i ++)
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
}
