#include <stdio.h>
void func(int a, int b)
{
	int c =0;
	c = a +b ;
	printf("in func: &a = %lx - &b = %lx - &c = %lx \n", (long)&a, (long)&b, (long)&c );	
	
}

void main(){
	int c;
	
	func(10, 20);
	
	printf("in main: &c = %lx \n", (long)&c );
	
}
