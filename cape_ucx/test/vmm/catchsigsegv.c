#include <stdlib.h>
#include <sys/mman.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>   
#include <signal.h>
#include <malloc.h>
#include <errno.h>
#include "chardev9.h"


#define N 100
#define A_PAGE_SIZE 4096
#define handle_error(msg) \
           do { perror(msg); exit(EXIT_FAILURE); } while (0)
           
         
int driver_file;
int myid;
int A[N][N];	

void APP_Init(){
	
	myid= getpid();
	
	//Open driver	
	driver_file = open("/dev/chardev90", O_RDONLY);
	if (driver_file < 0) {
		printf ("Can't open device file: /dev/chardev90\n");
		exit(1);
	}
}
void APP_Finalize(){
	close(driver_file);
}

 int ioctl_set_write_protect(unsigned int pid, unsigned long dst){
	int ret_val;
	data_change_t data_change;
	data_change.pid = pid;
	data_change.src_addr = 0;
	data_change.dst_addr = dst;
	data_change.len = 0;
	ret_val = ioctl(driver_file, IOCTL_SET_WRITE_PROTECT, &data_change);	
	return ret_val;
}

int ioctl_clear_write_protect(unsigned int pid, unsigned long dst){
	int ret_val;
	data_change_t data_change;

	data_change.pid = pid;
	data_change.src_addr = 0;
	data_change.dst_addr = dst;
	data_change.len = 0;

	//call the driver to clear the write protection
	ret_val = ioctl(driver_file, IOCTL_CLEAR_WRITE_PROTECT, &data_change);
	if(ret_val != 0){		
		dprintf("Monitor: Error on calling ioctl_clear_write_protect: %d, at %ld\n", ret_val, dst);		
		return 1;
	}		

	return 0;
}

int lock_memory(unsigned int pid,  unsigned long start_addr, unsigned long len ){
	int ret_val;
	int i, npages = 1;
	long aligned_addr = start_addr & ~(A_PAGE_SIZE -1);
	
	if (len % A_PAGE_SIZE == 0 ){
		npages = len / A_PAGE_SIZE ;
	}else{	
		npages = len / A_PAGE_SIZE  + 1 ;
	}	
	for( i = 1; i <= npages; i++ ){
		printf("Process id %d - Lock page address %lx \n", pid, aligned_addr);
		ret_val = ioctl_set_write_protect(pid, aligned_addr);
//		 if (mprotect(aligned_addr, A_PAGE_SIZE, PROT_READ) == -1)
//               handle_error("mprotect");
		
		aligned_addr += A_PAGE_SIZE ;	
	}	
	return ret_val;
}

int unlock_memory(unsigned int pid,  unsigned long start_addr, unsigned long len ){
	int ret_val;
	int i, npages = 1;
	long aligned_addr = start_addr & ~(A_PAGE_SIZE -1);
	
	if (len % A_PAGE_SIZE == 0 ){
		npages = len / A_PAGE_SIZE ;
	}else{	
		npages = len / A_PAGE_SIZE  + 1 ;
	}	
	for( i = 1; i <= npages; i++ ){
		ret_val = ioctl_clear_write_protect(pid, aligned_addr);
		printf("Process id %d - Unlock page address %lx \n", pid, aligned_addr);
		// if (mprotect(aligned_addr, A_PAGE_SIZE, PROT_WRITE) == -1)
        //       handle_error("mprotect");
		aligned_addr += A_PAGE_SIZE ;	
	}
	return ret_val;	
}




static void handler(int sig, siginfo_t *si, void *unused)
{
   int ret_val = 0;
   
   printf("pid %d - Got SIGSEGV at address: 0x%lx\n", getpid(),  (long) si->si_addr);
  
   unlock_memory(myid, si->si_addr, A_PAGE_SIZE );
   
  // exit(1);

 }

void main(){
	
     APP_Init();
	 int i,j,k;              
       
     struct sigaction sa;
     sa.sa_flags = SA_SIGINFO;
     sigemptyset(&sa.sa_mask);
     sa.sa_sigaction = handler;
     if (sigaction(SIGSEGV, &sa, NULL) == -1)
               handle_error("sigaction");
               
           
          for(i = 1; i < N ; i++ )
				for (j = 0; j < N ; j++){
					A[i][j] = 0 ;
		  }
        
                      

         lock_memory( myid, ((long)&A[0][0]), (N-1) * (N-1) * sizeof(int) );
          
         for(i = 0; i < N ; i++ )
				for (j = 0; j < N ; j++){
					A[i][j] = i+ 1 ;
		 }

	
		  unlock_memory(myid, ((long)&A[0][0]), (N-1) * (N-1) * sizeof(int) );

           printf("Loop completed\n");     
	
	
			for(i = 1; i < 10 ; i++ ){			
				for (j = 0; j < 10 ; j++){
					printf("%d \t", A[i][j]);					
				}
				printf("\n");
			}
	
			 printf("Hello\n");     
	
	APP_Finalize();

}
