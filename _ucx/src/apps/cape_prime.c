/**
  Purpose: PRIME_NUMBER returns the number of primes between 1 and N.
  Parameters:
    Input, int N, the maximum number to check.
    Output, int PRIME_NUMBER, the number of prime numbers up to N.
                N  PRIME_NUMBER

                1           0
               10           4
              100          25
            1,000         168
           10,000       1,229
          100,000       9,592
        1,000,000      78,498
       10,000,000     664,579
      100,000,000   5,761,455
    1,000,000,000  50,847,534
    
OpenMP source code:  
  int i;
  int j;
  int prime;
  int total = 0;
# pragma omp parallel for shared ( n ) private ( j, prime ) reduction  ( + : total )
  for ( i = 2; i <= n; i++ ){
    prime = 1;
    for ( j = 2; j < i; j++ ){
      if ( i % j == 0 ){
        prime = 0;
        break;
      }
    }
    total = total + prime;
  }
  return total;
}
*/
#include <math.h>
#include <stdio.h>
#include <sys/time.h>
#include "../../include/cape.h"
void * __get_pc () { return __builtin_return_address(0); }
#define N 1000000

unsigned long get_ms_of_day(){
	struct timeval tv;
	char st[40];
	gettimeofday(&tv, NULL); 
	sprintf(st, "%ld", tv.tv_usec);
	st[4] = '\0';
	return tv.tv_sec * 1000 + (strtol(st, NULL, 10) + 5) / 10;
}

unsigned int n;
unsigned int i;
unsigned int j;
unsigned int prime;
unsigned int total;

void main()
{
	unsigned int t0=0, t1=0;
	t0= get_ms_of_day() ;
	
	cape_declare_variable(&n, CAPE_INT, 1, 0);
	cape_declare_variable(&i, CAPE_INT, 1, 0);
	cape_declare_variable(&j, CAPE_INT, 1, 0);
	cape_declare_variable(&prime, CAPE_INT, 1, 0);
	cape_declare_variable(&total, CAPE_INT, 1, 0);
		
	n = N ;
	cape_init();
	
	//# pragma omp parallel for shared ( n ) private ( j, prime ) reduction  ( + : total )	
	cape_begin(PARALLEL_FOR, 2, n);
	cape_set_shared(&n);
	cape_set_private(&j);
	cape_set_private(&prime);
	cape_set_reduction(&total, CAPE_SUM);	
	ckpt_start();
		
	for ( i = __left__; i <= __right__; i++ ){
		prime = 1;
		for ( j = 2; j < i; j++ ){
			if ( i % j == 0 ){
				prime = 0;
				break;
			}
		}
		total = total + prime;
	}	
	//printf("\nBefore synchronized: Node %ld: total Primer Number = %d \n", cape_get_node_num(), total);
	
	__pc__ = (unsigned long)__get_pc();
	cape_end(PARALLEL_FOR,TRUE);
	
	//printf("\nAfter synchronized: Node %ld: total Primer Number = %d \n", cape_get_node_num(), total);
	
	t1= get_ms_of_day() ;	
	if (cape_get_node_num() == 0)
		printf("%ld\t%ld\t%ld\n", N, total, t1-t0);
	
	cape_finalize();
}
