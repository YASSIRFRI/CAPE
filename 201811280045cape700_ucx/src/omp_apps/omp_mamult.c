#include "omp.h"
#include <math.h>
#include <stdio.h>
#include <assert.h>
#define mx 10
int a[mx][mx];
int b[mx][mx];
int c[mx][mx];
void generate_matrix(int n){
	int i,j;
	for(i=0;i<n;i++)	
	{
		for(j=0;j<n;j++)
		{
			a[i][j]= 1; //rand()%100;
			b[i][j]= 1; //rand()%100;
		}
	}
}
void print_matrix(int n, int nfirstlines, int nlastlines){
	int i, j;
	for(i = 0; i< nfirstlines; i++){
		printf("\n");
		for (j= 0; j<nfirstlines; j ++)
			printf("%d\t", c[i][j]);
	}
	printf("\n....................") ;
	for(i = n - nlastlines; i< n; i++){
		printf("\n");
		for (j= n-nlastlines; j< n; j ++)
			printf("%d\t", c[i][j]);
	}
	printf("\n");		

}
void matrix_mult(int n)
{
	int i,j,k, sum;
	#pragma omp parallel for
	for(i = 0; i < n; i ++){
		for(j = 0; j < n; j++ )
		{
				sum = 0 ;
				for ( k = 0; k < n; k++)
				{
					sum += a[i][k] * b[k][j];
				}
				c[i][j] = sum;
		}	
	}	
}
int main() {
	int n=10;
	generate_matrix(n);	
	matrix_mult(n);	
	print_matrix(n, 10, 0);	
	return 0;
	
}
