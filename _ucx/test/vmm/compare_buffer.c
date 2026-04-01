#include <stdlib.h>
void main(){
	int ch;
	int i;

	float x1 = 0.0000, x2=0;
	
	char buff1[4069];
	char buff2[4069];
	
	char ch1 = 0, ch2 = -0;
	
	double f1 = 0.000009, f2 =0.0, f3 ;
	
	
	memcpy(buff1, &f1, sizeof(double));
	memcpy(buff2, &f2, sizeof(double));
	
	f2= 1.000;
	f1= 1.0;
	memcpy(buff1 + 8, &x2, sizeof(float));
	memcpy(buff2 + 8, &x1, sizeof(float));
	
	
	memcpy(buff1 + 12, &ch1, sizeof(float));
	memcpy(buff2 + 12, &ch2, sizeof(float));
	
	
	if (*(double *) buff1 != *(double *)buff2 ) printf("DIFF \n");
	
	if (*(unsigned int *) (buff1 + 8) != *(unsigned int *)(buff2 + 8) ) printf("DIFF Int, Float... \n");

	if (*(unsigned char *) (buff1 + 12) != *(unsigned char *) (buff2 +12 )) printf("DIFF Char \n");
	
	
	char A1[4];
	char A2[4];
	
	int r = 0;
	
	for (i = 0; i < 4; i++){
		A1[i] = i ;
		A2[i] = i ;
	}
	
	for(i =0; i < sizeof(double); i ++){
		r = buff1[i] ^ buff2[i];
		if (r != 0 ) break ;	
	}
	
	
	f3 = *(double *) buff1;
	
	printf("%lf ^ %lf = %d  - f3 = %lf\n", f1, f2, r, f3);
	
	printf("Address of ch and i: %lx - %lx \n", &x1, & x2 );
}
