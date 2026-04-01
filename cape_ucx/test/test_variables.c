#include <stdio.h>

void main()
{	
	char ch1;
	int n;
	float x;
	double y, y2;
	char ch2;
	
	ch1 = 'A';
	n = 100;
	x = 1.234;
	y = 0.1234;
	y2 = 2.56789;
	ch2 = 'B';	
	char ch3;
	char ch4;
	char ch5;
	char ch6;
	
	long a_ch1 = (long)(&ch1)  & ~(4 -1);
	long a_ch2 = (long)(&ch2)  & ~(4 -1);
	long a_ch3 = (long)(&ch3)  & ~(4 -1);
	long a_ch4 = (long)(&ch4)  & ~(4 -1);
	long a_ch5 = (long)(&ch5)  & ~(4 -1);
	long a_ch6 = (long)(&ch6)  & ~(4 -1);
	
	
	printf("ch1: (Ox%lx - Ox%lx )  \n" , (unsigned long) &ch1, a_ch1);
	printf("ch2: (Ox%lx - Ox%lx )  \n" , (unsigned long)&ch2, a_ch2);	
	printf("ch3: (Ox%lx - Ox%lx)  \n" , (unsigned long) &ch3, a_ch3);
	printf("ch4: (Ox%lx - Ox%lx )  \n" , (unsigned long) &ch4, a_ch4);
	printf("ch5: (Ox%lx - Ox%lx)  \n" , (unsigned long) &ch5, a_ch5);
	printf("ch6: (Ox%lx - Ox%lx)  \n" , (unsigned long) &ch6, a_ch6);	
	printf("  n: (Ox%lx - %10d)  \n" , (unsigned long) &n, n );
	printf("  x: (Ox%lx - %10f)  \n" , (unsigned long) &x, x );
	printf("  y: (Ox%lx - %10lf) \n" , (unsigned long) &y, y);
	printf(" y2: (Ox%lx - %10lf) \n" , (unsigned long) &y2, y2 );
	
	
	
}

/* Result: 
ch1: (Oxbfe5c136 -          A)  
  n: (Oxbfe5c138 -        100)  
  x: (Oxbfe5c13c -   1.234000)  
  y: (Oxbfe5c140 -   0.123400) 
 y2: (Oxbfe5c148 -   2.567890) 
ch2: (Oxbfe5c137 -          B)  
 */
