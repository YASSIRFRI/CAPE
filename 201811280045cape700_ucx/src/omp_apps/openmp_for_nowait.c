#include <math.h>
#include <omp.h>

#define N 10
#define M 5

int n, m;
float a[N], b [N], y[M], z[M];

void init_data(){
	int i;
	for (i=0; i<n; i++){
		a[i] = i * 1.1;
		z[i] = i * i * 1.0;
	}
}

void print_data(){
	int i;
	printf("\nNode %d: \n", omp_get_thread_num());
	
	for(i=0; i<n; i++ )
		printf("%.2f\t", a[i]);
	printf("\n--------------- \n");
	for(i=0; i<m; i++ )
		printf("%.2f\t", y[i]);
	printf("\n--------------- \n");
}


void main()
{
	int i;
	n= N;
	m = M;
	init_data();
	
	#pragma omp parallel
	{
		#pragma omp for nowait
		for (i=1; i<n; i++)
			b[i] = (a[i] + a[i-1]) / 2.0;
			
		#pragma omp for nowait
		for (i=0; i<m; i++)
			y[i] = z[i] * i;

	}
	
	print_data();
}

