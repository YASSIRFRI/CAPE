#include <stdio.h>

char *bp;
int A[10000];

int
main (void)
{

  size_t size;
  FILE *stream;
  int i;
  
  stream = open_memstream (&bp, &size);
  
   for (i = 0; i< 2000000; i++){
		fwrite(A, sizeof(int), 1000, stream);
		fflush(stream);
   }  
 
  fclose (stream);
  printf ("size = %zu\n", size/(1024*1024));

  return 0;
}
