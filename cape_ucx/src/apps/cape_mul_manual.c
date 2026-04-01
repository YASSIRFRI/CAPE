#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "../../include/cape_dickpt.h"

#define  N  10

/* -----------------------------------
 * Global variables
 * -----------------------------------
 */

unsigned long C[N][N],A[N][N],B[N][N];
int __ckpt_flag__ = 0;
unsigned long __node__,__num_nodes__;
extern void*__data_start;

int i, j, k;

/* -----------------------------------------------------------
 * Program
 * -----------------------------------------------------------
 */
int main(int argc,char*argv[])
{
	unsigned long sum;

	__node__ = dickpt_read_node();
	dickpt_send_data_start((unsigned long)&__data_start);
	dickpt_send_num_jobs(N);

	//load data
	for(i=0;i<N;i++)
		for(j=0;j<N;j++){
                       C[i][j]=0;
                       A[i][j]= 1;
                       B[i][j]= 1;
       }


	if (__node__ == 0) dickpt_start_ckpt();
	i=0;
	for(i; i < N  ; i++)
	{
		if(__node__==0)
		{
			dickpt_generate_ckpt();
		}
	}

	if(__node__ == 0)
	{
		dickpt_generate_ckpt();
		dickpt_stop_ckpt();
	}

	//Print result
	for(i=0;i<N;i++){
		for(j=0;j<N;j++){
      		printf("%ld\t", C[i][j]);
	       }
	       printf("\n");
	 }

}
