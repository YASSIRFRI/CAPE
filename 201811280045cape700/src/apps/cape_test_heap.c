#include <math.h>
#include <stdio.h>
#include "mpi.h"
#include <sys/time.h>
#include "../../include/cape.h"
#define mx 200

unsigned long get_ms_of_day(){
	struct timeval tv;
	char st[40];
	gettimeofday(&tv, NULL); 
	sprintf(st, "%ld", tv.tv_usec);
	st[4] = '\0';
	return tv.tv_sec * 1000 + (strtol(st, NULL, 10) + 5) / 10;
}

void * __get_pc () { return __builtin_return_address(0); }

int a[mx][mx];
int b[mx][mx];
int c[mx][mx];

int test;

void generate_matrix(int n){__enter_func();	
	cape_declare_variable(&n, CAPE_INT, 1, 0);
	int i,j;
	cape_declare_variable(&i, CAPE_INT, 1, 0);
	cape_declare_variable(&j, CAPE_INT, 1, 0);
	
	int *p1;
	cape_declare_variable(&p1, CAPE_INT, 1, 1);
	
	p1 = malloc(100* sizeof(int));
	cape_allocate_memory(&p1, p1, 100*sizeof(int));
	
	
	double *p2;
	cape_declare_variable(&p2, CAPE_DOUBLE, 1, 1);

	p2= calloc(10, sizeof(double));
	cape_allocate_memory(&p2, p2, 10*sizeof(double));
	
	printf("\nNode %d - In Generate function : address of p1 = %p - p2 = %p ", cape_get_node_num(), p1, p2) ;
	CAPE_DEBUG();
	
	for(i=0;i<n;i++)	
	{
		for(j=0;j<n;j++)
		{
			a[i][j]= rand()%100;
			b[i][j]= rand()%100;
		}
	}
__exit_func();}

void print_matrix(int n, int nfirstlines, int nlastlines){__enter_func();

	cape_declare_variable(&n, CAPE_INT, 1, 0);
	cape_declare_variable(&nfirstlines, CAPE_INT, 1, 0);
	cape_declare_variable(&nlastlines, CAPE_INT, 1, 0);
	int i, j;
	cape_declare_variable(&i, CAPE_INT, 1, 0);
	cape_declare_variable(&j, CAPE_INT, 1, 0);
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

__exit_func();}

void matrix_mult_2(int n){__enter_func();	
	cape_declare_variable(&n, CAPE_INT, 1, 0);
	int i,j,k;
	cape_declare_variable(&i, CAPE_INT, 1, 0);
	cape_declare_variable(&j, CAPE_INT, 1, 0);
	cape_declare_variable(&k, CAPE_INT, 1, 0);	
		
	cape_begin(PARALLEL_FOR, 0, n);
	cape_set_private(&a);
	cape_set_private(&b);
	ckpt_start_2();
	for(i = 0; i < n; i ++){
		for(j = 0; j < n; j++ )
		{
				int sum = 0 ;
				cape_declare_variable(&sum, CAPE_INT, 1, 0);
				for ( k = 0; k < n; k++)
				{
					sum += a[i][k] * b[k][j];
				}
				c[i][j] = sum;
		}	
	}
	__pc__ = (unsigned long) __get_pc();
	cape_end(PARALLEL_FOR, FALSE);	
__exit_func();}

void matrix_mult(int n){__enter_func();	
	//cape_declare_variable(&n, CAPE_INT, 1, 0);
	int i,j,k;
	//cape_declare_variable(&i, CAPE_INT, 1, 0);
	//cape_declare_variable(&j, CAPE_INT, 1, 0);
	//cape_declare_variable(&k, CAPE_INT, 1, 0);	
	
	int *p11;
	p11 = malloc(100* sizeof(int));
	
	double *p22;
	p22= malloc(10 * sizeof(double));	
	printf("\nNode %d - In matrix_mult function : address of p1 = %p - p2 = %p ",cape_get_node_num(), p11, p22) ;
			
	cape_begin(PARALLEL_FOR, 0, n);
	cape_set_private(&a);
	cape_set_private(&b);
	ckpt_start();
	for(i = __left__; i < __right__; i ++){
		for(j = 0; j < n; j++ )
		{
				int sum = 0 ;
				//cape_declare_variable(&sum, CAPE_INT, 1, 0);
				for ( k = 0; k < n; k++)
				{
					sum += a[i][k] * b[k][j];
				}
				c[i][j] = sum;
		}	
	}
	__pc__ = (unsigned long) __get_pc();
	cape_end(PARALLEL_FOR, FALSE);
__exit_func();}


int main() {
	unsigned long t[10], t1[10], i;
	for(i=0; i<10; i ++) { t[i] = 0; t1[i] = 0 ;}
	
	cape_declare_variable(&a, CAPE_INT, mx*mx, 0);
	cape_declare_variable(&b, CAPE_INT, mx*mx, 0);
	cape_declare_variable(&c, CAPE_INT, mx*mx, 0);
	
	cape_init();
	
	int n=200;
	cape_declare_variable(&n, CAPE_INT, 1, 0);
	generate_matrix(n);		
	
	t[0] = get_ms_of_day(); 
	matrix_mult(n);	
	t[1] = get_ms_of_day(); 
	
	//t1[0] = get_ms_of_day(); 
	//matrix_mult_2(n);	
	//t1[1] = get_ms_of_day(); 
	//sleep(10);
	//print_matrix(n, 10, 10);	
	
	CAPE_DEBUG();
	
	printf(" Execution time: %ld ms \n", t[1]-t[0]);

	//printf(" SAVED in disk  : %ld ms \n", t1[1] - t1[0]);
	
	cape_finalize();
	return 0;	
}
