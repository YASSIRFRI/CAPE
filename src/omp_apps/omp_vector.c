void vector(float A[], float B[], float C[], float D[], int n){
	int i, nthreads, tid;
	#pragma omp parallel shared(a,b,c,d,nthreads) private(i,tid)
    {
		tid = omp_get_thread_num();
		if (tid == 0)
		{
			nthreads =  omp_get_num_threads();
			printf("Number of threads = %d\n", nthreads);
		}
		printf("Thread %d starting...\n",tid);
		#pragma omp sections nowait
		{	  
			#pragma omp section
     		printf("Thread %d doing section 1\n",tid);
			for (i=0; i<N; i++)
			{
				C[i] = A[i] + B[i];
				printf("Thread %d: c[%d]= %f\n",tid,i,c[i]);
			}		
			#pragma omp section
			printf("Thread %d doing section 2\n",tid);
			for (i=0; i<N; i++)
			{
				D[i] = A[i] * B[i];
				printf("Thread %d: d[%d]= %f\n",tid,i,d[i]);
			}
		}  /* end of sections */    
	}  /* end of parallel section */
}


void vector2(int A[], int B[], int Y[], int Z[], int n)
{
	int i;
	#pragma omp parallel
	{
		#pragma omp for nowait
		for (i=1; i<n; i++)
			b[i] = (a[i] + a[i-1]) / 2.0;			
		#pragma omp for nowait
		for (i=0; i<m; i++)
			y[i] = z[i] * i;
	}
}
