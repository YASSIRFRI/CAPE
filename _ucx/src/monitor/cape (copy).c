#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "../../include/cape.h"
#include "mpi.h"
#include <sys/mman.h>
#if DDEBUG
#define dprintf(fmt, args...) printf(fmt, ## args);
#else
#define dprintf(fmt, args...) ;
#endif

/** Set a buffer between Monitor's variables and App's variables ****/
static char buffer2[4096]; //create a space in memory
/**********Local Variables ********************************************/        
static VarList *_var_list_head = NULL;
static VarList *_var_list_tail = NULL;
static PageList *_page_list_head = NULL;
static DPageList *_dpage_list_head = NULL;

static FILE *before_ckpt_stream, *after_ckpt_stream, *final_ckpt_stream;
static char *before_ckpt, *after_ckpt, *final_ckpt;
static size_t before_ckpt_size, after_ckpt_size, final_ckpt_size;

static char __ckpt_level__ = 1;
static unsigned int __time_stamp__ = 1 ;
static int __node__ = -1; //current node
static int __nnodes__ = -1 ; // Number of working nodes
static int __total_nodes__ = -1 ; //Total nodes in the system
static int __cape_token__ = -1;
static unsigned int __current_session__ = -1;

/**********Public Variables ********************************************/     
int __left__ = -1;
int __right__ = -1;
int __i__;

static char buffer3[4096]; //create a space in memory
char buffer[4096]; //create a space in memory
struct sigaction sa;

/**********************************************************************/
/* 					Private Functions								 */
/*********************************************************************/
/*
 * ---------------------------------------------------------------------
 * Add or Update page address and properties
 * Return:
 * 	0 : page is null
 *  1 : one page is added
 * ---------------------------------------------------------------------
 */
int add_page_list(PageList **plist, Page page){

	PageList *pl;		
	pl = malloc(sizeof(struct PageList));	
	pl->addr = page.addr;
	pl->wp_flag = page.wp_flag;
	pl->next = NULL;
	pl->prev = NULL;

	
	//If the pagelist is EMPTY
	if (*plist == NULL){
		*plist = pl ;		
		return 1 ;
	}	

	PageList *tmp = NULL;
	tmp = *plist;
	//Insert or replace at the begin pagelist
	if (tmp->addr > pl->addr)
	{
		pl->next = tmp;
		tmp->prev = pl ;		
		*plist = pl ;
		return 1;
	}
		
	while((tmp->next !=NULL) && (tmp->addr < pl->addr)){
			tmp  =  tmp-> next;
	} 	
	//Insert before tmp
	if (tmp->addr > pl->addr) {
		pl->next = tmp ;
		pl->prev = tmp->prev ;
		tmp->prev->next = pl;
		tmp->prev = pl;
		return 1 ;
	} 
	//Insert after tmp (End of list)
	if (tmp->addr < pl->addr) {
		tmp->next = pl;
		pl->prev = tmp;
		return 1;	
	}

	free(pl);

	return 0;
}

 /*
 * ---------------------------------------------------------------------
 * Update wp_flag
 * ---------------------------------------------------------------------
 */
int update_page_list(PageList *plist, unsigned long addr, unsigned char flag){
	
	//If the pagelist is EMPTY
	if (plist == NULL) return 0;
	if (addr <=0 ) return 0;	
	
	PageList *tmp = NULL;
	tmp = plist;	
	
	while((tmp!=NULL) && (tmp->addr != addr)){
			tmp  =  tmp-> next;
	}
	if (tmp==NULL) return 0;
		
	if(tmp->addr == addr) {		
		tmp->wp_flag = flag ;
		return 1;
	}		
	return 0;
} 
 /*
 * ---------------------------------------------------------------------
 * Add or Update variable into List of variables
 * Return:
 * 	0: Variable is empty
 *  1: Variable is inserted
 *  2: Variable's properties is modified (explicitly)
 * ---------------------------------------------------------------------
 */
int update_var_list(VarList **vlist, VarList **vlist_tail, Var var){
	VarList *vl;
	if (var.addr <= 0) return 0;
	
	vl = malloc(sizeof(struct VarList));
	vl->next = NULL;
	vl->prev = NULL;
	vl->addr = var.addr ;
	vl->size = var.size ;
	vl->n = var.n;
	vl->pro = var.pro;
	vl->level=var.level;
	vl->dtype =var.dtype;
	
	if (*vlist == NULL ) {
		*vlist = vl;
		*vlist_tail = vl;
		return 1;
	}
		
	VarList *tmp,*tmp2;
	tmp = *vlist;
	tmp2 = *vlist_tail;
	//Insert at the begin of list
	if (tmp->addr > vl->addr) {
		tmp->prev = vl;
		vl->next = tmp;
		*vlist = vl ;
		return 1;
	}	
	//Mofify the variable properties
	if (tmp->addr == vl->addr) {		
		free(vl);
		return 2;
	}	
	//Insert at the end of list
	if(tmp2->addr < vl->addr){
		tmp2->next = vl;
		vl->prev = tmp2;
		*vlist_tail = vl;
		return 1;
	}
	//Insert at the end of list
	if(tmp2->addr == vl->addr){
		free(vl);
		return 2;
	}
	
	//Find the position to insert or modify
	while ((tmp->next != NULL) && (tmp->addr < vl->addr )){
		tmp = tmp->next;
	}	
	//Insert before tmp
	if (tmp->addr > vl->addr){
		vl->next = tmp;
		vl->prev = tmp->prev;
		tmp->prev->next = vl;
		tmp->prev = vl ;
		return 1;
	}
	//Exist in list
	if (tmp->addr == vl->addr){
		free(vl);
		return 2;
	}	
		
}

/*
 * ---------------------------------------------------------------------
 * Doublicate Varlist and add new level of properties
 * ---------------------------------------------------------------------
 */
int add_var_level(VarList **vlist_head, VarList **vlist_tail, unsigned char level){

	VarList *vl_tail_level = NULL;
	VarList *vl_head = NULL;
	
	vl_head = *vlist_head;
	vl_tail_level = *vlist_tail;
	
	if(vl_head == NULL) return 0;
	VarList *copy_item = NULL;		
	VarList *item = NULL;
	
	//Copy items and assigned new level
	item = vl_head;
	while(item->level == 0){			
		copy_item = malloc(sizeof(VarList));		
		copy_item->addr = item->addr;
		copy_item->size = item->size;
		copy_item->n = item->n;
		copy_item->dtype = item->dtype;
		copy_item->pro = item->pro; 
		copy_item->level = __ckpt_level__; //New level			
		copy_item->next = NULL;
		copy_item->prev = vl_tail_level;		
		vl_tail_level->next = copy_item;
		vl_tail_level = copy_item;		
		item = item->next;
	}	
	*vlist_tail = vl_tail_level;
	return 1;	
}

/**
 * ---------------------------------------------------------------------
 * Remove variables level
 *----------------------------------------------------------------------
 */
int remove_var_level(VarList **vlist_head, VarList **vlist_tail, unsigned char level){

	VarList *vl_tail;
	VarList *vl_tmp;
	
	vl_tail = *vlist_tail;
	if ((vl_tail ==NULL ) ||(vl_tail->level == 0)) return 0;
	
	while((vl_tail->level == level) && (vl_tail != NULL) && (vl_tail->level != 0)){		
		vl_tmp = vl_tail;
		vl_tail = vl_tail->prev;
		vl_tail->next = NULL;
		vl_tmp->prev = NULL;
		free(vl_tmp);	
	} 
	*vlist_tail = vl_tail;	
	return 1;
}

/**
 * ---------------------------------------------------------------------
 * Set data attribute: default(none)
 * ---------------------------------------------------------------------
 */
void set_default_none(VarList *vlist_tail, char level){
	VarList *vtail;
	vtail = vlist_tail;
	while ((vtail != NULL ) && (vtail->level == level) ){
		vtail->pro = CAPE_PRIVATE;		
		vtail = vtail->prev;
	}
}

/**
 * ---------------------------------------------------------------------
 * Set data attribute
 * ---------------------------------------------------------------------
 */
void set_data_attribute(VarList *vlist_tail, long addr, char pro, char level){
	VarList *vtail;
	vtail = vlist_tail;
	while ((vtail != NULL ) && (vtail->level == level) ){
		if (vtail->addr == addr) {
			vtail->pro = pro;
			break;		
		}
		vtail = vtail->prev;
	}
}

/**
 * ---------------------------------------------------------------------
 * Set thread private
 * ---------------------------------------------------------------------
 */
void set_threadprivate(VarList *vlist_head, long addr){
	VarList *vhead;
	vhead = vlist_head;
	while (vhead != NULL ){
		if (vhead->addr == addr) {
			vhead->pro = CAPE_PRIVATE;
			break;		
		}
		vhead = vhead->next;
	}
}

 

/**
 * ---------------------------------------------------------------------
 * Copy all pages that containt shareable variables
 * 		Return: number of pages and list of data
 * ---------------------------------------------------------------------
 */
int copy_dpage(DPageList **dplist, PageList *plist){
	
	if (plist == NULL) return 0;
	int rc = 0;
	DPageList * dpl = NULL;
	DPageList * tmp = NULL;
	
	PageList *pl = plist;
	
	rc ++;
	dpl = malloc(sizeof(DPageList));
	dpl->addr = pl->addr;
	memcpy(dpl->data, dpl->addr, CAPE_PAGE_SIZE);
	dpl->next = NULL;
	dpl->prev = NULL;
	*dplist = dpl;
	tmp = dpl;
		
	pl = pl->next;
	
	while(pl != NULL){
		rc ++;		
		dpl = malloc(sizeof(DPageList));
		dpl->addr = pl->addr;
		memcpy(dpl->data, dpl->addr, CAPE_PAGE_SIZE);
		dpl->next = NULL;
		dpl->prev = tmp;
		tmp->next = dpl;

		tmp = tmp->next;
		pl = pl->next;
	}	
	
	return rc;

}


/*
 * ---------------------------------------------------------------------
 * Add a DPage list into List of Data that is allocated in the modified page
 * ---------------------------------------------------------------------
 */
int update_dpage_list(DPageList **dplist, unsigned long addr){	
	
	DPageList *_dpl = NULL;
	if (addr <= 0) return 0;		
	
	_dpl = malloc(sizeof(struct DPageList));
	_dpl->addr =  addr & ~(CAPE_PAGE_SIZE -1);	
	memcpy(_dpl->data, _dpl->addr, CAPE_PAGE_SIZE);
			
	_dpl->next = NULL;
	_dpl->prev = NULL;
	
	//If DPageList is NULL
	if (*dplist == NULL){
		*dplist = _dpl ;
		return 1;		
	}
	
	DPageList *tmp = NULL;
	tmp = *dplist;
	if (tmp->addr > _dpl->addr)
	{
		_dpl->next = tmp;
		tmp->prev = _dpl;
		*dplist = _dpl;
		return 1;		
	}
	
	while ( (tmp->next !=NULL) && (tmp->addr < _dpl->addr)){
		tmp = tmp->next ;
	}
	
	if(tmp->addr > _dpl->addr){
		_dpl->next = tmp;
		_dpl->prev = tmp->prev;
		tmp->prev->next = _dpl ;
		tmp->prev = _dpl;
		return 1;
	}
	
	if (tmp->addr < _dpl->addr){
		tmp->next = _dpl;
		_dpl->prev = tmp;
		return 1;
	}
		
	return 0;
}
/*
 * ---------------------------------------------------------------------
 * Set write protected to all page in PageList
 * ---------------------------------------------------------------------
 */
int set_write_protected(PageList *plist){
	PageList * _plist;

	_plist = plist;
	if (_plist == NULL) return 0;
	while(_plist != NULL){		
		_plist->wp_flag = PAGE_WRITE_PROTECTED ;
	
		
		if (mprotect(_plist->addr, CAPE_PAGE_SIZE, PROT_READ) == -1)
               handle_error("mprotect");
        dprintf("Page Ox%lx is write protected\n", _plist->addr);
        _plist = _plist->next;
	}	
	return 1;
}
/*
 * ---------------------------------------------------------------------
 * Remove write protected 
 * ---------------------------------------------------------------------
 */
int remove_write_protected(PageList *plist){
	PageList * _plist;
	_plist = plist;
	if (_plist == NULL) return 0;
	while(_plist != NULL){		
		if(_plist->wp_flag == PAGE_WRITE_PROTECTED ){			
			if (mprotect(_plist->addr, CAPE_PAGE_SIZE, PROT_READ | PROT_WRITE) == -1)
				handle_error("mprotect");
			_plist->wp_flag = PAGE_WRITABLE;
			dprintf("Page Ox%lx is writeable (%d) \n", _plist->addr, _plist->wp_flag);
		}
        _plist= _plist->next;
	}	
	return 1;
}
/*
 * ---------------------------------------------------------------------
 * Remove write protected a page
 * ---------------------------------------------------------------------
 */
int remove_page_write_protected(unsigned long addr){	
	if (addr <= 0) return 0;
	long aligned_addr = addr & ~(CAPE_PAGE_SIZE -1);	
	if (mprotect(aligned_addr, CAPE_PAGE_SIZE, PROT_WRITE | PROT_EXEC | PROT_READ) == -1)
               handle_error("mprotect");
    update_page_list(_page_list_head, aligned_addr, PAGE_WRITABLE);
	return 1;	
}
/*
 *----------------------------------------------------------------------
 * Clear Data in DPagelist and free memory
 *----------------------------------------------------------------------
 */
void clear_dpage_list(DPageList **dplist){
	DPageList *_dpl = NULL;
	_dpl = *dplist;
	while(_dpl->next != NULL){
		DPageList *_tmp = _dpl; 
		_dpl = _dpl->next;
		free(_tmp);		
	}		
	if (_dpl !=NULL) 
		free(_dpl);
	*dplist = NULL;
}
/*
 * ---------------------------------------------------------------------
 * Find variable in VarList that contain addr_start
 * ---------------------------------------------------------------------
 */
VarList *find_variable(VarList *vlist, unsigned long addr, char level){
	VarList *vl = NULL;
	vl = vlist;	
	if (vl == NULL) return vl;
	while (vl!= NULL) {
		if ((addr >= vl->addr) && (addr <  vl->addr + (vl->n * vl->size) ) ){						
			// printf("Ox%lx is in VarList: [Ox%lx - Ox%lx) \n" , addr, vl->addr, vl->addr + (vl->n * vl->size));
			return vl;
		}
		else{			
//			printf("Checking: Ox%lx is in VarList: [Ox%lx - Ox%lx) - Level: %d \n" , addr, vl->addr, vl->addr + (vl->n * vl->size), vl->level);
			vl = vl->next;
		}
	}
	return vl;	
}
/*
 * ---------------------------------------------------------------------
 * Find variable in VarList with address addr * 
 * ---------------------------------------------------------------------
 */
VarList * find_variable_by_addr(VarList *vlist, long addr, char level){
	VarList *vl = NULL;
	vl = vlist;	
	if (vl == NULL) return vl;
	while (vl != NULL) {
		if ((vl->addr == addr) && (vl->level == level ) ){				 
			break;
		}
		//			printf("Checking: Ox%lx is in VarList: [Ox%lx - Ox%lx) - Level: %d \n" , addr, vl->addr, vl->addr + (vl->n * vl->size), vl->level);
		vl = vl->next;		
	}
	return vl;	
}



/**
 * ---------------------------------------------------------------------
 * Generate checkpoint (BACKUP)
 * 	Checkpoint will be generated depending on properties of variables 
 * 	and the phase of execution model.
 * Checkpoint Structure:
 * {t , size_of_S, S, L}, 
 * 		where S = All modified data (addr, len, data)
 * 			  L = data of reduction variables (addr, len, data)
 *----------------------------------------------------------------------
 */
FILE *generate_checkpoint_backup(DPageList *dplist,
		char *ckpt_data, size_t *ckpt_size,
		VarList *vlist,	unsigned char level,
		unsigned char cflag,
		unsigned long tsp ){
	
	VarList *v = NULL, *v1 = NULL;
	DPageList *dp = NULL;
	unsigned long timestamp;
	unsigned long size_s = 0;
	unsigned long start_addr;
	unsigned long page_end, v_addr;
	unsigned long start;
	unsigned int len;
	FILE *stream;
	
	if (dplist == NULL ) return NULL;
	
	//open checkpoint file
	timestamp = tsp;
	stream = open_memstream(ckpt_data, ckpt_size);
	
	//write time stamp into checkpoint file
	fwrite(&timestamp, sizeof(unsigned long), 1, stream);
	fflush(stream);
	
	//Write size_s into checkpoint file, and this place will be modified after writting S part
	fwrite(&size_s, sizeof(unsigned long), 1, stream);
	fflush(stream);
	
	//Move to current window (the current level of VarList)
	v = vlist;
	while ((v != NULL) && (v->level != level))
		v = v->next;
		
	//Move to the first DPAGE
	while ((v != NULL) && ((v->addr + v->n * v->size) < dplist->addr))
		v = v->next;
	
		
	start_addr = v->addr;	
	
	//Foreach page, find midifiled data of shared variables and save into cheeckpoint file	
	dp = dplist;
		
	while(dp != NULL){		
		page_end = dp->addr + CAPE_PAGE_SIZE;					
		if (dp->addr > start_addr)
			start_addr = dp->addr;
			//printf("Working from 0x%lx - 0x%lx \n", start_addr, page_end );						
		while(start_addr < page_end){					
			v1 = NULL;			
			v1 = find_variable(v, start_addr, level);			
			if (v1 == NULL) {
				start_addr +=CAPE_WORD;
				continue;
			}			
			v = v1;			
			if ((cflag == ENTRY_CHECKPOINT)	&& 		\
					((v->pro == CAPE_PRIVATE) || (v->pro == CAPE_LAST_PRIVATE) || (v->pro==CAPE_COPY_PRIVATE))){			
				start_addr  = v->addr + (v->size * v->n); //Move to next variable
				continue;
			}	
			
			if((cflag == EXIT_CHECKPOINT) && 	\
				((v->pro== CAPE_PRIVATE) || (v->pro==CAPE_FIRST_PRIVATE) || (v->pro== CAPE_COPY_IN) ||
				 (v->pro== CAPE_SUM) || (v->pro == CAPE_MUL) || (v->pro == CAPE_MAX) || (v->pro == CAPE_MIN))
				){
				start_addr = v->addr + (v->size * v->n) ; 
				continue;
			}			
			
			//Size of this variable is 4 bytes
			if (v->size == CAPE_WORD){						
				//Ignore the data that is not modified
				while ( (start_addr < page_end) &&
						(start_addr < (v->addr + v->n*v->size)) &&
						( (*(int *)start_addr) == (*(int *) (dp->data + (start_addr - dp->addr ))) )	)			
					start_addr += CAPE_WORD;
				
				if ((start_addr >= page_end) || (start_addr >= (v->addr + v->n*v->size)))
					continue;
					
				//Count the modified memory
				start = start_addr;				
				while ( (start_addr < page_end) &&
						(start_addr < (v->addr + v->n*v->size)) &&
						( (*(int *)start_addr) != (*(int *) (dp->data + (start_addr - dp->addr ))) )	)			
					start_addr += CAPE_WORD;
				
				//Save the modified data into checkpoint file 
				if (start_addr > start){					
					len = start_addr - start;
					fwrite(&start, sizeof(unsigned long), 1, stream);
					fwrite(&len, sizeof(unsigned int), 1, stream);
					fwrite(start, len, 1, stream);
					fflush(stream);	
//					dprintf("Saved 0x%lx - %d bytes \n", start, len);
				}	
			}
			else //8-byte variables
			{
				//Ignore the data that is not modified
				while ( (start_addr < page_end) &&
						(start_addr < (v->addr + v->n*v->size)) &&
						( (*(double *)start_addr) == (*(double *) (dp->data + (start_addr - dp->addr ))) )	)			
					start_addr += DOUBLE_CAPE_WORD;
				
				if ((start_addr >= page_end) || (start_addr >= (v->addr + v->n*v->size)))
					continue;
				
				//Count the modified memory
				start = start_addr;				
				while ( (start_addr < page_end) &&
						(start_addr < (v->addr + v->n*v->size)) &&
						( (*(double *)start_addr) != (*(double *) (dp->data + (start_addr - dp->addr ))) )	)			
					start_addr += DOUBLE_CAPE_WORD;
				
				//Save the modified data into checkpoint file 
				if (start_addr > start){
					len = start_addr - start;
					fwrite(&start, sizeof(unsigned long), 1, stream);
					fwrite(&len, sizeof(unsigned int), 1, stream);
					fwrite(start, len, 1, stream);
					fflush(stream);	
									
//					dprintf("Saved - DOUBLE -  0x%lx - %d bytes \n", start, start_addr - start);				
				}	
				
			}					
			
		}
				
		dp = dp->next;
	}
	
	//Identity size of S part
	size_s = *ckpt_size;
	
	
	//Write L part into checkpoint
	if(cflag == EXIT_CHECKPOINT){
		//Move to current window (the current level of VarList)
		v = vlist;
		while ((v != NULL) && (v->level != level))
			v = v->next;
		while ((v != NULL) && (v->level == level)){
			if ((v->pro == CAPE_SUM) || (v->pro == CAPE_MUL) || (v->pro == CAPE_MAX) ||(v->pro == CAPE_MIN) ){
				len = v->size * v->n;
				fwrite(&v->addr, sizeof(long), 1, stream);
				fwrite(&len, sizeof(unsigned int), 1, stream);
				fwrite(v->addr, len, 1, stream);
				fflush(stream);				
			}
			v = v->next;
		}			
	}
	//printf("-------- v->adr: Node %d: 0x%lx - Data %d \n", __node__, v->addr, *(int *)v->addr);
	
	//print_checkpoint_data(ckpt_data, *ckpt_size);
	
	unsigned long save_file_pointer = *ckpt_size;
	//Modify size_s in checkpoint file	
	fseek(stream, sizeof(unsigned int), SEEK_SET);
	fwrite(&size_s, sizeof(unsigned int), 1, stream);
	fflush(stream);	
	
	fseek(stream, save_file_pointer, SEEK_SET);
	fflush(stream);
	//memcpy((ckpt_data + sizeof(unsigned long)), &size_s, sizeof(unsigned long));
	
	
	return stream;

}

/**
 * ---------------------------------------------------------------------
 * Generate checkpoint 
 * 	Checkpoint will be generated depending on properties of variables 
 * 	and the phase of execution model.
 * Checkpoint Structure:
 * C = {t , size_of_S, S, L}, 
 * 		where S = All modified data (addr, len, data)
 * 			  L = data of reduction variables (addr, len, data)
 *----------------------------------------------------------------------
 */
FILE *generate_checkpoint(DPageList *dplist,
		VarList *vlist,	unsigned char level,
		unsigned char cflag,
		unsigned char ops_flag,
		unsigned long tsp ){
	
	VarList *v = NULL, *v1 = NULL;
	DPageList *dp = NULL;
	unsigned long timestamp;
	unsigned long size_s = 0;
	unsigned long start_addr;
	unsigned long page_end, v_addr;
	unsigned long start;
	unsigned int len;
	FILE *stream;
	
	if (dplist == NULL ) return NULL;
	
	//open checkpoint file
	timestamp = tsp;
	stream = open_memstream(&after_ckpt, &after_ckpt_size);
	
	//write time stamp into checkpoint file
	fwrite(&timestamp, sizeof(unsigned long), 1, stream);
	fflush(stream);
	
	//Write size_s into checkpoint file, and this place will be modified after writting S part
	fwrite(&size_s, sizeof(unsigned long), 1, stream);
	fflush(stream);
	
	//Move to current window (the current level of VarList)
	v = vlist;
	while ((v != NULL) && (v->level != level))
		v = v->next;
		
	//Move to the first DPAGE
	while ((v != NULL) && ((v->addr + v->n * v->size) < dplist->addr))
		v = v->next;
	
		
	start_addr = v->addr;	
	
	//Foreach page, find midifiled data of shared variables and save into cheeckpoint file	
	dp = dplist;
		
	while(dp != NULL){		
		page_end = dp->addr + CAPE_PAGE_SIZE;					
		if (dp->addr > start_addr)
			start_addr = dp->addr;
			//printf("Working from 0x%lx - 0x%lx \n", start_addr, page_end );						
		while(start_addr < page_end){					
			v1 = NULL;			
			v1 = find_variable(v, start_addr, level);			
			if (v1 == NULL) {
				start_addr +=CAPE_WORD;
				continue;
			}			
			v = v1;			
			if ((cflag == ENTRY_CHECKPOINT)	&& 		\
					((v->pro == CAPE_PRIVATE) || (v->pro == CAPE_LAST_PRIVATE) || (v->pro==CAPE_COPY_PRIVATE))){			
				start_addr  = v->addr + (v->size * v->n); //Move to next variable
				continue;
			}	
			if (ops_flag == TRUE){
				if((cflag == EXIT_CHECKPOINT) &&	\
					((v->pro== CAPE_PRIVATE) || (v->pro==CAPE_FIRST_PRIVATE) || (v->pro== CAPE_COPY_IN) ||
					(v->pro== CAPE_SUM) || (v->pro == CAPE_MUL) || (v->pro == CAPE_MAX) || (v->pro == CAPE_MIN))
					){
					start_addr = v->addr + (v->size * v->n) ; 
					continue;
				}		
			}	
			
			//Size of this variable is 4 bytes
			if (v->size == CAPE_WORD){						
				//Ignore the data that is not modified
				while ( (start_addr < page_end) &&
						(start_addr < (v->addr + v->n*v->size)) &&
						( (*(int *)start_addr) == (*(int *) (dp->data + (start_addr - dp->addr ))) )	)			
					start_addr += CAPE_WORD;
				
				if ((start_addr >= page_end) || (start_addr >= (v->addr + v->n*v->size)))
					continue;
					
				//Count the modified memory
				start = start_addr;				
				while ( (start_addr < page_end) &&
						(start_addr < (v->addr + v->n*v->size)) &&
						( (*(int *)start_addr) != (*(int *) (dp->data + (start_addr - dp->addr ))) )	)			
					start_addr += CAPE_WORD;
				
				//Save the modified data into checkpoint file 
				if (start_addr > start){					
					len = start_addr - start;
					fwrite(&start, sizeof(unsigned long), 1, stream);
					fwrite(&len, sizeof(unsigned int), 1, stream);
					fwrite(start, len, 1, stream);
					fflush(stream);	
//					dprintf("Saved 0x%lx - %d bytes \n", start, len);
				}	
			}
			else //8-byte variables
			{
				//Ignore the data that is not modified
				while ( (start_addr < page_end) &&
						(start_addr < (v->addr + v->n*v->size)) &&
						( (*(double *)start_addr) == (*(double *) (dp->data + (start_addr - dp->addr ))) )	)			
					start_addr += DOUBLE_CAPE_WORD;
				
				if ((start_addr >= page_end) || (start_addr >= (v->addr + v->n*v->size)))
					continue;
				
				//Count the modified memory
				start = start_addr;				
				while ( (start_addr < page_end) &&
						(start_addr < (v->addr + v->n*v->size)) &&
						( (*(double *)start_addr) != (*(double *) (dp->data + (start_addr - dp->addr ))) )	)			
					start_addr += DOUBLE_CAPE_WORD;
				
				//Save the modified data into checkpoint file 
				if (start_addr > start){
					len = start_addr - start;
					fwrite(&start, sizeof(unsigned long), 1, stream);
					fwrite(&len, sizeof(unsigned int), 1, stream);
					fwrite(start, len, 1, stream);
					fflush(stream);	
									
//					dprintf("Saved - DOUBLE -  0x%lx - %d bytes \n", start, start_addr - start);				
				}	
				
			}					
			
		}
				
		dp = dp->next;
	}
	
	//Identity size of S part
	size_s = after_ckpt_size;		
	//Write L part into checkpoint
	if((cflag == EXIT_CHECKPOINT) && (ops_flag==TRUE)){
		//Move to current window (the current level of VarList)
		v = vlist;
		while ((v != NULL) && (v->level != level))
			v = v->next;
		while ((v != NULL) && (v->level == level)){
			if ((v->pro == CAPE_SUM) || (v->pro == CAPE_MUL) || (v->pro == CAPE_MAX) ||(v->pro == CAPE_MIN) ){
				len = v->size * v->n;
				fwrite(&v->addr, sizeof(long), 1, stream);
				fwrite(&len, sizeof(unsigned int), 1, stream);
				fwrite(v->addr, len, 1, stream);
				fflush(stream);				
			}
			v = v->next;
		}			
	}

	memcpy((after_ckpt + sizeof(unsigned int)), &size_s, sizeof(unsigned int));	
	
	return stream;

}

/**
 * ---------------------------------------------------------------------
 * Merge S = S1 + S2
 * Write S into after_checkpoint
 * Structer of S part is: {(addr, len, data) .... }
 * --------------------------------------------------------------------- 
 */
int merge_data(char *s1, unsigned int pos_s1, unsigned size_s1, 
				char *s2, unsigned int pos_s2, unsigned size_s2){
	unsigned int p1, p2;
	unsigned int len, len1, len2;
	long addr1, addr2, old_addr2;
	p1 = pos_s1;
	p2 = pos_s2;
	
	if ((p1 >= size_s1) && (p2 >=size_s2))
		return 0;
	//if S1 == NULL =>  S = S2
	if (p1 >= size_s1 ){
		fwrite(s2 + p2, size_s2 - p2, 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		return 1;
	}
	//if S2 == NULL => S = S1
	if (p2 >= size_s2 ){
		fwrite(s1 + p1, size_s1 - p1, 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		return 1;
	}	
	addr1 = *(long *) (s1 + p1 );
	p1 += sizeof(long);
	len1 = *(unsigned int *) (s1 + p1) ;
	p1 += sizeof(unsigned int);
	
	addr2 = *(long *) (s2 + p2 );
	p2 += sizeof(long);
	len2 = *(unsigned int *) (s2 + p2) ;
	p2 += sizeof(unsigned int);
		
	while ((p1 < size_s1) && (p2 < size_s2)){
		if (addr1 <= addr2){
			fwrite(&addr1, sizeof(long), 1, after_ckpt_stream);
			fflush(after_ckpt_stream);
			fwrite(&len1, sizeof(unsigned int), 1, after_ckpt_stream);
			fflush(after_ckpt_stream);			
			fwrite(s1 + p1, len1, 1, after_ckpt_stream);
			fflush(after_ckpt_stream);
			p1 += len1;
			//S1: [-----------)  
			//S2:     -----		 ------	
			if ((addr1 + len1) >= (addr2+ len2)){
				p2 += len2;
				if (p2 < size_s2) {				
					addr2 = *(long *) (s2 + p2 );
					p2 += sizeof(long);
					len2 = *(unsigned int *) (s2 + p2) ;
					p2 += sizeof(unsigned int);
				}
			}			
			//S1: [-----------)       [--------)
			//S2:        -----[----)
			if (((addr1 + len1) >= addr2) && ((addr1 + len1) < (addr2 + len2))){
				old_addr2 = addr2; //save the old addr
				addr2 = addr1 + len1;
				len2 = len2 - (addr2 - old_addr2);
				p2 += (addr2 - old_addr2);			
			}			
			if (p1 < size_s1) {
				addr1 = *(long *) (s1 + p1 );
				p1 += sizeof(long);
				len1 = *(unsigned int *) (s1 + p1) ;
				p1 += sizeof(unsigned int);
			}					
		}else{ //addr1 > addr2
			//S1:                      [-----------)         [--------)
			//S2:        [--------)		    -------[------)	
			if ((addr2 + len2) < addr1) {
				fwrite(&addr2, sizeof(long), 1, after_ckpt_stream);
				fflush(after_ckpt_stream);
				fwrite(&len2, sizeof(unsigned int), 1, after_ckpt_stream);
				fflush(after_ckpt_stream);			
				fwrite(s2 + p2, len2, 1, after_ckpt_stream);
				fflush(after_ckpt_stream);
				p2 += len2;					
				if (p2 >= size_s2) break;
				addr2 = *(long *) (s2 + p2 );
				p2 += sizeof(long);
				len2 = *(unsigned int *) (s2 + p2) ;
				p2 += sizeof(unsigned int);												
			//S1:       [-----------)        [--------)
			//S2:       	 -------[--------)--------[------)	
			}else {
				len = addr1 - addr2;				
				fwrite(&addr2, sizeof(long), 1, after_ckpt_stream);
				fflush(after_ckpt_stream);				
				fwrite(&len, sizeof(unsigned int), 1, after_ckpt_stream);
				fflush(after_ckpt_stream);
				fwrite(s2 + p2, len2, 1, after_ckpt_stream);
				fflush(after_ckpt_stream);
				p2 += len;

				len2 = len2-len;	
				addr2 = addr1;							
			}
		}	
	}	
	//Merge the rest part
	if (p1 < size_s1){
		fwrite(&addr1, sizeof(long), 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		fwrite(&len1, sizeof(unsigned int), 1, after_ckpt_stream);
		fflush(after_ckpt_stream);			
		fwrite(s1 + p1, len1, 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		p1 += len1;
	}	
	while (p1 < size_s1){
		addr1 = *(long *) (s1 + p1 );
		p1 += sizeof(long);
		len1 = *(unsigned int *) (s1 + p1) ;
		p1 += sizeof(unsigned int);
		fwrite(&addr1, sizeof(long), 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		fwrite(&len1, sizeof(unsigned int), 1, after_ckpt_stream);
		fflush(after_ckpt_stream);			
		fwrite(s1 + p1, len1, 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		p1 += len1;		
	}
	
	if (p2 < size_s2){
		fwrite(&addr2, sizeof(long), 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		fwrite(&len2, sizeof(unsigned int), 1, after_ckpt_stream);
		fflush(after_ckpt_stream);			
		fwrite(s2 + p2, len2, 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		p2 += len2;
	}	
	while (p2 < size_s2){
		addr2 = *(long *) (s2 + p2 );
		p2 += sizeof(long);
		len2 = *(unsigned int *) (s2 + p2) ;
		p2 += sizeof(unsigned int);
		fwrite(&addr2, sizeof(long), 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		fwrite(&len2, sizeof(unsigned int), 1, after_ckpt_stream);
		fflush(after_ckpt_stream);			
		fwrite(s2 + p2, len2, 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		p2 += len2;		
	}
	return 2;	
}
/*
 * ---------------------------------------------------------------------
 * Merge a buffer to after_checkpoint
 * 	if (t1 >= t2) 
 * 		{ C <- t1 ; C <- S2; C <- S1}
 * 	else
 * 		{ C <- t2 ; C <- S1; C <- S2}
 * --------------------------------------------------------------------- 
 */
int merge_checkpoint(char *src_ckpt, size_t src_size, char ckpt_flag){	
	
	FILE *tmp_stream;
	char *tmp_ckpt;
	size_t tmp_size;
	
	unsigned int src_pointer, tmp_pointer;
	
	if (src_size <= 4)
		return 0;
	
	if (after_ckpt_size == 0){	
		after_ckpt_stream = open_memstream(&after_ckpt, &after_ckpt_size);
		fwrite(src_ckpt, src_size, 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		return src_size;	
	}	
	//Copy des_stream file to tmp_stream file
	tmp_stream = open_memstream(&tmp_ckpt, &tmp_size);
	fwrite(after_ckpt, after_ckpt_size, 1,tmp_stream);
	fflush(tmp_stream);
	
	//Close des_stream file
	fclose(after_ckpt_stream);
	free(after_ckpt);
	after_ckpt = NULL;
	after_ckpt_size = 0;
		
	src_pointer = 0;
 	tmp_pointer =0 ;
 	unsigned long t1, t2;
 	unsigned long size_s = 0, size_s1, size_s2;
  	
 	//1. Read timespan 	
// 	fseek(tmp_stream, tmp_pointer, SEEK_SET);
// 	fread(&t1, sizeof(unsigned long), 1, tmp_stream);
 	
 	t1 = *(unsigned long *)tmp_ckpt;
 	t2 = *(unsigned long *)src_ckpt; 
 	 	
 	tmp_pointer += sizeof(unsigned long);
 	src_pointer += sizeof(unsigned long);
 	
 	size_s1 =  *(unsigned long *)(tmp_ckpt + tmp_pointer);
 	size_s2 =  *(unsigned long *)(src_ckpt + src_pointer);
 	 	
 	src_pointer += sizeof(unsigned long);
 	tmp_pointer += sizeof(unsigned long);
	 	
 	//Open des_stream file again
 	after_ckpt_stream = open_memstream(&after_ckpt, &after_ckpt_size);
 	
 	
 	if (t1 >= t2){ 			//C <- t1, size_s, S2, S1		
		fwrite(&t1, sizeof(unsigned long), 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		fwrite(&size_s, sizeof(unsigned long), 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		merge_data(tmp_ckpt, tmp_pointer, size_s1, src_ckpt, src_pointer, size_s2);		
	}else{	//C <-	t1, size_s, S1, S2		
		fwrite(&t2, sizeof(unsigned long), 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		fwrite(&size_s, sizeof(unsigned long), 1, after_ckpt_stream);
		fflush(after_ckpt_stream);		
		merge_data(src_ckpt, src_pointer, size_s2, tmp_ckpt, tmp_pointer, size_s1);				
	}
	tmp_pointer = size_s1;
	src_pointer = size_s2;	
	size_s = after_ckpt_size;
	
	//Check if exist reduction data
	if (ckpt_flag == EXIT_CHECKPOINT){ 
		while (	(tmp_pointer < tmp_size) && (src_pointer <src_size)){		
			long addr;
			unsigned int len;
			
			addr = *((long*)(tmp_ckpt + tmp_pointer));
			src_pointer += sizeof(long);
			tmp_pointer += sizeof(long);

			len = *(unsigned int *) (tmp_ckpt + tmp_pointer) ;
			src_pointer += sizeof(long);
			tmp_pointer += sizeof(long);
			
			fwrite(&addr, sizeof(long), 1, after_ckpt_stream);
			fflush(after_ckpt_stream);
			fwrite(&len, sizeof(unsigned int), 1, after_ckpt_stream);
			fflush(after_ckpt_stream);

			VarList *var = NULL; 
			var = find_variable_by_addr(_var_list_head, addr , __ckpt_level__);	
			
			if (var == NULL) return -1; //ERROR

			int num, n1, n2;
			unsigned long num_l, n1_l, n2_l;
			float num_f, n1_f, n2_f;
			double num_d, n1_d, n2_d;			
			switch (var->dtype){
				case CAPE_CHAR:
				case CAPE_INT:
				case CAPE_LONG:					
					n1 =  *(long*) (tmp_ckpt + tmp_pointer);
					n2=  *(long*) (src_ckpt + src_pointer) ;
					if (var->pro == CAPE_SUM){
						num = 0;
						num = n1 + n2;
						fwrite(&num, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}
					if (var->pro == CAPE_MUL){
						num = 1;
						num = n1 * n2;		
						fwrite(&num, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}					
					if (var->pro == CAPE_MAX){
						num = (n1 >= n2 )? n1 : n2 ;
						fwrite(&num, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}
					if (var->pro == CAPE_MIN){						
						num = (n1 < n2 )? n1 : n2 ;				
						fwrite(&num, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}							
					break;
				case CAPE_UNSIGNED_CHAR:	
				case CAPE_UNSIGNED_INT:
				case CAPE_UNSIGNED_LONG:					
					n1_l =  *(unsigned long*) (tmp_ckpt + tmp_pointer);
					n2_l=  *(unsigned long*) (src_ckpt + src_pointer) ;
					if (var->pro == CAPE_SUM){
						num_l = 0;
						num_l = n1_l + n2_l;
						fwrite(&num_l, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}
					if (var->pro == CAPE_MUL){
						num_l = 1;
						num_l = n1_l * n2_l;		
						fwrite(&num_l, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}					
					if (var->pro == CAPE_MAX){
						num_l = (n1_l >= n2_l )? n1_l : n2_l ;
						fwrite(&num_l, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}
					if (var->pro == CAPE_MIN){						
						num_l = (n1_l < n2_l )? n1_l : n2_l ;				
						fwrite(&num_l, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}							
					break;
				case CAPE_FLOAT:					
					n1_f =  *(float*) (tmp_ckpt + tmp_pointer);
					n2_f=  *(float*) (src_ckpt + src_pointer) ;
					if (var->pro == CAPE_SUM){
						num_f = 0.0;
						num_f = n1_f + n2_f;
						fwrite(&num_f, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}
					if (var->pro == CAPE_MUL){
						num_f = 1.0;
						num_f = n1_f * n2_f;		
						fwrite(&num_f, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}					
					if (var->pro == CAPE_MAX){
						num_f = (n1_f >= n2_f )? n1_f : n2_f ;
						fwrite(&num_f, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}
					if (var->pro == CAPE_MIN){						
						num_f = (n1_f < n2_f )? n1_f : n2_f ;				
						fwrite(&num_f, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}							
					break;
				case CAPE_DOUBLE:					
					n1_d =  *(double*) (tmp_ckpt + tmp_pointer);
					n2_d=  *(double*) (src_ckpt + src_pointer);					
					if (var->pro == CAPE_SUM){
						num_d = 0.0;
						num_d = n1_d + n2_d;
						fwrite(&num_d, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}
					if (var->pro == CAPE_MUL){
						num_d = 1.0;
						num_d = n1_d * n2_d;		
						fwrite(&num_d, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}					
					if (var->pro == CAPE_MAX){
						num = (n1_d >= n2_d )? n1_d : n2_d ;
						fwrite(&num_d, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}
					if (var->pro == CAPE_MIN){						
						num_d = (n1_d < n2_d )? n1_d : n2_d ;				
						fwrite(&num_d, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}							
					break;
				default:
					printf("This datatype is not supported !!!!!");
					return -1;
			}
			
			src_pointer += len;
			tmp_pointer += len;
		}
		

	}	
	memcpy(after_ckpt + sizeof(unsigned long), &size_s, sizeof(unsigned long) );	
	

	fclose(tmp_stream);
	free(tmp_ckpt);	
	tmp_ckpt = NULL;
	
	return src_size;	
}


/*
 * ---------------------------------------------------------------------
 * Merge a buffer to after_checkpoint
 * 	if (t1 >= t2) 
 * 		{ C <- t1 ; C <- S2; C <- S1}
 * 	else
 * 		{ C <- t2 ; C <- S1; C <- S2}
 * --------------------------------------------------------------------- 
 */
int merge_checkpoint_2(char *src_ckpt, size_t src_size, char ckpt_flag){	
	
	FILE *tmp_stream;
	char *tmp_ckpt;
	size_t tmp_size;
	
	unsigned int src_pointer, tmp_pointer;
	
	if (src_size <= 4)
		return 0;
	
	if (after_ckpt_size == 0){	
		after_ckpt_stream = open_memstream(&after_ckpt, &after_ckpt_size);
		fwrite(src_ckpt, src_size, 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		return src_size;	
	}
	
	//Copy des_stream file to tmp_stream file
	tmp_stream = open_memstream(&tmp_ckpt, &tmp_size);
	fwrite(after_ckpt, after_ckpt_size, 1,tmp_stream);
	fflush(tmp_stream);
	
	//Close des_stream file
	fclose(after_ckpt_stream);
	free(after_ckpt);
	after_ckpt = NULL;
	after_ckpt_size = 0;
		
	src_pointer = 0;
 	tmp_pointer =0 ;
 	unsigned long t1, t2;
 	unsigned long size_s = 0, size_s1, size_s2;
  	
 	//1. Read timespan 	
// 	fseek(tmp_stream, tmp_pointer, SEEK_SET);
// 	fread(&t1, sizeof(unsigned long), 1, tmp_stream);
 	
 	t1 = *(unsigned long *)tmp_ckpt;
 	t2 = *(unsigned long *)src_ckpt; 
 	 	
 	tmp_pointer += sizeof(unsigned long);
 	src_pointer += sizeof(unsigned long);
 	
 	size_s1 =  *(unsigned long *)(tmp_ckpt + tmp_pointer);
 	size_s2 =  *(unsigned long *)(src_ckpt + src_pointer);
 	 	
 	src_pointer += sizeof(unsigned long);
 	tmp_pointer += sizeof(unsigned long);
	 	
 	//Open des_stream file again
 	after_ckpt_stream = open_memstream(&after_ckpt, &after_ckpt_size);
 	
 	
 	if (t1 >= t2){ 			//C <- t1, size_s, S2, S1		
		fwrite(&t1, sizeof(unsigned long), 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		fwrite(&size_s, sizeof(unsigned long), 1, after_ckpt_stream);
		fflush(after_ckpt_stream);		
		//write S2 to des_stream
		if (src_pointer < size_s2){
			fwrite(src_ckpt + src_pointer, size_s2 - src_pointer, 1, after_ckpt_stream);
			fflush(after_ckpt_stream);			
		}
		//Write S1 to des_stream
		if (tmp_pointer < size_s1){
			fwrite(tmp_ckpt + tmp_pointer, size_s1 - tmp_pointer, 1, after_ckpt_stream);
			fflush(after_ckpt_stream);			
		}	
		
	}else{	//C <-	t1, size_s, S1, S2		
		fwrite(&t2, sizeof(unsigned long), 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		fwrite(&size_s, sizeof(unsigned long), 1, after_ckpt_stream);
		fflush(after_ckpt_stream);		
		//Write S1 to des_stream
		if (tmp_pointer < size_s1){
			fwrite(tmp_ckpt + tmp_pointer, size_s1 - tmp_pointer, 1, after_ckpt_stream);
			fflush(after_ckpt_stream);			
		}
		//Write S2 to des_stream
		if (src_pointer < size_s2){
			fwrite(src_ckpt + src_pointer, size_s2 - src_pointer, 1, after_ckpt_stream);
			fflush(after_ckpt_stream);			
		}		
	}
	tmp_pointer = size_s1;
	src_pointer = size_s2;	
	size_s = after_ckpt_size;
	
	printf("____________Node %d - Merging: %d + %d - 8 = Final size %d \n", __node__,  size_s1, size_s2, size_s);
	//Check if exist reduction data
	if (ckpt_flag == EXIT_CHECKPOINT){ 
		while (	(tmp_pointer < tmp_size) && (src_pointer <src_size)){
			
			long addr;
			unsigned int len;
			
			addr = *(long *) (tmp_ckpt + tmp_pointer );
			src_pointer += sizeof(long);
			tmp_pointer += sizeof(long);

			len = *(unsigned int *) (tmp_ckpt + tmp_pointer) ;
			src_pointer += sizeof(long);
			tmp_pointer += sizeof(long);
			
			fwrite(&addr, sizeof(long), 1, after_ckpt_stream);
			fflush(after_ckpt_stream);
			fwrite(&len, sizeof(unsigned int), 1, after_ckpt_stream);
			fflush(after_ckpt_stream);
			
			printf("=== Node %d - addr 0x%lx - len %d \n", __node__,  addr, len);
			
			fwrite(src_ckpt + src_pointer, len, 1, after_ckpt_stream);
			fflush(after_ckpt_stream);
			
			VarList *var = NULL; 
			var = find_variable_by_addr(_var_list_head, addr , __ckpt_level__);	
			
//printf("============= Node %d - find(0x%lx) - found: 0x%lx \n", __node__,  addr, var->addr);	
/*
			switch (var->dtype){
				case CAPE_CHAR:
				case CAPE_INT:
				case CAPE_LONG:
					if (var->pro == CAPE_SUM){
						int num =  ( *(int*) (tmp_ckpt + tmp_pointer)) + ( *(int*) (src_ckpt + src_pointer));
						fwrite(&num, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}
					if (var->pro == CAPE_MUL){
						int num =  (*(int*) (tmp_ckpt + tmp_pointer)) * ( *(int*) (src_ckpt + src_pointer));				
						fwrite(&num, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}					
					if (var->pro == CAPE_MAX){						
						int num =  ((*(int*) (tmp_ckpt + tmp_pointer)) >= ( *(int*) (src_ckpt + src_pointer)) )
									?(*(int*) (tmp_ckpt + tmp_pointer)) : ( *(int*) (src_ckpt + src_pointer));						
						fwrite(&num, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}
					if (var->pro == CAPE_MIN){						
						int num =  ((*(int*) (tmp_ckpt + tmp_pointer)) < ( *(int*) (src_ckpt + src_pointer)) )
									?(*(int*) (tmp_ckpt + tmp_pointer)) : ( *(int*) (src_ckpt + src_pointer));					
						fwrite(&num, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}							
					break;
				case CAPE_UNSIGNED_CHAR:	
				case CAPE_UNSIGNED_INT:
				case CAPE_UNSIGNED_LONG:
					if (var->pro == CAPE_SUM){
						unsigned long num =  ( *(unsigned long*) (tmp_ckpt + tmp_pointer)) + ( *(unsigned long*) (src_ckpt + tmp_pointer));
						fwrite(&num, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}
					if (var->pro == CAPE_MUL){
						unsigned long num =  (*(unsigned long*) (tmp_ckpt + tmp_pointer)) * ( *(unsigned long*) (src_ckpt + tmp_pointer));				
						fwrite(&num, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}					
					if (var->pro == CAPE_MAX){						
						unsigned long num =  ((*(unsigned long*) (tmp_ckpt + tmp_pointer)) >= ( *(unsigned long*) (src_ckpt + src_pointer)) )
									?(*(unsigned long*) (tmp_ckpt + tmp_pointer)) : ( *(unsigned long*) (src_ckpt + src_pointer));						
						fwrite(&num, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}
					if (var->pro == CAPE_MIN){						
						unsigned long num =  ((*(unsigned long*) (tmp_ckpt + tmp_pointer)) < ( *(unsigned long*) (src_ckpt + src_pointer)) )
									?(*(unsigned long*) (tmp_ckpt + tmp_pointer)) : ( *(unsigned long*) (src_ckpt + src_pointer));					
						fwrite(&num, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}				
					break;
				case CAPE_FLOAT:
					if (var->pro == CAPE_SUM){
						float num =  ( *(float*) (tmp_ckpt + tmp_pointer)) + ( *(float*) (src_ckpt + src_pointer));						
						fwrite(&num, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);		
						
printf("_____Node %d:  %f = %f + %f \n", __node__, num, ( *(float*) (tmp_ckpt + tmp_pointer)), ( *(float*) (src_ckpt + src_pointer) )	);		
						break;						
					}
					if (var->pro == CAPE_MUL){
						float num =  ( *(float*) (tmp_ckpt + tmp_pointer)) + ( *(float*) (src_ckpt + src_pointer));				
						fwrite(&num, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}					
					if (var->pro == CAPE_MAX){						
						float num =  ((*(float*) (tmp_ckpt + tmp_pointer)) >= ( *(float*) (src_ckpt + src_pointer)) )
									?(*(float*) (tmp_ckpt + tmp_pointer)) : ( *(float*) (src_ckpt + src_pointer));						
						fwrite(&num, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}
					if (var->pro == CAPE_MIN){						
						float num =  ((*(float*) (tmp_ckpt + tmp_pointer)) >= ( *(float*) (src_ckpt + src_pointer)) )
									?(*(float*) (tmp_ckpt + tmp_pointer)) : ( *(float*) (src_ckpt + src_pointer));					
						fwrite(&num, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}				
					break;
				case CAPE_DOUBLE:
					if (var->pro == CAPE_SUM){
						double num =  ( *(double*) (tmp_ckpt + tmp_pointer)) + ( *(double*) (src_ckpt + src_pointer));
						fwrite(&num, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}
					if (var->pro == CAPE_MUL){
						double num =  (*(double*) (tmp_ckpt + tmp_pointer)) * ( *(double*) (src_ckpt + src_pointer));				
						fwrite(&num, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}					
					if (var->pro == CAPE_MAX){						
						double num =  ((*(double*) (tmp_ckpt + tmp_pointer)) >= ( *(double*) (src_ckpt + src_pointer)) )
									?(*(double*) (tmp_ckpt + tmp_pointer)) : ( *(double*) (src_ckpt + src_pointer));						
						fwrite(&num, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}
					if (var->pro == CAPE_MIN){						
						double num =  ((*(double*) (tmp_ckpt + tmp_pointer)) < ( *(double*) (src_ckpt + src_pointer)) )
									?(*(double*) (tmp_ckpt + tmp_pointer)) : ( *(double*) (src_ckpt + src_pointer));					
						fwrite(&num, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}
					break;
				default:
					printf("This datatype is not supported !!!!!");
			}
			*/
			src_pointer += 4;
			tmp_pointer += 4;
		}
		

	}	
	//memcpy(after_ckpt + sizeof(unsigned long), &size_s, sizeof(unsigned long) );	
	
	
	
	//Modify size_s in checkpoint file	
	unsigned long save_file_pointer = after_ckpt_size;
	fseek(after_ckpt_stream, sizeof(unsigned long), SEEK_SET);
	fwrite(&size_s, sizeof(unsigned long), 1, after_ckpt_stream);
	fflush(after_ckpt_stream);	
	
	fseek(after_ckpt_stream, save_file_pointer, SEEK_SET);
	fflush(after_ckpt_stream);
	
	fclose(tmp_stream);
	free(tmp_ckpt);	
	tmp_ckpt = NULL;
	
	return src_size;	
}

/*----------------------------------------------------------------------
 * log2(n): calculate log2 of n
 *----------------------------------------------------------------------
 */
int mylog2(unsigned int n){
	int p = 0;
	int tmp;
	tmp = n;
	while(tmp >>=1) ++p;
	return p;	
}
/*----------------------------------------------------------------------
 * Check a number is power of 2 or not
 *---------------------------------------------------------------------
 */
int is_power_of_two(unsigned int n)
{
  if (n == 0) return 0;
  while (n != 1){
    if (n%2 != 0) return 0;
    n = n/2;
  }
  return 1;
}

/*----------------------------------------------------------------------
 * find the number lower than n, but it is nearest number that powerof 2
 * ---------------------------------------------------------------------
 */
unsigned int nearest_power_of_two(unsigned int n){
	
	if (is_power_of_two(n)) return n;
	while(n > 1){
		n--;
		if (is_power_of_two(n)) return n;
	}
	return 0;
}

/*
 *---------------------------------------------------------------------
 * Send and Receive checkpoint based on hypercube algorithm  
 *----------------------------------------------------------------------
 */
int hypercube_allreduce(unsigned int node, unsigned int num_nodes, char ckpt_flag){
	int i, nsteps = 0;
	unsigned int partner;
	unsigned long send_msg_size=0, recv_msg_size = 0;
	char *send_msg;
	char *recv_msg;
	MPI_Status status;
	
	nsteps = mylog2(num_nodes);
	
	send_msg = after_ckpt;
	send_msg_size = after_ckpt_size;	
	
	
	for(i = 0; i< nsteps ; i++){
		partner = node ^ (1 << i);
		
		
		MPI_Sendrecv(&send_msg_size, 1, MPI_UNSIGNED_LONG, partner, i,	\
					&recv_msg_size, 1, MPI_UNSIGNED_LONG, partner, i,	\
					MPI_COMM_WORLD, &status);
	
		
		recv_msg = malloc(sizeof(char) * recv_msg_size);
		
		MPI_Sendrecv(send_msg, send_msg_size, MPI_CHAR, partner, i,	\
					recv_msg, recv_msg_size, MPI_CHAR, partner, i, \
					MPI_COMM_WORLD, &status);
				
		merge_checkpoint(recv_msg, recv_msg_size, ckpt_flag);	
		
		free(recv_msg);
		recv_msg_size = 0;
		
		send_msg = after_ckpt;
		send_msg_size = after_ckpt_size;			
	
	}
	
	//printf("Node %d: After merge: %ld bytes \n",__node__, after_ckpt_size);
	
	return 1;
}

/*
 *---------------------------------------------------------------------
 * Send and Receive checkpoint based on Ring algorithm  
 *----------------------------------------------------------------------
 */
int ring_allreduce(unsigned int node, unsigned int nnodes, char ckpt_flag){

	char *send_msg;
	char *recv_msg;
	unsigned int send_msg_size, recv_msg_size;
	MPI_Status status;
	
	unsigned int i, left, right;
	left = (node - 1 + nnodes) % nnodes;
	right = (node + 1) % nnodes;
	
	send_msg = after_ckpt;
	send_msg_size = after_ckpt_size;
	
	for(i = 1 ; i < nnodes; i ++){		
		MPI_Sendrecv(&send_msg_size, 1, MPI_INT, right, i, 		\
						&recv_msg_size, 1, MPI_INT, left, i, 	\ 
						MPI_COMM_WORLD, &status) ;
		
		recv_msg = malloc(sizeof(char) * recv_msg_size );
		
		MPI_Sendrecv(send_msg, send_msg_size, MPI_CHAR, right, i,		\
						recv_msg, recv_msg_size, MPI_CHAR, left, i,		\
						MPI_COMM_WORLD, &status);
		
		merge_checkpoint(recv_msg, recv_msg_size, ckpt_flag);
		
		send_msg = recv_msg;
		send_msg_size = recv_msg_size;
		
		recv_msg = NULL;
		recv_msg_size = 0;				
	}
	return 1;
}

/*
 * ---------------------------------------------------------------------
 * Inject checkpoint into application's memory
 * Parameters: checkpoint file
 * ---------------------------------------------------------------------
 */
int inject_checkpoint(char *data_ckpt, size_t file_size){
	unsigned long len=0, file_pointer = 0;
	long addr;
	
	if (file_size <= sizeof(unsigned long)) return 0;	
	file_pointer = sizeof(unsigned long) * 2;
	
	
	
	while(file_pointer < file_size){
		
		addr = *(long *) (data_ckpt + file_pointer);		
		file_pointer += sizeof(long);		
		len = *(unsigned int*) (data_ckpt + file_pointer );
		file_pointer += sizeof(long);			
		memcpy(addr, data_ckpt+file_pointer, len)	;		
		
		//if (__node__ == 0)
		//	printf("DATA: Node %d: addr = 0x%lx - len = %d bytes - data %d \n",__node__, addr, len, *(int *) (data_ckpt+file_pointer) );	
				
		file_pointer += len ;			
	}
	
	return 1;
}

/*
 * ---------------------------------------------------------------------
 * Require synchronize checkpoints
 * ---------------------------------------------------------------------
 */

int require_allreduce(char ckpt_flag){
	
//	printf("Node %d:  nnodes = %d - is_power_of2: %d \n", __node__, __nnodes__, is_power_of_two(__nnodes__));
	
	if (after_ckpt_size == 0) return 0 ;	
	if (is_power_of_two(__nnodes__))
		hypercube_allreduce(__node__, __nnodes__, ckpt_flag);
	else
		ring_allreduce(__node__, __nnodes__, ckpt_flag);
	
}


/*
 * ---------------------------------------------------------------------
 * Open the window of variables
 * ---------------------------------------------------------------------
 */
void open_checkpoint_window(){	
	__ckpt_level__ ++;
	add_var_level(&_var_list_head,&_var_list_tail, __ckpt_level__);	
}

/*
 * ---------------------------------------------------------------------
 * Close the window of variables
 * Release checkpoint memory
 * ---------------------------------------------------------------------
 */
void close_checkpoint_window(){
	
	if (__ckpt_level__ > 0) {
		remove_var_level(&_var_list_head, &_var_list_tail,__ckpt_level__);
		__ckpt_level__--;
	}
}

void release_checkpoint(){
	
//	if (after_ckpt_stream != NULL){
//		fclose(after_ckpt_stream);
//	}
	free(after_ckpt);	
	after_ckpt = NULL;
	after_ckpt_size = 0;
}



/*********** Some debug functions **************************************/
int print_checkpoint_data(char *data_ckpt, size_t file_size){
	unsigned long len=0, file_pointer = 0;
	long addr;
	unsigned int t, size_s;
	
	if (file_size <= sizeof(unsigned long)) return 0;	
	
	t= *(unsigned int *) data_ckpt;
	file_pointer += sizeof(unsigned int);
	size_s = *(unsigned int *) (data_ckpt + file_pointer); 
	file_pointer += sizeof(unsigned int);
	
	printf("CHECKPOINT DATA: Node %d: t= %d - size_s = %d - size = %d \n",__node__, t, size_s, file_size);	
	while(file_pointer < file_size){
		
		addr = *(long *) (data_ckpt + file_pointer);		
		file_pointer += sizeof(long);		
		len = *(unsigned int*) (data_ckpt + file_pointer );
		file_pointer += sizeof(long);			
		//if (__node__ == 0)
			printf("CHECKPOINT DATA: Node %d: addr = 0x%lx - len = %d bytes - data %lf \n",__node__, addr, len, *(double*) (data_ckpt+file_pointer) );	
				
		file_pointer += len ;			
	}
	
	return 1;
}
int map_pages(PageList *plist){
	if (plist == NULL) return 0;
	
		
	while(plist!=NULL){
			unsigned long *map = mmap(NULL, 	\
							CAPE_PAGE_SIZE,	\
							PROT_WRITE,	\
							MAP_PRIVATE,	\
							-1, 0	);
						
			plist = plist->next;			
			printf("Page Ox%lx is mapped \n", *map);
	}

}
int unmap_pages(PageList *plist){
	if (plist == NULL) return 0;	
	while(plist!=NULL){
			void *map = munmap(plist->addr, 	\
							CAPE_PAGE_SIZE);						
			plist = plist->next;
	}

}
void print_pages_list(PageList *plist)
{
	int tmp1, tmp2=-1;
	if(plist != NULL){
		while(plist!=NULL){			
			printf("Page Ox%lx - Write Protected = %d contains variables \n", plist->addr, plist->wp_flag );	
		//	memcpy(&tmp1,plist->page->addr, 4); //allocate page in virtual memory
		//	memcpy(plist->page->addr, &tmp2, 4);
		//	memcpy(plist->page->addr, &tmp1, 4);		
			plist = plist->next;
		}
	}
	else
		printf("PageList is NULL \n");
}
void print_var_list(VarList *vlist)
{
	if(vlist != NULL){
		while(vlist!=NULL){
			printf("Variables: Ox%lx - size: %u - n: %u- pro: %d - dtype: %d - level: %d  \n ", 	 \
					vlist->addr, vlist->size, vlist->n, vlist->pro, vlist->dtype, vlist->level);
			vlist = vlist->next;
		}
	}
	else
		printf("VarList is NULL \n");
}
void print_dpage_list(DPageList *dplist){
	int count = 0;
	if(dplist != NULL){
		while(dplist!=NULL){
			count++;
			dplist = dplist->next;
		}		
		printf("NUMBER OF PAGES CONTAINT DATA: %d \n", count);
	}
	else
		printf("DPageList is NULL \n");
}

/***********************************************************************/
/* Runtime functions												   */
/***********************************************************************/
void cape_set_num_nodes(int nnodes){
	__nnodes__ = nnodes ;
}

void cape_set_time_stamp(int time_stamp){
	__time_stamp__ = time_stamp;
}

int cape_get_node_num(){
	return __node__;
}

int cape_get_num_nodes(){
	return __nnodes__;
}

int cape_get_left(int n, int start){
	int left = __node__ * (n - start) /__nnodes__ + start;
	return left ;
}

int cape_get_right(int n, int start){
	int right = (__node__ + 1) * (n - start) / __nnodes__ + start;	
	return right;
}

int cape_get_token(){
	return __cape_token__;
}

int cape_section(){
	__current_session__ ++ ; //declere section
	if (__current_session__ % __nnodes__ == __node__)
		return 1;
	else
		return 0;	
}



/***********************************************************************/
/*		Clauses and data sharing directives							   */
/***********************************************************************/
void cape_set_default_none(){
	set_default_none(_var_list_tail, __ckpt_level__);
}
void cape_set_threadprivate(long addr){
	set_threadprivate(_var_list_head, addr);
}

void cape_set_shared(long addr){
	set_data_attribute(_var_list_tail, addr, CAPE_SHARED, __ckpt_level__);
}

void cape_set_private(long addr){
	set_data_attribute(_var_list_tail, addr, CAPE_PRIVATE, __ckpt_level__);
}

void cape_set_firstprivate(long addr){
	set_data_attribute(_var_list_tail, addr, CAPE_FIRST_PRIVATE, __ckpt_level__);
}
    
void cape_set_lastprivate(long addr){
	set_data_attribute(_var_list_tail, addr, CAPE_LAST_PRIVATE, __ckpt_level__);
}

void cape_set_reduction(long addr, char op){
	set_data_attribute(_var_list_tail, addr, op , __ckpt_level__);
}

void cape_set_copyin(long addr){
	set_data_attribute(_var_list_tail, addr, CAPE_COPY_IN, __ckpt_level__);

}

void cape_set_copythread(long addr){
	set_data_attribute(_var_list_tail, addr, CAPE_COPY_PRIVATE, __ckpt_level__);
}



/**********************************************************************/
/* 					Public Functions 								 */
/*********************************************************************/
/*
 * ---------------------------------------------------------------------
 * Catch SIGSEGV signal -> remove write protected and save old data
 * ---------------------------------------------------------------------
 */
void CAPE_Sigsegv_Handler(int sig, siginfo_t *si, void *unused)
{
   int ret_val = 0; 
   int i;  
   dprintf("Got SIGSEGV at address: 0x%lx\n", (long) si->si_addr);      
   unsigned long addr;
   addr = (unsigned long) si->si_addr ; 
      
   remove_page_write_protected(si->si_addr);  
   update_dpage_list(&_dpage_list_head, addr);    
   
 }
 
/*
 * ---------------------------------------------------------------------
 * TODO: Initialize 
 * ---------------------------------------------------------------------
 */
void CAPE_Init(){	
	MPI_Init(NULL,NULL);
	MPI_Comm_rank(MPI_COMM_WORLD, &__node__);
	MPI_Comm_size(MPI_COMM_WORLD, &__nnodes__);		
		
	sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = CAPE_Sigsegv_Handler;
    if (sigaction(SIGSEGV, &sa, NULL) == -1)
		handle_error("sigaction");	

}
/*
 * ---------------------------------------------------------------------
 * TODO: Release the initialization envrironment
 * ---------------------------------------------------------------------
 */
void CAPE_Finalize(){	
	 MPI_Finalize();	
	
}

/*
 * ---------------------------------------------------------------------
 * Set barrier
 * ---------------------------------------------------------------------
 */
void CAPE_Flush(){
	__time_stamp__ = __node__;
	CAPE_Stop();
	require_generate_checkpoint(FALSE);
	require_allreduce(EXIT_CHECKPOINT);			
	inject_checkpoint(after_ckpt, after_ckpt_size);
	release_checkpoint();
	CAPE_Start();	
}

/*
 * ---------------------------------------------------------------------
 * Set barrier
 * ---------------------------------------------------------------------
 */
void CAPE_Barrier(){
	CAPE_Flush();
}
/*
 * ---------------------------------------------------------------------
 * Save variables and pages that contains variables
 * ---------------------------------------------------------------------
 */
void CAPE_Declare_Variable(unsigned long addr, unsigned char dtype, unsigned int n_elements){
	unsigned long page_begin_addr, end_addr; //address of pages that contain variables 
	int rel;
	Page _page; 
	Var _v;
	int size, _size = 0;
	int _n=0;
	int _addr = 0;	
		
	switch (dtype){
		case CAPE_CHAR:
		case CAPE_UNSIGNED_CHAR:
			size = 1;
			break;
		case CAPE_INT:
		case CAPE_UNSIGNED_INT:
		case CAPE_LONG:
		case CAPE_UNSIGNED_LONG:
		case CAPE_FLOAT:
			size = 4 ;
			break;
		case CAPE_DOUBLE:
			size = 8 ;
			break;
		default:
			size = 4;
	}
	if(size < CAPE_WORD){
		_size = CAPE_WORD;
		_addr = addr & ~(CAPE_WORD - 1);
		if (n_elements % CAPE_WORD==0) 
			_n = n_elements / CAPE_WORD;
		else
			_n = n_elements / CAPE_WORD + 1;
	}else {		
		_size = size;
		_n = n_elements;
		_addr = addr ;
	}		
	_v.addr = _addr ;
	_v.size = _size ;
	_v.pro = CAPE_SHARED;
	_v.n = _n;
	_v.level = 0;
	_v.dtype = dtype;
	rel = update_var_list(&_var_list_head,&_var_list_tail, _v);
	
	if (rel == 1){
		page_begin_addr = _addr & ~(CAPE_PAGE_SIZE -1);
		end_addr = (_addr + _size*_n) ;		
		while(page_begin_addr < end_addr){			
			_page.addr = page_begin_addr;
			_page.wp_flag = PAGE_WRITABLE;				
			add_page_list(&_page_list_head, _page);
			page_begin_addr += CAPE_PAGE_SIZE;			
		}	
	}
}
/*
 * ---------------------------------------------------------------------
 * Setup variables environments for a block
 * ---------------------------------------------------------------------
 */
void CAPE_Begin(unsigned char name_directive, long first, long second){

	switch(name_directive){
		case PARALLEL:
			open_checkpoint_window();
			break;
		case FOR:
		case FOR_NOWAIT:
			__left__ = cape_get_left(second, first);
			__right__= cape_get_right(second, first);
			break;
		case PARALLEL_FOR:
			open_checkpoint_window();
			__left__ = cape_get_left(second, first);
			__right__= cape_get_right(second, first);
			break;
		case CRISTIAL:			
			break;
		case SECTIONS_NOWAIT:
			__current_session__ = -1 ;
			break;
		default:
			break;
	}
	
	CAPE_Start();	
}

/*
 * ---------------------------------------------------------------------
 * Release variables environments of a block
 * ---------------------------------------------------------------------
 */
void CAPE_End(unsigned char name_directive, long time_stamp, unsigned char ops_flag){	
	switch(name_directive){
		case CRISTIAL:
		case FOR:
			__time_stamp__ = time_stamp;
			CAPE_Stop();
			require_generate_checkpoint(ops_flag);
			require_allreduce(EXIT_CHECKPOINT);			
			inject_checkpoint(after_ckpt, after_ckpt_size);
			release_checkpoint();
			CAPE_Start();
			break;
		case FOR_NOWAIT:
		case SECTIONS_NOWAIT: 
			break;		
		case PARALLEL:
		case PARALLEL_FOR:
			__time_stamp__ = time_stamp;
			CAPE_Stop();
			require_generate_checkpoint(ops_flag);
			require_allreduce(EXIT_CHECKPOINT);			
			inject_checkpoint(after_ckpt, after_ckpt_size);
			release_checkpoint();
			close_checkpoint_window();
			break;
		
	}			

}
/*
 * --------------------------------------------------------------------
 * TODO: Set write protected to all pages in PageList
 * ---------------------------------------------------------------------
 */
void CAPE_Start(){
	set_write_protected(_page_list_head);	
	//copy_dpage(&_dpage_list_head, _page_list_head);
}
/*
 * ---------------------------------------------------------------------
 *TODO: Remove write protected and syncronization data
 * ---------------------------------------------------------------------
 */
void CAPE_Stop(){		
	remove_write_protected(_page_list_head);
}

void require_generate_checkpoint(char ops_flag){
		if(_dpage_list_head != NULL){		
		after_ckpt_stream = generate_checkpoint(_dpage_list_head,	\
								_var_list_head, 
								__ckpt_level__,		\
								EXIT_CHECKPOINT,				\
								ops_flag,						\
								__time_stamp__); 	  
		clear_dpage_list(&_dpage_list_head);	
		
	}
}



void CAPE_DEBUG()
{	
	print_pages_list(_page_list_head);
	print_dpage_list(_dpage_list_head);
	print_var_list(_var_list_head);	
}


