#include <stdio.h>
void * get_pc () { return __builtin_return_address(0); }

void func1(){
	printf("In func1(): %ld\n ", (long int) get_pc() );	
}
void func1_1(){
	
	printf("Begin func1_1(): %ld\n ", (long int) get_pc() );	
	
	func1();
	
	printf("End func1_1(): %ld\n ", (long int) get_pc() );
}

void func2(){
	printf("In func2(): %ld\n ", (long int) get_pc() );	
}

int main () {
	int i;
	for(i=0; i<10; i++){
		printf("Step %d \t", i);
		printf("%ld\n",(long int) get_pc() + i);	
	}

    printf("Outside for loop: %p\n", get_pc());
    
    func2();
    func1_1();

    return 0;
}
