#include <math.h>
#include <stdio.h>
#include "mpi.h"
#include <sys/time.h>
#include "../../include/cape.h"
void * __get_pc () { return __builtin_return_address(0); }
#define N 100

int i;
int j;
int prime;
int total;

int main ( int argc, char *argv[] );
int prime_number ( int n );

/******************************************************************************/

int main ( int argc, char *argv[] )

/******************************************************************************/
{
  cape_declare_variable(&i, CAPE_INT, 1, 0);
  cape_declare_variable(&j, CAPE_INT, 1, 0);
  cape_declare_variable(&prime, CAPE_INT, 1, 0);
  cape_declare_variable(&total, CAPE_INT, 1, 0);  
  int ii;
  cape_init();    
  for (ii = 1; ii <=N ; ii++){     
     printf("%10d %10d \n", i, prime_number(i) );
  }  
  cape_finalize();
  return 0;
}
/******************************************************************************/

/******************************************************************************/

int prime_number ( int n )
{__enter_func();
  total = 0;
//# pragma omp parallel for private ( j, prime ) reduction  ( + : total )
  cape_begin(PARALLEL_FOR, 2, n);
  cape_set_private(&j);
  cape_set_private(&prime);
  cape_set_reduction(&total, CAPE_SUM);
  ckpt_start();
  for ( i = __left__; i <= __right__; i++ )
  {
    prime = 1;
    for ( j = 2; j < i; j++ )
    {
      if ( i % j == 0 )
      {
        prime = 0;
        break;
      }
    }
    total = total + prime;
  }
  __pc__ = (unsigned long)__get_pc();
  cape_end(PARALLEL_FOR, TRUE);
  return total;
__exit_func();}
