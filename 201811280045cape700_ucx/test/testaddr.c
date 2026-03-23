#include <stdio.h>

typedef struct vars{
	unsigned long addr;
	char type;
	unsigned int nelements;
	char ispointer;
} vars;
typedef struct varslist{
	vars var;
	struct varslist *prev, *next;
} varslist;

typedef struct pointers{
	unsigned long addr;
	unsigned long manager_addr;
	char type;
	unsigned int nelements;
} pointers;

typedef struct pointerslist{
	pointers p;
	struct pointerlist *prev, *next;	
} pointerslist;

typedef struct copydata{
	unsigned long addr;
	unsigned len;
	char *data;
	struct copydata *next, *prev;
} copydata;

char * buffer;

copydata *copy_data_head = NULL, *copy_data_tail = NULL ; 
varslist *listvars_head = NULL, *listvars_tail = NULL;
pointerslist *listpoint_head = NULL, *listpoint_tail = NULL;

int declare_variable(vars v){	
	varslist  * vl, *list;
	vl = malloc(sizeof(struct varslist));	
	vl->var.addr = v.addr;
	vl->var.ispointer = v.ispointer;
	vl->var.nelements = v.nelements;
	vl->var.type = v.type;
	vl->prev = NULL;
	vl->next = NULL;	
	if (listvars_head == NULL){
		listvars_head = vl;
		listvars_tail = vl;
		return 1;
	}	
	if(vl->var.addr <= listvars_head->var.addr){
		vl->next = listpoint_head;
		vl->prev = NULL;
		listpoint_head->prev = vl;
		listpoint_head = vl;
		return 1;
	}
	if (vl->var.addr >= listvars_tail->var.addr){
		vl->next = NULL;
		vl->prev = listvars_tail;
		listvars_tail->next = vl;
		listvars_tail = vl;
		return 1;
	}
	
	list = listvars_head;
	while(list->var.addr < vl->var.addr) list = list->next;
	vl->next = list;
	vl->prev = list->prev;
	list->prev->next = vl;
	list->prev = vl;
	return 1;
}

/*
 * If (addr of new_pointer NOT IN pointer_list) 
 * 		add_newpointer()
 *  else
 * 		return 0
 */
int add_pointer(pointers pp){
	pointerslist * pl ;
	
	pl = malloc(sizeof(struct pointerslist));
	pl->p.addr = pp.addr;
	pl->p.manager_addr = pp.manager_addr;
	pl->p.nelements = pp.nelements;
	pl->p.type = pp.type;
	pl->next = NULL;
	pl->prev = NULL;
	
	if (listpoint_head == NULL ){
		listpoint_head = pl;
		listpoint_tail = pl;
		return 1;
	}
}
//add pointer variables
/*
 * If exits(manager_addr) {remove(pointer); add_newpointer();}
 * Else
 * 	add_new_pointer();
 */

void variable(unsigned long addr, char type, unsigned int n, char ispointer){
	vars v ;	
	v.addr = addr;
	v.type = type;
	v.nelements = n;
	v.ispointer = ispointer;	
	declare_variable(v);	
}

int print_variable(){
	if (listvars_head == NULL) return 0;
	
	varslist *list;
	list = listvars_head;
	
	while(list != NULL ){
		printf("%p (%d)-[%d] -{%d} \n", list->var.addr, list->var.nelements, list->var.type, list->var.ispointer);
		list = list->next;
	}
	return 1;
}

void print_data_in_listdata(){
	copydata *listdata;
	int i;
	listdata = copy_data_head;
	while (listdata != NULL){
		printf("\n%p - %d bytes: ", listdata->addr, listdata->len );
		for(i= 0; i < listdata->len/4 ; i ++ ){
//			printf("\t%d", (*(int *)((*listdata->data) + i * 4)));		
		}
		listdata = listdata->next;
	}
	
}

int copy_data(){
	copydata *dt;
	varslist *list = NULL;
	list = listvars_head;	
	while(list != NULL){
		dt = malloc(sizeof(copydata));
		dt->addr = list->var.addr ;
		dt-> len = 4 * list->var.nelements;		
		
		printf("___ dt->len = %d \n", dt-> len );
		
	//	dt->data = (char *) malloc(dt->len +1);		
	//	memccpy(dt->data, dt->addr, dt->len);
				
		dt->next = NULL;
		dt->prev = NULL;
		
		if (copy_data_head == NULL){
			copy_data_head = dt;
			copy_data_tail = dt;
		}else{
			copy_data_tail->next = dt;
			dt->prev = copy_data_tail ;
			copy_data_tail = dt;		
		}
		list = list->next;
		
	}
	return 1;
}

void compare_data(){
	copydata *listdata = copy_data_head;
	int i;
	while(listdata != NULL){
		for(i=0; i< (listdata->len/4); i+4){
			if ((*(int *)(listdata->addr+ i * 4)) != (*(int *)(listdata->data + i * 4))){			
				printf("Addr %p: New %d # %d Old \n", (listdata->addr+ i * 4), (*(int *)(listdata->addr+ i * 4)), (*(int *)(listdata->data + i * 4)) );
			
			}		
		}
		
		listdata = listdata->next;
	}
}

void main()
{	
	int a = 10; variable(&a, 1, 1, 0);
	int *b; //variable(&b, 1, 1, 1);
	int c[5]; variable(&c, 1, 5, 0);
	int d = 0; variable(&d, 1, 1, 0);
	int i;
	
	
	
	for (i=0; i< 5; i ++)	c[i] = i+ 10 ;
	
	buffer = malloc(sizeof(int) * 5 );
	memcpy(buffer, &c, sizeof(int) * 5);
	for(i= 0; i < 5 ; i ++ ){
		printf("\t%d", (*(int *)(buffer + i * 4)));		
	}
	free(buffer);
	printf("\n");
	
	int C[10];
	for (i=0; i< 10; i ++)	C[i] = i ;
	
	buffer = malloc(sizeof(int) * 10 );
	memcpy(buffer, &C, sizeof(int) * 10);
	for(i= 0; i < 10 ; i ++ ){
		printf("\t%d", (*(int *)(buffer + i * 4)));		
	}
	free(buffer);
	
//	copy_data();
//	print_data_in_listdata();
/*	
	printf("\---------------------------------\n");
	printf("a: addr = %p, value = %d \n", &a, a);
	printf("b: addr = %p, value = %p \n", &b, b);
	printf("c: addr = [%p, %p], value = %p \n", &c[0], &c[5]);
	
	a = 20;
	
	b = &a ;
	
	d = 10;
	
	for (i=0; i< 5; i ++)
		c[i] = i +1 ;
	
	printf("---------------------------------\n");	
	printf("a: addr = %p, value = %d \n", &a, a);
	printf("b: addr = %p, value = %p \n", &b, b);
	

	
	printf("---------------------------------\n");
	b = (int *) malloc(10 * sizeof(int));
	printf("a: addr = %p, value = %d \n", &a, a);
	printf("b: addr = %p, value = %p \n", &b, b);
	
	print_variable();
	
	compare_data();
	
	printf("\n");
*/
}
