#include <stdio.h>

int f_index = 0, activation_tree[100];

inline cape_enter_func(){
	register long bsp asm("ebp");
	register long esp asm("esp");
	long cEsp = esp;	
	printf("\n Function enter_func: esp = Ox%lx ", cEsp) ;
	printf("\n Function enter_func: bsp = Ox%lx ", bsp) ;
	f_index++;
	activation_tree[f_index] = bsp;
}
inline cape_exit_func(){
	register long bsp asm("ebp");
	register long esp asm("esp");
	long cEsp = esp;	
	printf("\n Function exit_func: esp = Ox%lx ", cEsp) ;
	printf("\n Function exit_func: bsp = Ox%lx ", bsp) ;
	f_index--;
}

int f2(int a, int b, int c){
	int d;
	printf("\n In f2: a = %p, b = %p, c = %p,", &a, &b, &c);
	return 0;
}

int f3(int a, int b, int c){
	int d;
	printf("\n In f3: a = %p, b = %p, c = %p,", &a, &b, &c);
	return 0;
}

int f1(int a, int b, int b1, int b2){
	//cape_enter_func();
	int c ;
	
	printf("\n a = %p, b = %p, b1 = %p, b2 = %p, c = %p", &a, &b, &b1, &b2, &c);
	
	register long bsp asm("ebp");
	register long esp asm("esp");
	long cEsp = esp;
	
	printf("\nValue before:  a = %d, b = %d, b1 = %d, b2 = %d, c = %d", a, b, b1, b2, c);	
	
	b = 100;
	a = 200;
	
	printf("\nValue after:  a = %d, b = %d, b1 = %d, b2 = %d, c = %d", a, b, b1, b2, c);
	
	printf("\n Function f1: esp = Ox%lx ", cEsp);
	printf("\n Function f1: bsp = Ox%lx ", bsp);
	

	//cape_exit_func();
	return 0;
}

void main(){
	
	//cape_enter_func();
		
	register long bsp asm("ebp");
	register long esp asm("esp");
	long cEsp = esp;	
	printf("\n Function main: esp = Ox%lx ", cEsp) ;
	printf("\n Function main: bsp = Ox%lx ", bsp) ;
	
	int d;
	printf("\n d = %p ", &d) ;

	register long esp1 asm("esp");
	long cEsp1 = esp1;
	printf("\nInside main: esp = Ox%lx ", cEsp1) ;

	f1(10, 20, 1, 2);
printf ( "Hello world\n") ;
	f2(1,2,3);
	
	f3(4,5,6);
	
	int e ;
	
	printf("\n   e = %p ", &e) ;
	
	register long esp2 asm("esp");
	long cEsp2 = esp2;
	printf("\n esp = Ox%lx ", cEsp2) ;	
	
	//cape_exit_func();
}
