#include <stdio.h>
int abc(int x, int y) {
	int t;
	char name[16];
	t=7;
	if (x<0) return 0;
	int t1;
	t1 = x - y;
	abc(t1,t);
}
int main( )
{
	abc(10,2);
	return 0;
}
