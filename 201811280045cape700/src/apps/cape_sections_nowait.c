#include <math.h>
#include <stdio.h>
#include "mpi.h"
#include <sys/time.h>
#include "../../include/cape.h"
void * __get_pc () { return __builtin_return_address(0); }

#define N     100

int i, nthreads, tid;
float a[N], b[N], c[N], d[N];

void print_result(){__enter_func();
   
   printf("Node %d: d[%d] = ", cape_get_node_num(), N);
   for (i=0; i<N; i++)  {  
        printf("%f\t",d[i]);   }
   printf("\n");
   
__exit_func();}

int main (int argc, char *argv[]) 
{	
	cape_declare_variable(&i, CAPE_INT, 1, 0);
	cape_declare_variable(&nthreads, CAPE_INT, 1, 0);	
	cape_declare_variable(&tid, CAPE_INT, 1, 0);
	cape_declare_variable(&a, CAPE_FLOAT, N, 0);
	cape_declare_variable(&b, CAPE_FLOAT, N, 0);
	cape_declare_variable(&c, CAPE_FLOAT, N, 0);
	cape_declare_variable(&d, CAPE_FLOAT, N, 0);
	cape_init();
	

/* Some initializations */
for (i=0; i<N; i++) {
  a[i] = i * 1.5;
  b[i] = i + 22.35;
  c[i] = d[i] = 0.0;
  }

//#pragma omp parallel shared(a,b,c,d,nthreads) private(i,tid)
  cape_begin(PARALLEL, 0, 0);
  cape_set_private(&i);
  cape_set_private(&tid);
  cape_set_shared(&a);
  cape_set_shared(&b);
  cape_set_shared(&c);
  cape_set_shared(&d);
  cape_set_shared(&nthreads);
  ckpt_start();
  {
	tid = cape_get_node_num();
	if (tid == 0)
    {
		nthreads = cape_get_num_nodes();
		printf("Number of threads = %d\n", nthreads);
    }
	printf("Thread %d starting...\n",tid);

   //#pragma omp sections nowait
   cape_begin(SECTIONS_NOWAIT, 0, 0);  
   {
	  
	  //#pragma omp section
      if(cape_section())      
      {
		printf("Thread %d doing section 1\n",tid);
		for (i=0; i<N; i++)
        {
			c[i] = a[i] + b[i];
			printf("Thread %d: c[%d]= %f\n",tid,i,c[i]);
        }
      }
     //#pragma omp section
      if(cape_section()) 
      {
      printf("Thread %d doing section 2\n",tid);
      for (i=0; i<N; i++)
        {
        d[i] = a[i] * b[i];
        printf("Thread %d: d[%d]= %f\n",tid,i,d[i]);
        }
      }
    }  /* end of sections */
    cape_end(SECTIONS_NOWAIT, FALSE);
    printf("Thread %d done.\n",tid); 

  }  /* end of parallel section */
  __pc__ = (unsigned long)__get_pc();
  cape_end(PARALLEL, FALSE);
   
   
  print_result();
  
  cape_finalize();
}
