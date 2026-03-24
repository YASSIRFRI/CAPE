#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "../../include/cape.h"
#include <ucp/api/ucp.h>
#ifdef USE_PMIX
#include <pmix.h>
#endif
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

#if DDEBUG
#define dprintf(fmt, args...) printf(fmt, ## args);
#else
#define dprintf(fmt, args...) ;
#endif
/**********Local Variables ********************************************/        
static char *_var_data; //copy variable data
static unsigned long * __var_addr; //address of variables
static unsigned long * __var_len; //
static FILE * __ckpt_data;


PointerList *__var_heap_list_head = NULL;
PointerList *__var_heap_list_tail = NULL;

VarList *__active_variable_head = NULL;
VarList *__active_variable_tail = NULL;
VarList *__var_list_head = NULL;
VarList *__var_list_tail = NULL;

FILE *ckpt_data_stream;
FILE *before_ckpt_stream, *after_ckpt_stream, *final_ckpt_stream;
char *ckpt_data, *before_ckpt, *after_ckpt, *final_ckpt;
size_t ckpt_data_size, before_ckpt_size, after_ckpt_size, final_ckpt_size;

static unsigned char __activate_func_level__ = 1;
static unsigned char __parallel_level__ = 0;
static int __node__ = -1; //current node
static int __nnodes__ = -1 ; // Number of working nodes
static int __total_nodes__ = -1 ; //Total nodes in the system
static int __cape_token__ = -1;
static unsigned int __current_session__ = -1;
static uint32_t __allreduce_epoch__ = 0;

static __is_inside_parallel_region__ = 0;

char buffer[4096];
char __ckpt_data_file[100];
int __ckpt_data_size = 0;

/**********UCX + PMIx State ********************************************/
#ifdef USE_PMIX
static pmix_proc_t   pmix_myproc;
#endif
static ucp_context_h ucp_context;
static ucp_worker_h  ucp_worker;
static ucp_ep_h     *ucp_endpoints;  /* one per peer process */

/* Per-request state allocated by UCX in the request headroom */
typedef struct {
    volatile int completed;
    ucs_status_t status;
    size_t       recv_len;
    ucp_tag_t    sender_tag;   /* actual tag of matched message */
} cape_ucx_req_t;

static void cape_ucx_req_init(void *request)
{
    cape_ucx_req_t *r = (cape_ucx_req_t *)request;
    r->completed  = 0;
    r->status     = UCS_OK;
    r->recv_len   = 0;
    r->sender_tag = 0;
}

static void cape_send_cb(void *request, ucs_status_t status, void *user_data)
{
    cape_ucx_req_t *r = (cape_ucx_req_t *)request;
    r->status    = status;
    r->completed = 1;
}

static void cape_recv_cb(void *request, ucs_status_t status,
                         const ucp_tag_recv_info_t *info, void *user_data)
{
    cape_ucx_req_t *r = (cape_ucx_req_t *)request;
    r->status    = status;
    if (info != NULL) {
        r->recv_len   = info->length;
        r->sender_tag = info->sender_tag;
    }
    r->completed = 1;
}

/* Block until a non-blocking UCX request finishes.
 * out_tag: if non-NULL, receives the sender_tag from the matched message. */
static void cape_ucx_wait(void *req, size_t expect_len, int check_len,
                          ucp_tag_t *out_tag)
{
    if (out_tag) *out_tag = 0;
    if (req == NULL)
        return; /* completed immediately */
    if (UCS_PTR_IS_ERR(req)) {
        fprintf(stderr, "CAPE UCX error: %s\n",
                ucs_status_string(UCS_PTR_STATUS(req)));
        exit(1);
    }
	cape_ucx_req_t *r = (cape_ucx_req_t *)req;
	while (!r->completed)
		ucp_worker_progress(ucp_worker);
	if (out_tag)
		*out_tag = r->sender_tag;
	if (r->status != UCS_OK) {
		fprintf(stderr, "CAPE UCX request failed: %s\n",
		        ucs_status_string(r->status));
		ucp_request_free(req);
		exit(1);
	}
	if (check_len && (r->recv_len != 0) && (r->recv_len != expect_len)) {
		fprintf(stderr, "CAPE UCX recv length mismatch: got=%zu expected=%zu"
		        " sender_tag=0x%lx\n",
		        r->recv_len, expect_len,
		        (unsigned long)r->sender_tag);
		/* Reset before freeing so reuse from pool starts clean */
		r->completed  = 0;
		r->status     = UCS_OK;
		r->recv_len   = 0;
		r->sender_tag = 0;
		ucp_request_free(req);
		exit(1);
	}
	/* Reset before freeing so reuse from pool starts clean.
	 * UCX's request_init may only be called on first allocation,
	 * not on reuse from the free pool. */
	r->completed  = 0;
	r->status     = UCS_OK;
	r->recv_len   = 0;
	r->sender_tag = 0;
	ucp_request_free(req);
}

/*
 * Simultaneous tag send/receive (mimics MPI_Sendrecv).
 * The UCX tag encodes the sender rank in the upper 32 bits so that a
 * receiver matches only messages from the expected peer.
 */
/*
 * Keep matching bits in the low portion of the tag, which is the safest
 * subset across transports that may not preserve/compare all high tag bits.
 */
#define CAPE_UCX_TAG(sender_rank, token) \
	((uint64_t)((((uint32_t)(sender_rank) & 0x0fffU) << 20) | ((uint32_t)(token) & 0x000fffffU)))
#define CAPE_UCX_TAG_MASK  ((uint64_t)0x00000000ffffffffULL)

static void cape_ucx_sendrecv(
        const void *sendbuf, size_t sendlen, int dest,
        void       *recvbuf, size_t recvlen, int src,
        uint32_t    token)
{
    ucp_tag_t send_tag = CAPE_UCX_TAG(__node__, token);
    ucp_tag_t recv_tag = CAPE_UCX_TAG(src,      token);

    ucp_request_param_t sp = {
        .op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK,
        .cb.send      = cape_send_cb
    };
    ucp_request_param_t rp = {
        .op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK
                      | UCP_OP_ATTR_FIELD_FLAGS,
        .flags        = UCP_OP_ATTR_FLAG_NO_IMM_CMPL,
        .cb.recv      = cape_recv_cb
    };

    // fprintf(stderr, "CAPE DBG node %d: sendrecv send_tag=0x%lx recv_tag=0x%lx"
    //         " sendlen=%zu recvlen=%zu dest=%d src=%d token=0x%x\n",
    //         __node__, (unsigned long)send_tag, (unsigned long)recv_tag,
    //         sendlen, recvlen, dest, src, (unsigned)token);

    /* Post receive before send to avoid lost-message races */
    void *rreq = ucp_tag_recv_nbx(ucp_worker, recvbuf, recvlen,
                                   recv_tag, CAPE_UCX_TAG_MASK, &rp);
    void *sreq = ucp_tag_send_nbx(ucp_endpoints[dest], sendbuf, sendlen,
                                   send_tag, &sp);

    // fprintf(stderr, "CAPE DBG node %d: rreq=%p sreq=%p\n",
    //         __node__, rreq, sreq);

    ucp_tag_t matched_tag = 0;
    cape_ucx_wait(rreq, recvlen, 1, &matched_tag);

    /* Hex dump first 48 bytes of received data */
    // {
    //     size_t dumplen = recvlen < 48 ? recvlen : 48;
    //     fprintf(stderr, "CAPE DBG node %d: recv buf [%zu bytes] matched_tag=0x%lx:",
    //             __node__, recvlen, (unsigned long)matched_tag);
    //     for (size_t di = 0; di < dumplen; di++)
    //         fprintf(stderr, " %02x", (unsigned char)((char *)recvbuf)[di]);
    //     fprintf(stderr, "\n");
    // }
    // /* Also dump what we SENT for comparison */
    // {
    //     size_t dumplen = sendlen < 48 ? sendlen : 48;
    //     fprintf(stderr, "CAPE DBG node %d: send buf [%zu bytes]:",
    //             __node__, sendlen);
    //     for (size_t di = 0; di < dumplen; di++)
    //         fprintf(stderr, " %02x", (unsigned char)((const char *)sendbuf)[di]);
    //     fprintf(stderr, "\n");
    // }

    cape_ucx_wait(sreq, 0, 0, NULL);
}
/***********************************************************************/

/**********Public Variables ********************************************/
int __left__ = -1;
int __right__ = -1;
int __i__;
unsigned long __time_stamp__ = 1 ;
unsigned long __pc__;
/***********************************************************************/


/**********************************************************************/
/* 					Private Functions								 */
/*********************************************************************/
 /*
  * Add active variables in __active_variable list
  * 	Insert at the end of list (LIFO)
  */
int add_active_variable(VarList **vlist_head, VarList **vlist_tail, Var v){	
	VarList *vl;	
	vl = malloc(sizeof(struct VarList));
	vl->next = NULL;
	vl->prev = NULL;
	vl->var.addr = v.addr;
	vl->var.size = v.size;
	vl->var.n = v.n;
	vl->var.pro = v.pro;
	vl->var.level = v.level;
	vl->var.dtype = v.dtype;
	vl->var.ispointer = v.ispointer;	
	if (*vlist_head == NULL){
		*vlist_head = vl ;
		*vlist_tail = vl;
		return 1;
	}	
	VarList *tmp2;
	tmp2 = *vlist_tail;	
	//if variable exists in list
	if (tmp2->var.addr == vl->var.addr){
		free(vl);
		return 0;
	}
	//insert at the end of list
	vl->prev = tmp2;
	tmp2->next = vl;
	*vlist_tail = vl;
	return 1;	
}
/*
 * Remove all variables with in var.level =  func_level from activate list
 * and remove all variables on heap that is managered by these variables
 */
int remove_active_variables(VarList **vlist_head, VarList **vlist_tail, unsigned char func_level){
	
	if (*vlist_head == NULL) 
		return 0 ;
	
	VarList *tmp1, *tmp2;
	tmp1 = *vlist_head;
	tmp2 = *vlist_tail;	
	while((tmp2 != tmp1) && (tmp2->var.level == func_level)){
		VarList *tmp;
		tmp = tmp2;		
		tmp2= tmp2->prev;
		tmp2->next = NULL;
		*vlist_tail = tmp2;
		if(tmp->var.ispointer)
			remove_heap_variables(&__var_heap_list_head,
								&__var_heap_list_tail,
								tmp->var.addr);
		free(tmp);
	}	
	if ((tmp1 == tmp2) && (tmp1->var.level == func_level)){
		*vlist_head = NULL;
		*vlist_tail = NULL;
		tmp2 = NULL;
		if(tmp1->var.ispointer)
			remove_heap_variables(&__var_heap_list_head,
								&__var_heap_list_tail,
								tmp1->var.addr);
		free(tmp1);
		return 0;
	}
	return 1;
}
/*
 * Generate variables list and their parallel level for parallel windows
 * Read variables form active variables list, group each variables is 4 bytes
 * If (list !EXISTS) create_new
 * else
 * 		copy_variable_with_new_level_of_parallel_region
 */
int generate_variable_list_for_parallel_windows(VarList *active_variable_head){
	unsigned int size, nelements;
	unsigned long addr;
	if (__var_list_head == NULL){
		if (active_variable_head == NULL) return 0;
		VarList *tmp;
		tmp = active_variable_head;
		while(tmp!=NULL){
			Var var;		
			var.dtype = tmp->var.dtype;
			var.level = __parallel_level__;
			var.ispointer = tmp->var.ispointer;
			var.pro = tmp->var.pro;	

			///TODO: COPY and ADD variables in to new list (note group of 4 bytes)			
			if(tmp->var.size < CAPE_WORD){
				size = CAPE_WORD;
				addr = tmp->var.addr & ~(CAPE_WORD - 1);
				if (tmp->var.n % CAPE_WORD==0) 
					nelements = tmp->var.n / CAPE_WORD;
				else
					nelements = tmp->var.n / CAPE_WORD + 1;
			}else {		
				size = tmp->var.size;
				nelements = tmp->var.n;
				addr = tmp->var.addr ;
			}
			var.n = nelements ;
			var.addr = addr ;
			var.size = size;			
			//add var into new list
			add_shared_variable(&__var_list_head, &__var_list_tail, var);	
	
			tmp = tmp->next;
		}
	}
	else{
		__parallel_level__++;
		///TODO: Implement multi-level parallel region
		return 0;
	}	
}

/*
 * add shared variable in to __var_list_head
 */
int add_shared_variable(VarList **vlist, VarList **vlist_tail, Var var){
	VarList *vl;
	if (var.addr <= 0) return 0;		
	vl = malloc(sizeof(struct VarList));
	vl->next = NULL;
	vl->prev = NULL;
	vl->var.addr = var.addr ;
	vl->var.size = var.size ;
	vl->var.n = var.n;
	vl->var.pro = var.pro;
	vl->var.level=var.level;
	vl->var.dtype =var.dtype;	
	vl->var.ispointer = var.ispointer ;
	if (*vlist == NULL ) {
		*vlist = vl;
		*vlist_tail = vl;
		return 1;
	}
		
	VarList *tmp,*tmp2;
	tmp = *vlist;
	tmp2 = *vlist_tail;
	//Insert at the begin of list
	if (tmp->var.addr > vl->var.addr) {
		tmp->prev = vl;
		vl->next = tmp;
		*vlist = vl ;
		return 1;
	}	
	//Mofify the variable properties
	if (tmp->var.addr == vl->var.addr) {		
		free(vl);
		return 2;
	}	
	//Insert at the end of list
	if(tmp2->var.addr < vl->var.addr){
		tmp2->next = vl;
		vl->prev = tmp2;
		*vlist_tail = vl;
		return 1;
	}
	//Insert at the end of list
	if(tmp2->var.addr == vl->var.addr){
		free(vl);
		return 2;
	}
	
	//Find the position to insert or modify
	while ((tmp->next != NULL) && (tmp->var.addr < vl->var.addr )){
		tmp = tmp->next;
	}	
	//Insert before tmp
	if (tmp->var.addr > vl->var.addr){
		vl->next = tmp;
		vl->prev = tmp->prev;
		tmp->prev->next = vl;
		tmp->prev = vl ;
		return 1;
	}
	//Exist in list
	if (tmp->var.addr == vl->var.addr){
		free(vl);
		return 2;
	}	
} 

/*
 * --------------------------------------------------------------------
 * Add new pointer into heap list
 * 	manager_addr is not exists in heaplist
 * 	[addr, addr+len] is not exists in heaplist
 * --------------------------------------------------------------------
 */
int addnew_pointer(PointerList **hlist_head, 
		PointerList **hlist_tail, 
		Pointer pt){
	
	PointerList *item;
	item = malloc(sizeof(PointerList));
	item->pointer.manager_addr = pt.manager_addr;
	item->pointer.addr = pt.addr;
	item->pointer.len = pt.len ;
	item->next = NULL;
	item->prev = NULL;
	
	if (*hlist_head == NULL){
		*hlist_head = item ;
		*hlist_tail = item ;
		return 1 ;
	}
	
	PointerList *tmp_head, *tmp_tail;
	tmp_head = *hlist_head;
	tmp_tail = *hlist_tail;
	//insert at the begin of list
	if (tmp_head->pointer.addr < item->pointer.addr){
		item->next = tmp_head;
		tmp_head->prev = item;
		*hlist_head = item;
		return 1;
	}
	//insert at the end of list
	if (tmp_tail->pointer.addr > item->pointer.addr){
		tmp_tail->next = item ;
		item->prev = tmp_tail ;
		*hlist_tail = item ;
		return 1;
	}
	
	//find location to insert
	while((tmp_head !=NULL) && (tmp_head->pointer.addr < item->pointer.addr))
		tmp_head = tmp_head->next;
	
	//insert before tmp_head
	item->next = tmp_head;
	item->prev = tmp_head->prev;
	tmp_head->prev->next = item ;
	tmp_head->prev = item;
	return 1;	
}
/*
 * --------------------------------------------------------------------
 * Remove head variables
 * Parameters:
 * 	manager address
 * Return
 * 	Head list pointers, that remove the item managered by manager_addr
 * -------------------------------------------------------------------
 */

int remove_heap_variables(PointerList **hlist_head, 
		PointerList **hlist_tail,
		unsigned long manager_addr){	

	if (*hlist_head == NULL) return 0;
	
	PointerList *tmp_head;
	PointerList *tmp_tail;
	PointerList *tmp;
	
	tmp_head = *hlist_head;
	tmp_tail = *hlist_tail;
	
	if ((tmp_head == tmp_tail) && (tmp_head->pointer.manager_addr == manager_addr)){
		*hlist_head = NULL;
		*hlist_tail = NULL;
		free(tmp_head);
		return 1;
	}	
	tmp= tmp_head;
	while(tmp != NULL){
		if ((tmp->pointer.manager_addr == manager_addr) && (tmp==tmp_head)){
			tmp_head = tmp->next;
			tmp->next = NULL;
			tmp_head->prev = NULL;
			free(tmp);
			*hlist_head = tmp_head;
			return 1;
		}
		if ((tmp->pointer.manager_addr == manager_addr) && (tmp==tmp_tail)){
			tmp_tail = tmp->prev;
			tmp->prev = NULL;
			tmp_tail->next = NULL;
			free(tmp);
			*hlist_tail = tmp_tail;
			return 1;
		}
		if (tmp->pointer.manager_addr == manager_addr){
			tmp->prev->next = tmp->next;
			tmp->next->prev = tmp->prev;
			tmp->prev = NULL;
			tmp->next = NULL;
			free(tmp);
			return 2;
		}		
		tmp = tmp->next;
	}
	
	
}

/*
 * ---------------------------------------------------------------------
 * Remove item contain manager_addr or addr in heaplist
 * ---------------------------------------------------------------------
 */
int remove_exists_item(PointerList **hlist_head, 
		PointerList **hlist_tail, 
		Pointer pt){	
	if (*hlist_head == NULL) return 0;
	PointerList *tmp_head;
	PointerList *tmp_tail;
	PointerList *tmp;
	
	tmp_head = * hlist_head;
	tmp_tail = * hlist_tail;
	
	while (tmp_head != *hlist_tail){
		if ((tmp_head->pointer.manager_addr == pt.manager_addr) ||
				((tmp_head->pointer.addr >=pt.addr) 
						&& (tmp_head->pointer.addr <= pt.addr + pt.len) ) ||
				((tmp_head->pointer.addr+tmp_head->pointer.len >= pt.addr) 
						&& (tmp_head->pointer.addr+tmp_head->pointer.len <= pt.addr +pt.len ) )
			){
			tmp = tmp_head ;
			if (tmp_head = *hlist_head){					
				tmp_head = tmp_head->next;
				*hlist_head = tmp_head;							
			}else {
				tmp_head->prev->next = tmp->next;
				tmp_head->next->prev = tmp->prev ;
				tmp_head = tmp->next ;	
			}
			free(tmp);			
		} else{
		
			tmp_head = tmp_head->next;
		}
	}
	
	// Last node
	if ((tmp_head->pointer.manager_addr == pt.manager_addr) ||
				((tmp_head->pointer.addr >=pt.addr) 
						&& (tmp_head->pointer.addr <= pt.addr + pt.len) ) ||
				((tmp_head->pointer.addr+tmp_head->pointer.len >= pt.addr) 
						&& (tmp_head->pointer.addr+tmp_head->pointer.len <= pt.addr +pt.len ) )
			){
	
		*hlist_head = NULL;
		*hlist_tail = NULL;
		free (tmp_head) ;
	}
	return 1 ;	
}


/*
 * ---------------------------------------------------------------------
 * Open the window of variables
 * ---------------------------------------------------------------------
 */
void open_parallel_window(){	
	__is_inside_parallel_region__ = TRUE; // Enter parallel reion	
	__parallel_level__ ++;
	generate_variable_list_for_parallel_windows(__active_variable_head);	
}

/*
 * ---------------------------------------------------------------------
 * Close the window of variables
 * Release checkpoint memory
 * ---------------------------------------------------------------------
 */
void close_parallel_window(){
	
	if (__parallel_level__ > 0) {
		remove_active_variables(&__var_list_head, &__var_list_tail, __parallel_level__);		
		__parallel_level__--;
	}
	__is_inside_parallel_region__= FALSE; //Exit parallel region

}

/*
 * ---------------------------------------------------------------------
 * Add new region in parallel region
 * => copy variables of parent's level
 * ---------------------------------------------------------------------
 */
int add_parallel_region(VarList **vlist_head, VarList **vlist_tail, unsigned char level){
	
	VarList *vl_tail_level = NULL;
	VarList *vl_head = NULL;
	
	vl_head = *vlist_head;
	vl_tail_level = *vlist_tail;
	
	if(vl_head == NULL) return 0;
	VarList *copy_item = NULL;		
	VarList *item = NULL;
	
	item = vl_head;
	while(item->var.level != (level - 1) )
		item = item->next;
	
	//Copy items and assigned new level	
	while(item->var.level == (level -1)){			
		copy_item = malloc(sizeof(VarList));		
		copy_item->var.addr = item->var.addr;
		copy_item->var.size = item->var.size;
		copy_item->var.n = item->var.n;
		copy_item->var.dtype = item->var.dtype;
		copy_item->var.pro = item->var.pro; 
		copy_item->var.level = level; //New level	
		copy_item->var.ispointer = item->var.ispointer;		
		copy_item->next = NULL;
		copy_item->prev = vl_tail_level;		
		vl_tail_level->next = copy_item;
		vl_tail_level = copy_item;		
		item = item->next;
	}	
	*vlist_tail = vl_tail_level;
	return 1;
}
/*
 * ---------------------------------------------------------------------
 * Remove variables level
 *----------------------------------------------------------------------
 */
int remove_parellel_region(unsigned char level){
	if( level > 1){
		remove_active_variables(&__var_list_head, &__var_list_tail, level ); 
	}
	return 1;
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
		if ((vl->var.addr == addr) && (vl->var.level == level ) ){				 
			break;
		}
		//printf("Checking: Ox%lx is in VarList: [Ox%lx - Ox%lx) - Level: %d \n" , addr, vl->var.addr, vl->var.addr + (vl->var.n * vl->var.size), vl->var.level);
		vl = vl->next;		
	}
	return vl;	
}

/*
 * ---------------------------------------------------------------------
 * Generate checkpoint 
 * 	Checkpoint will be generated depending on properties of variables 
 * 	and the phase of execution model.
 * Checkpoint Structure:
 * C = {t , program counter, size_of_S, S, L}, 
 * 		where S = All modified data (addr, len, data)
 * 			  L = data of reduction variables (addr, len, data)
 *----------------------------------------------------------------------
 */
FILE *generate_checkpoint(VarList *vlist,	
			unsigned char level,
			unsigned char cflag,
			unsigned char ops_flag,
			unsigned long tsp,
			unsigned long pc ){
	
	VarList *v = NULL, *v1 = NULL;	
	unsigned long timestamp, programcounter;
	unsigned long size_s = 0;
	unsigned long start_addr;
	unsigned long  v_end, v_addr;
	unsigned long start;
	unsigned int len;
	unsigned long file_pointer = 0;
	char *data;
	
		
	FILE *stream;
	
	//open checkpoint file
	timestamp = tsp;
	stream = open_memstream(&after_ckpt, &after_ckpt_size);
	
	//write time stamp into checkpoint file
	fwrite(&timestamp, sizeof(unsigned long), 1, stream);
	fflush(stream);
	
	//write program counter into checkpoint file
	programcounter = pc ;
	fwrite(&programcounter, sizeof(unsigned long), 1, stream);
	fflush(stream);
	
	if (ckpt_data_size <= 0){ 
		size_s = sizeof(unsigned long) * 3;
		fwrite(&size_s, sizeof(unsigned long), 1, stream);
		fflush(stream);		
		return stream;
	}
	
	//Write size_s into checkpoint file, and this place will be modified after writting S part
	fwrite(&size_s, sizeof(unsigned long), 1, stream);
	fflush(stream);
	
	//Move to current window (the current level of VarList)
	v = vlist;
	while ((v != NULL) && (v->var.level != level))
		v = v->next;
		
	//Move to the first DPAGE
	//while ((v != NULL) && ((v->addr + v->n * v->size) < dplist->addr))
	//	v = v->next;
	unsigned int data_pointer;	
	while(file_pointer < ckpt_data_size){
		v_addr = (*(unsigned long *) (ckpt_data + file_pointer)) ; 		
		v1 = find_variable_by_addr(v, v_addr, level);
		file_pointer += sizeof(unsigned long);
		data_pointer = file_pointer; //save possition of data
		file_pointer += v1->var.n * v1->var.size ;
		
		if ((v1->var.pro == CAPE_PRIVATE) || (v1->var.pro == CAPE_THREAD_PRIVATE))
			continue;
		
		if ((cflag == ENTRY_CHECKPOINT) && (v1->var.pro == CAPE_LAST_PRIVATE))
			continue;
		
		if ((cflag ==EXIT_CHECKPOINT) && 
			((v1->var.pro == CAPE_FIRST_PRIVATE) ||
		     (v1->var.pro == CAPE_COPY_IN) ||
		     (v1->var.pro == CAPE_SUM) ||
		     (v1->var.pro == CAPE_MUL) || 
		     (v1->var.pro == CAPE_MAX) || 
		     (v1->var.pro == CAPE_MIN)))
				continue ;
			
	//	printf("CHECKPOINT DATA: In VarList: [Ox%lx - Ox%lx) - Level: %d - %d bytes \n" , \
			v1->var.addr, v1->var.addr + (v1->var.n * v1->var.size), v1->var.level , v1->var.n * v1->var.size);
		
		start_addr = v_addr;
		v_end = v_addr + (v1->var.n * v1->var.size);
		while (start_addr < v_end){
			if (v1->var.size == CAPE_WORD){
				//Ignore the data that is not modified
				while ((start_addr < v_end) &&						
					( (*(int *)start_addr) == (*(int *) (ckpt_data + data_pointer + (start_addr - v_addr ))) )	)			
					start_addr += CAPE_WORD;				
				//count the modified data
				start = start_addr ;
				while ((start_addr < v_end) &&						
					( (*(int *)start_addr) != (*(int *) (ckpt_data + data_pointer + (start_addr - v_addr ))) )	)
				{			
					//printf("(%d) \t" , (*(int *) start_addr) );
					start_addr += CAPE_WORD ;
				}
				//save into ckpt file
				if (start_addr > start){
					len = start_addr - start ;
					fwrite(&start, sizeof(unsigned long), 1, stream);
					fwrite(&len, sizeof(unsigned int), 1, stream);
					fwrite(start, len, 1, stream);
					fflush(stream);
				}
			}
			else{ //8 bytes			
				//Ignore the data that is not modified
				while ((start_addr < v_end) &&						
					( (*(double*)start_addr) == (*(double *) (ckpt_data + data_pointer + (start_addr - v_addr ))) )	)			
					start_addr += DOUBLE_CAPE_WORD;				
				//count the modified data
				start = start_addr ;
				while ((start_addr < v_end) &&						
					( (*(double *)start_addr) != (*(double *) (ckpt_data + data_pointer + (start_addr - v_addr ))) )	)
				{			
					//printf("%d \t" , (*(int *) start_addr) );
					start_addr += DOUBLE_CAPE_WORD ;
				}
				//save into ckpt file
				if (start_addr > start){
					len = start_addr - start ;
					fwrite(&start, sizeof(unsigned long), 1, stream);
					fwrite(&len, sizeof(unsigned int), 1, stream);
					fwrite(start, len, 1, stream);
					fflush(stream);
				}
	}
		
			//printf("\nNode %d: File pointer %ld - file size %ld \n",__node__, file_pointer, ckpt_data_size);
		}
	}	
	//Identity size of S part
	size_s = after_ckpt_size;		
	//Write L part into checkpoint
	if((cflag == EXIT_CHECKPOINT) && (ops_flag==TRUE)){
		//Move to current window (the current level of VarList)
		v = vlist;
		while ((v != NULL) && (v->var.level != level))
			v = v->next;
		while ((v != NULL) && (v->var.level == level)){
			if ((v->var.pro == CAPE_SUM) || (v->var.pro == CAPE_MUL) || (v->var.pro == CAPE_MAX) ||(v->var.pro == CAPE_MIN) ){
				len = v->var.size * v->var.n;
				fwrite(&v->var.addr, sizeof(long), 1, stream);
				fwrite(&len, sizeof(unsigned int), 1, stream);
				fwrite(v->var.addr, len, 1, stream);
				fflush(stream);				
			}
			v = v->next;
		}			
	}
	long end_pos = ftell(stream);
	if (end_pos >= 0) {
		fseek(stream, 2*sizeof(unsigned long), SEEK_SET);
		fwrite(&size_s, sizeof(unsigned long), 1, stream);
		fseek(stream, end_pos, SEEK_SET);
		fflush(stream);
	}
	
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
	unsigned int end_s1, end_s2;
	unsigned int len, len1, len2;
	long addr1, addr2, old_addr2;
	p1 = pos_s1;
	p2 = pos_s2;
	end_s1 = pos_s1 + size_s1;
	end_s2 = pos_s2 + size_s2;
	
	//printf("*** NODE %d: Position 1= %d, possition 2 =%d: Merge %ld bytes s1 and %ld bytes s2 \n", __node__, pos_s1, pos_s2, size_s1, size_s2);
	
	if ((p1 >= end_s1) && (p2 >= end_s2))
		return 0;
	//if S1 == NULL =>  S = S2
	if (p1 >= end_s1){
		fwrite(s2 + p2, end_s2 - p2, 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		return 1;
	}
	//if S2 == NULL => S = S1
	if (p2 >= end_s2){
		fwrite(s1 + p1, end_s1 - p1, 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		return 1;
	}
	/* Need at least [addr,len] header in each stream */
	if ((end_s1 - p1) < (sizeof(long) + sizeof(unsigned int)) ||
	    (end_s2 - p2) < (sizeof(long) + sizeof(unsigned int)))
		return -1;

	addr1 = *(long *) (s1 + p1 );
	p1 += sizeof(long);
	len1 = *(unsigned int *) (s1 + p1) ;
	p1 += sizeof(unsigned int);
	if (len1 > (end_s1 - p1))
		return -1;
	
	addr2 = *(long *) (s2 + p2 );
	p2 += sizeof(long);
	len2 = *(unsigned int *) (s2 + p2) ;
	p2 += sizeof(unsigned int);
	if (len2 > (end_s2 - p2))
		return -1;
	while ((p1 < end_s1) && (p2 < end_s2)){
		//printf("\n Node %ld: (0x%lx - %ld ) + (0x%lx - %ld)", __node__, addr1, len1, addr2, len2) ;
		if (addr1 <= addr2){
			fwrite(&addr1, sizeof(long), 1, after_ckpt_stream);
			fflush(after_ckpt_stream);
			fwrite(&len1, sizeof(unsigned int), 1, after_ckpt_stream);
			fflush(after_ckpt_stream);			
			if (len1 > (end_s1 - p1))
				return -1;
			fwrite(s1 + p1, len1, 1, after_ckpt_stream);
			fflush(after_ckpt_stream);
			p1 += len1;
			//printf("\n Node %ld: Write: 0x%lx  : %ld ", __node__, addr1, len1) ;
			//S1: [-----------)  
			//S2:     -----		 ------	
			if ((addr1 + len1) >= (addr2+ len2)){
				p2 += len2;
				if (p2 < end_s2) {
					if ((end_s2 - p2) < (sizeof(long) + sizeof(unsigned int)))
						return -1;
					addr2 = *(long *) (s2 + p2 );
					p2 += sizeof(long);
					len2 = *(unsigned int *) (s2 + p2) ;
					p2 += sizeof(unsigned int);
					if (len2 > (end_s2 - p2))
						return -1;
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
			if (p1 < end_s1) {
				if ((end_s1 - p1) < (sizeof(long) + sizeof(unsigned int)))
					return -1;
				addr1 = *(long *) (s1 + p1 );
				p1 += sizeof(long);
				len1 = *(unsigned int *) (s1 + p1) ;
				p1 += sizeof(unsigned int);
				if (len1 > (end_s1 - p1))
					return -1;
			}					
		}else{ //addr1 > addr2
			//S1:                      [-----------)         [--------)
			//S2:        [--------)		    -------[------)	
			if ((addr2 + len2) < addr1) {
				fwrite(&addr2, sizeof(long), 1, after_ckpt_stream);
				fflush(after_ckpt_stream);
				fwrite(&len2, sizeof(unsigned int), 1, after_ckpt_stream);
				fflush(after_ckpt_stream);			
				if (len2 > (end_s2 - p2))
					return -1;
				fwrite(s2 + p2, len2, 1, after_ckpt_stream);
				fflush(after_ckpt_stream);
				//printf("\n Node %ld: Write: 0x%lx : %ld ", __node__, addr2, len2) ;
				p2 += len2;					
				if (p2 >= end_s2) break;
				if ((end_s2 - p2) < (sizeof(long) + sizeof(unsigned int)))
					return -1;
				addr2 = *(long *) (s2 + p2 );
				p2 += sizeof(long);
				len2 = *(unsigned int *) (s2 + p2) ;
				p2 += sizeof(unsigned int);												
				if (len2 > (end_s2 - p2))
					return -1;
			//S1:       [-----------)        [--------)
			//S2:       	 -------[--------)--------[------)	
			}else {
				len = addr1 - addr2;				
				fwrite(&addr2, sizeof(long), 1, after_ckpt_stream);
				fflush(after_ckpt_stream);				
				fwrite(&len, sizeof(unsigned int), 1, after_ckpt_stream);
				fflush(after_ckpt_stream);
				if (len > (end_s2 - p2))
					return -1;
				fwrite(s2 + p2, len, 1, after_ckpt_stream);
				fflush(after_ckpt_stream);
				//printf("\n Node %ld: Write: 0x%lx : %ld ", __node__, addr2, len2) ;
				p2 += len;

				len2 = len2-len;	
				addr2 = addr1;							
			}
		}	
	}	
	//Merge the rest part
	if (p1 < end_s1){
		fwrite(&addr1, sizeof(long), 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		fwrite(&len1, sizeof(unsigned int), 1, after_ckpt_stream);
		fflush(after_ckpt_stream);			
		if (len1 > (end_s1 - p1))
			return -1;
		fwrite(s1 + p1, len1, 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		p1 += len1;
	}	
	while (p1 < end_s1){
		if ((end_s1 - p1) < (sizeof(long) + sizeof(unsigned int)))
			return -1;
		addr1 = *(long *) (s1 + p1 );
		p1 += sizeof(long);
		len1 = *(unsigned int *) (s1 + p1) ;
		p1 += sizeof(unsigned int);
		if (len1 > (end_s1 - p1))
			return -1;
		fwrite(&addr1, sizeof(long), 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		fwrite(&len1, sizeof(unsigned int), 1, after_ckpt_stream);
		fflush(after_ckpt_stream);			
		fwrite(s1 + p1, len1, 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		p1 += len1;		
	}
	
	if (p2 < end_s2){
		fwrite(&addr2, sizeof(long), 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		fwrite(&len2, sizeof(unsigned int), 1, after_ckpt_stream);
		fflush(after_ckpt_stream);			
		if (len2 > (end_s2 - p2))
			return -1;
		fwrite(s2 + p2, len2, 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		p2 += len2;
	}	
	while (p2 < end_s2){
		if ((end_s2 - p2) < (sizeof(long) + sizeof(unsigned int)))
			return -1;
		addr2 = *(long *) (s2 + p2 );
		p2 += sizeof(long);
		len2 = *(unsigned int *) (s2 + p2) ;
		p2 += sizeof(unsigned int);
		if (len2 > (end_s2 - p2))
			return -1;
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

static int validate_checkpoint_s_section(const char *ckpt,
		size_t total_size,
		unsigned long size_s,
		const char *label)
{
	const unsigned long ckpt_header_size = sizeof(unsigned long) * 3;
	unsigned long p = ckpt_header_size;
	unsigned int len = 0;

	if (total_size < ckpt_header_size) {
		fprintf(stderr, "CAPE %s: checkpoint too small (%zu)\n", label, total_size);
		return -1;
	}
	if ((size_s < ckpt_header_size) || (size_s > total_size)) {
		fprintf(stderr, "CAPE %s: invalid size_s=%lu total=%zu\n",
		        label, size_s, total_size);
		return -1;
	}

	while (p < size_s) {
		if ((size_s - p) < (sizeof(long) + sizeof(unsigned int))) {
			fprintf(stderr, "CAPE %s: truncated record header at %lu/%lu\n",
			        label, p, size_s);
			return -1;
		}
		p += sizeof(long);
		len = *(unsigned int *)(ckpt + p);
		p += sizeof(unsigned int);
		if (len > (size_s - p)) {
			fprintf(stderr, "CAPE %s: truncated record payload at %lu len=%u size_s=%lu\n",
			        label, p, len, size_s);
			return -1;
		}
		p += len;
	}
	return 0;
}
/*
 * ---------------------------------------------------------------------
 * Merge a buffer to after_checkpoint
 * 	if (t1 >= t2) 
 * 		{ C <- t1 , pc1, size_s, S1 + S2}
 * 	else
 * 		{ C <- t2, pc2, size_s, S2+S1}
 * --------------------------------------------------------------------- 
 */
int merge_checkpoint(char *src_ckpt, size_t src_size, char ckpt_flag){	
	
	FILE *tmp_stream = NULL;
	char *tmp_ckpt = NULL;
	size_t tmp_size = 0;
	const unsigned long ckpt_header_size = sizeof(unsigned long) * 3;
	
	unsigned int src_pointer =0, tmp_pointer =0;
	
	if (src_size < ckpt_header_size)
		return 0;
	
	if (after_ckpt_size == 0){	
		unsigned long size_s_src = *(unsigned long *)(src_ckpt + (2 * sizeof(unsigned long)));
		if (validate_checkpoint_s_section(src_ckpt, src_size, size_s_src, "merge_checkpoint[src_init]") < 0)
			return -1;
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
	if (after_ckpt_stream != NULL) {
		fclose(after_ckpt_stream);
		after_ckpt_stream = NULL;
	}
	free(after_ckpt);
	after_ckpt = NULL;
	after_ckpt_size = 0;
		
	src_pointer = 0;
 	tmp_pointer =0 ;
 	unsigned long t1, t2;
 	unsigned long pc1, pc2;
 	unsigned long size_s = 0, size_s1, size_s2;
	int merge_rc = 0;
  	
	
 	t1 = *(unsigned long *)tmp_ckpt;
 	t2 = *(unsigned long *)src_ckpt;  	 	
 	tmp_pointer += sizeof(unsigned long);
 	src_pointer += sizeof(unsigned long);
 	
 	pc1 =  *(unsigned long *)(tmp_ckpt + tmp_pointer);
 	pc2 =  *(unsigned long *)(src_ckpt + src_pointer);	 	
 	src_pointer += sizeof(unsigned long);
 	tmp_pointer += sizeof(unsigned long);
 	
 	size_s1 =  *(unsigned long *)(tmp_ckpt + tmp_pointer);
 	size_s2 =  *(unsigned long *)(src_ckpt + src_pointer);
	if ((size_s1 < ckpt_header_size) || (size_s1 > tmp_size) ||
	    (size_s2 < ckpt_header_size) || (size_s2 > src_size)) {
		fprintf(stderr,
		        "CAPE merge_checkpoint: invalid size_s values (size_s1=%lu tmp_size=%zu size_s2=%lu src_size=%zu)\n",
		        size_s1, tmp_size, size_s2, src_size);
		fprintf(stderr,
		        "CAPE merge_checkpoint: header dump local(t=%lu pc=0x%lx) remote(t=%lu pc=0x%lx)\n",
		        t1, pc1, t2, pc2);
		fclose(tmp_stream);
		free(tmp_ckpt);
		return -1;
	}
	if (validate_checkpoint_s_section(tmp_ckpt, tmp_size, size_s1, "merge_checkpoint[tmp]") < 0 ||
	    validate_checkpoint_s_section(src_ckpt, src_size, size_s2, "merge_checkpoint[src]") < 0) {
		fclose(tmp_stream);
		free(tmp_ckpt);
		return -1;
	}
 	 	
 	src_pointer += sizeof(unsigned long);
 	tmp_pointer += sizeof(unsigned long);
	 	
 	//Open des_stream file again
 	after_ckpt_stream = open_memstream(&after_ckpt, &after_ckpt_size);
 	
 	
 	if (t1 >= t2){ 			//C <- t1, pc1, size_s, S1+ S2		
		fwrite(&t1, sizeof(unsigned long), 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		fwrite(&pc1, sizeof(unsigned long), 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		fwrite(&size_s, sizeof(unsigned long), 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		if ((size_s1 > tmp_pointer) && (size_s2 > src_pointer))
			merge_rc = merge_data(tmp_ckpt, tmp_pointer, size_s1 - tmp_pointer, src_ckpt, src_pointer, size_s2 - src_pointer);
		else if (size_s1 > tmp_pointer)
			fwrite(tmp_ckpt + tmp_pointer, size_s1 - tmp_pointer, 1, after_ckpt_stream);
		else if (size_s2 > src_pointer)
			fwrite(src_ckpt + src_pointer, size_s2 - src_pointer, 1, after_ckpt_stream);
	}else{	//C <-	t2, pc2, size_s, S2 + S1		
		fwrite(&t2, sizeof(unsigned long), 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		fwrite(&pc2, sizeof(unsigned long), 1, after_ckpt_stream);
		fflush(after_ckpt_stream);
		fwrite(&size_s, sizeof(unsigned long), 1, after_ckpt_stream);
		fflush(after_ckpt_stream);		
		if ((size_s1 > tmp_pointer) && (size_s2 > src_pointer))
			merge_rc = merge_data(src_ckpt, src_pointer, size_s2 - src_pointer, tmp_ckpt, tmp_pointer, size_s1 - tmp_pointer);
		else if (size_s2 > src_pointer)
			fwrite(src_ckpt + src_pointer, size_s2 - src_pointer, 1, after_ckpt_stream);
		else if (size_s1 > tmp_pointer)
			fwrite(tmp_ckpt + tmp_pointer, size_s1 - tmp_pointer, 1, after_ckpt_stream);
	}
	if (merge_rc < 0) {
		fprintf(stderr, "CAPE merge_checkpoint: malformed S section (size_s1=%lu size_s2=%lu)\n",
		        size_s1, size_s2);
		fclose(after_ckpt_stream);
		after_ckpt_stream = NULL;
		free(after_ckpt);
		after_ckpt = NULL;
		after_ckpt_size = 0;
		fclose(tmp_stream);
		free(tmp_ckpt);
		tmp_ckpt = NULL;
		return -1;
	}
	tmp_pointer = size_s1;
	src_pointer = size_s2;	
	size_s = after_ckpt_size;
	
	//Check if exist reduction data
	if (ckpt_flag == EXIT_CHECKPOINT){ 
		while (	(tmp_pointer < tmp_size) && (src_pointer <src_size)){		
			long addr, src_addr;
			unsigned int len, src_len;
			
			if ((tmp_size - tmp_pointer) < (sizeof(long) + sizeof(unsigned int)) ||
			    (src_size - src_pointer) < (sizeof(long) + sizeof(unsigned int))) {
				fprintf(stderr, "CAPE merge_checkpoint: malformed L header\n");
				fclose(after_ckpt_stream);
				after_ckpt_stream = NULL;
				free(after_ckpt);
				after_ckpt = NULL;
				after_ckpt_size = 0;
				fclose(tmp_stream);
				free(tmp_ckpt);
				return -1;
			}

			addr = *((long*)(tmp_ckpt + tmp_pointer));
			src_addr = *((long*)(src_ckpt + src_pointer));
			tmp_pointer += sizeof(long);
			src_pointer += sizeof(long);

			len = *(unsigned int *) (tmp_ckpt + tmp_pointer) ;
			src_len = *(unsigned int *) (src_ckpt + src_pointer) ;
			tmp_pointer += sizeof(unsigned int);
			src_pointer += sizeof(unsigned int);
			if ((addr != src_addr) || (len != src_len)) {
				fprintf(stderr,
				        "CAPE merge_checkpoint: L mismatch (addr=0x%lx src_addr=0x%lx len=%u src_len=%u)\n",
				        addr, src_addr, len, src_len);
				fclose(after_ckpt_stream);
				after_ckpt_stream = NULL;
				free(after_ckpt);
				after_ckpt = NULL;
				after_ckpt_size = 0;
				fclose(tmp_stream);
				free(tmp_ckpt);
				return -1;
			}
			if ((len > (tmp_size - tmp_pointer)) || (len > (src_size - src_pointer))) {
				fprintf(stderr,
				        "CAPE merge_checkpoint: malformed L payload (len=%u tmp_left=%zu src_left=%zu)\n",
				        len, tmp_size - tmp_pointer, src_size - src_pointer);
				fclose(after_ckpt_stream);
				after_ckpt_stream = NULL;
				free(after_ckpt);
				after_ckpt = NULL;
				after_ckpt_size = 0;
				fclose(tmp_stream);
				free(tmp_ckpt);
				return -1;
			}
			
			fwrite(&addr, sizeof(long), 1, after_ckpt_stream);
			fflush(after_ckpt_stream);
			fwrite(&len, sizeof(unsigned int), 1, after_ckpt_stream);
			fflush(after_ckpt_stream);

			VarList *var = NULL; 
			var = find_variable_by_addr(__var_list_head, addr , __parallel_level__);	
			
			if (var == NULL) {
				fprintf(stderr, "CAPE merge_checkpoint: variable not found for addr=0x%lx level=%d\n",
				        addr, __parallel_level__);
				fclose(after_ckpt_stream);
				after_ckpt_stream = NULL;
				free(after_ckpt);
				after_ckpt = NULL;
				after_ckpt_size = 0;
				fclose(tmp_stream);
				free(tmp_ckpt);
				return -1; //ERROR
			}

			int num, n1, n2;
			unsigned long num_l, n1_l, n2_l;
			float num_f, n1_f, n2_f;
			double num_d, n1_d, n2_d;			
			switch (var->var.dtype){
				case CAPE_CHAR:
				case CAPE_INT:
				case CAPE_LONG:					
					n1 =  *(long*) (tmp_ckpt + tmp_pointer);
					n2=  *(long*) (src_ckpt + src_pointer) ;
					if (var->var.pro == CAPE_SUM){
						num = 0;
						num = n1 + n2;
						fwrite(&num, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}
					if (var->var.pro == CAPE_MUL){
						num = 1;
						num = n1 * n2;		
						fwrite(&num, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}					
					if (var->var.pro == CAPE_MAX){
						num = (n1 >= n2 )? n1 : n2 ;
						fwrite(&num, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}
					if (var->var.pro == CAPE_MIN){						
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
					if (var->var.pro == CAPE_SUM){
						num_l = 0;
						num_l = n1_l + n2_l;
						fwrite(&num_l, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}
					if (var->var.pro == CAPE_MUL){
						num_l = 1;
						num_l = n1_l * n2_l;		
						fwrite(&num_l, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}					
					if (var->var.pro == CAPE_MAX){
						num_l = (n1_l >= n2_l )? n1_l : n2_l ;
						fwrite(&num_l, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}
					if (var->var.pro == CAPE_MIN){						
						num_l = (n1_l < n2_l )? n1_l : n2_l ;				
						fwrite(&num_l, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}							
					break;
				case CAPE_FLOAT:					
					n1_f =  *(float*) (tmp_ckpt + tmp_pointer);
					n2_f=  *(float*) (src_ckpt + src_pointer) ;
					if (var->var.pro == CAPE_SUM){
						num_f = 0.0;
						num_f = n1_f + n2_f;
						fwrite(&num_f, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}
					if (var->var.pro == CAPE_MUL){
						num_f = 1.0;
						num_f = n1_f * n2_f;		
						fwrite(&num_f, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}					
					if (var->var.pro == CAPE_MAX){
						num_f = (n1_f >= n2_f )? n1_f : n2_f ;
						fwrite(&num_f, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}
					if (var->var.pro == CAPE_MIN){						
						num_f = (n1_f < n2_f )? n1_f : n2_f ;				
						fwrite(&num_f, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}							
					break;
				case CAPE_DOUBLE:					
					n1_d =  *(double*) (tmp_ckpt + tmp_pointer);
					n2_d=  *(double*) (src_ckpt + src_pointer);					
					if (var->var.pro == CAPE_SUM){
						num_d = 0.0;
						num_d = n1_d + n2_d;
						fwrite(&num_d, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}
					if (var->var.pro == CAPE_MUL){
						num_d = 1.0;
						num_d = n1_d * n2_d;		
						fwrite(&num_d, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}					
					if (var->var.pro == CAPE_MAX){
						num = (n1_d >= n2_d )? n1_d : n2_d ;
						fwrite(&num_d, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}
					if (var->var.pro == CAPE_MIN){						
						num_d = (n1_d < n2_d )? n1_d : n2_d ;				
						fwrite(&num_d, len, 1, after_ckpt_stream);
						fflush(after_ckpt_stream);
						break;						
					}							
					break;
				default:
					printf("This datatype is not supported !!!!!");
					fclose(after_ckpt_stream);
					after_ckpt_stream = NULL;
					free(after_ckpt);
					after_ckpt = NULL;
					after_ckpt_size = 0;
					fclose(tmp_stream);
					free(tmp_ckpt);
					return -1;
			}
			
			src_pointer += len;
			tmp_pointer += len;
		}
		if ((tmp_pointer != tmp_size) || (src_pointer != src_size)) {
			fprintf(stderr,
			        "CAPE merge_checkpoint: uneven L sections (tmp_pointer=%u tmp_size=%zu src_pointer=%u src_size=%zu)\n",
			        tmp_pointer, tmp_size, src_pointer, src_size);
			fclose(after_ckpt_stream);
			after_ckpt_stream = NULL;
			free(after_ckpt);
			after_ckpt = NULL;
			after_ckpt_size = 0;
			fclose(tmp_stream);
			free(tmp_ckpt);
			return -1;
		}
		

	}	
	long end_pos = ftell(after_ckpt_stream);
	if (end_pos >= 0) {
		fseek(after_ckpt_stream, 2*sizeof(unsigned long), SEEK_SET);
		fwrite(&size_s, sizeof(unsigned long), 1, after_ckpt_stream);
		fseek(after_ckpt_stream, end_pos, SEEK_SET);
		fflush(after_ckpt_stream);
	}

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
int hypercube_allreduce(unsigned int node, unsigned int num_nodes, char ckpt_flag, uint32_t epoch){
	int i, nsteps = 0;
	unsigned int partner;
	unsigned long send_msg_size=0, recv_msg_size = 0;
	int rc = 0;
	uint32_t size_token, data_token;
	char *send_msg;
	char *recv_msg;

	nsteps = mylog2(num_nodes);

	send_msg = after_ckpt;
	send_msg_size = after_ckpt_size;

	for(i = 0; i < nsteps; i++){
		partner = node ^ (1 << i);
		size_token = ((epoch & 0x0fffU) << 8) | (uint32_t)((i * 2) & 0xff);
		data_token = ((epoch & 0x0fffU) << 8) | (uint32_t)(((i * 2) + 1) & 0xff);

		/* Exchange message sizes (step tag: i*2) */
		cape_ucx_sendrecv(&send_msg_size, sizeof(unsigned long), partner,
		                  &recv_msg_size, sizeof(unsigned long), partner,
		                  size_token);

		// fprintf(stderr, "CAPE DBG node %d step %d: SIZE exchange done"
		//         " sent=%lu recv=%lu\n",
		//         node, i, send_msg_size, recv_msg_size);

		recv_msg = malloc(sizeof(char) * recv_msg_size);
		if (recv_msg == NULL) {
			fprintf(stderr, "CAPE hypercube_allreduce: malloc failed (recv_msg_size=%lu)\n",
			        recv_msg_size);
			return -1;
		}

		/* Exchange checkpoint data (step tag: i*2+1) */
		cape_ucx_sendrecv(send_msg, send_msg_size, partner,
		                  recv_msg, recv_msg_size, partner,
		                  data_token);

		rc = merge_checkpoint(recv_msg, recv_msg_size, ckpt_flag);

		free(recv_msg);
		recv_msg_size = 0;
		if (rc < 0)
			return -1;

		send_msg = after_ckpt;
		send_msg_size = after_ckpt_size;
	}

	return 1;
}

/*
 *---------------------------------------------------------------------
 * Send and Receive checkpoint based on Ring algorithm  
 *----------------------------------------------------------------------
 */
int ring_allreduce(unsigned int node, unsigned int nnodes, char ckpt_flag, uint32_t epoch){

	char *send_msg;
	char *recv_msg;
	char *owned_send_msg;
	unsigned int send_msg_size, recv_msg_size;
	int rc;
	uint32_t size_token, data_token;

	unsigned int i, left, right;
	left  = (node - 1 + nnodes) % nnodes;
	right = (node + 1) % nnodes;

	send_msg = after_ckpt;
	owned_send_msg = NULL;
	send_msg_size = after_ckpt_size;

	for(i = 1; i < nnodes; i++){
		size_token = ((epoch & 0x0fffU) << 8) | (uint32_t)((i * 2) & 0xff);
		data_token = ((epoch & 0x0fffU) << 8) | (uint32_t)(((i * 2) + 1) & 0xff);
		/* Exchange sizes: send to right, receive from left (step tag: i*2) */
		cape_ucx_sendrecv(&send_msg_size, sizeof(unsigned int), right,
		                  &recv_msg_size, sizeof(unsigned int), left,
		                  size_token);

		recv_msg = malloc(sizeof(char) * recv_msg_size);
		if (recv_msg == NULL) {
			fprintf(stderr, "CAPE ring_allreduce: malloc failed (recv_msg_size=%u)\n",
			        recv_msg_size);
			if (owned_send_msg != NULL)
				free(owned_send_msg);
			return -1;
		}

		/* Exchange data (step tag: i*2+1) */
		cape_ucx_sendrecv(send_msg, send_msg_size, right,
		                  recv_msg, recv_msg_size, left,
		                  data_token);

		rc = merge_checkpoint(recv_msg, recv_msg_size, ckpt_flag);
		if (owned_send_msg != NULL) {
			free(owned_send_msg);
			owned_send_msg = NULL;
		}
		if (rc < 0) {
			free(recv_msg);
			return -1;
		}

		owned_send_msg = recv_msg;
		send_msg      = owned_send_msg;
		send_msg_size = recv_msg_size;
		recv_msg_size = 0;
	}
	if (owned_send_msg != NULL)
		free(owned_send_msg);
	return 1;
}

/*
 * ---------------------------------------------------------------------
 * Require synchronize checkpoints
 * ---------------------------------------------------------------------
 */

int require_allreduce(char ckpt_flag){
	uint32_t epoch;
	
	//printf("Node %d:  nnodes = %d - is_power_of2: %d \n", __node__, __nnodes__, is_power_of_two(__nnodes__));
	
	if (after_ckpt_size == 0) return 0 ;
	__allreduce_epoch__++;
	epoch = __allreduce_epoch__ & 0x0fffU;
	if (epoch == 0)
		epoch = 1;
	if (is_power_of_two(__nnodes__))
		return hypercube_allreduce(__node__, __nnodes__, ckpt_flag, epoch);
	else
		return ring_allreduce(__node__, __nnodes__, ckpt_flag, epoch);
	
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
	const unsigned long ckpt_header_size = sizeof(unsigned long) * 3;
	
	if (file_size <= ckpt_header_size) return 0;	
	//printf("\n Node %d: inject ckpt - file size = %d bytes", __node__, file_size);
	
	__pc__ = *(unsigned long *) (data_ckpt + sizeof(unsigned long)); //get program counter
	file_pointer = ckpt_header_size; //timestime, program counter, size_s	
	
	while(file_pointer < file_size){
		if ((file_size - file_pointer) < (sizeof(long) + sizeof(unsigned int))) {
			fprintf(stderr, "CAPE inject_checkpoint: malformed record header at %lu/%zu\n",
			        file_pointer, file_size);
			return -1;
		}
		
		addr = *(long *) (data_ckpt + file_pointer);		
		file_pointer += sizeof(long);		
		
		len = *(unsigned int*) (data_ckpt + file_pointer );
		file_pointer += sizeof(unsigned int);
		if (len > (file_size - file_pointer)) {
			fprintf(stderr,
			        "CAPE inject_checkpoint: malformed record payload len=%lu left=%zu\n",
			        len, file_size - file_pointer);
			return -1;
		}
		if (addr == 0) {
			fprintf(stderr, "CAPE inject_checkpoint: null destination address\n");
			return -1;
		}

		memcpy(addr, data_ckpt+file_pointer, len)	;		
		
		//if (__node__ == 0)
		//	printf("DATA: Node %d: addr = 0x%lx - len = %d bytes - data %d \n",__node__, addr, len, *(int *) (data_ckpt+ file_pointer) );	
				
		file_pointer += len ;			
	}
	
	return 1;
}

void release_checkpoint(){
	if (after_ckpt_stream != NULL){
		fclose(after_ckpt_stream);
		after_ckpt_stream = NULL;
	}
	free(after_ckpt);	
	after_ckpt = NULL;
	after_ckpt_size = 0;
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


/*******************************************/
/* Data-sharing attribute */
/*******************************************/
/**
 * ---------------------------------------------------------------------
 * Set data attribute: default(none)
 * ---------------------------------------------------------------------
 */
void set_default_none(VarList *vlist_tail, char level){
	VarList *vtail;
	vtail = vlist_tail;
	while ((vtail != NULL ) && (vtail->var.level == level) ){
		vtail->var.pro = CAPE_PRIVATE;		
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
	while ((vtail != NULL ) && (vtail->var.level == level) ){
		if (vtail->var.addr == addr) {
			vtail->var.pro = pro;
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
		if (vhead->var.addr == addr) {
			vhead->var.pro = CAPE_PRIVATE;
			break;		
		}
		vhead = vhead->next;
	}
}


/***********************************************************************/
/*		Clauses and data sharing directives							   */
/***********************************************************************/
void cape_set_default_none(){
	set_default_none(__var_list_tail, __parallel_level__);
}
void cape_set_threadprivate(long addr){
	set_threadprivate(__var_list_head, addr);
}
void cape_set_shared(long addr){
	set_data_attribute(__var_list_tail, addr, CAPE_SHARED, __parallel_level__);
}
void cape_set_private(long addr){
	set_data_attribute(__var_list_tail, addr, CAPE_PRIVATE, __parallel_level__);
}
void cape_set_firstprivate(long addr){
	set_data_attribute(__var_list_tail, addr, CAPE_FIRST_PRIVATE, __parallel_level__);
}    
void cape_set_lastprivate(long addr){
	set_data_attribute(__var_list_tail, addr, CAPE_LAST_PRIVATE, __parallel_level__);
}
void cape_set_reduction(long addr, char op){
	set_data_attribute(__var_list_tail, addr, op , __parallel_level__);
}
void cape_set_copyin(long addr){
	set_data_attribute(__var_list_tail, addr, CAPE_COPY_IN, __parallel_level__);
}
void cape_set_copythread(long addr){
	set_data_attribute(__var_list_tail, addr, CAPE_COPY_PRIVATE, __parallel_level__);
}





/*****************************************/
void print_var_list(VarList *vlist)
{
	printf("PRINT ACTIVE VARIABLE LIST: \n");
	if(vlist != NULL){
		while(vlist!=NULL){
			printf("Node %ld: Variables: Ox%lx - size: %u - n: %u- pro: %d - dtype: %d - level: %d - is pointer: %d \n ", __node__, 	 \
					vlist->var.addr, 
					vlist->var.size, 
					vlist->var.n, 
					vlist->var.pro, 
					vlist->var.dtype,
					vlist->var.level,
					vlist->var.ispointer );
			vlist = vlist->next;
		}
	}
	else
		printf("VarList is NULL \n");
}

void print_var_head_list(PointerList *vlist){
	if(vlist != NULL){
		while(vlist!=NULL){
			printf("\n Node %d: In Heap: Manager addr: Ox%lx - Addr: 0x%lx - %ld bytes \n ", __node__, 	 \
					vlist->pointer.manager_addr, vlist->pointer.addr, vlist->pointer.len);
			vlist = vlist->next;
		}
	}
	else
		printf("VarList is NULL \n");
}



int print_data_in_checkpoint(char *data_ckpt, size_t file_size){
	unsigned long len=0, file_pointer = 0;
	unsigned long addr, pc, size_s, timestamp, i;
	
	timestamp = *(unsigned long *) data_ckpt ;
	pc = *(unsigned long *)(data_ckpt+ 4); //get program counter
	size_s =  *(unsigned long *)(data_ckpt+ 8);
	
	file_pointer = 12 ;
	
	printf("\nNode: %ld - timestamp: %ld - PC: Ox%lx - Size_S: %ld \n", __node__, timestamp, pc, size_s );
	
	while(file_pointer < file_size){
		
		addr = *(long *) (data_ckpt + file_pointer);		
		file_pointer += sizeof(long);		
		
		len = *(unsigned int*) (data_ckpt + file_pointer );
		file_pointer += sizeof(long);			
		
		printf(" ------ Node: %ld - Addr: 0x%lx - Len: %ld - Data: ", __node__, addr, len );
		for(i = 0; i < len/4 ; i++ ){
			printf("\t%d", *(int *) (data_ckpt + file_pointer));
			file_pointer += sizeof(int);
		}
		
		
		//if (__node__ == 0)
		//	printf("DATA: Node %d: addr = 0x%lx - len = %d bytes - data %d \n",__node__, addr, len, *(int *) (data_ckpt+ file_pointer) );	
				
		//file_pointer += len ;			
	}
	
	return 1;
}


/****************************************************************************************
 * public functions
 * **************************************************************************************
 */

/*
 * cape_declare_variable: version 7.0
 * 	Save all active and shared variables in __active_variable list
 *  (global and local variable is declared outside #pragma omp parallel)
 */
int cape_declare_variable(unsigned long addr,
						  unsigned char dtype,
						  unsigned int n_elements,
						  unsigned char ispointer){
							  
	if (__is_inside_parallel_region__) return 0;
	
	Var v;
	v.addr = addr;
	v.dtype = dtype;
	v.n = n_elements;
	v.pro = CAPE_SHARED;
	v.level = __activate_func_level__ ;
	v.ispointer = ispointer;
	unsigned int size;
	if (ispointer){
		size = 4 ;
	}
	else
	{
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
	}
	v.size = size;
	int val;
	val = add_active_variable(&__active_variable_head, &__active_variable_tail, v);	
	return val;
}

/*
 * ---------------------------------------------------------------------
 * Declare a initialization of heap memory
 * v = cape_malloc(size)
 * v = cape_calloc(v, p, size);
 * ---------------------------------------------------------------------
 */
void cape_allocate_memory(unsigned long manager_addr, 
						unsigned long addr, unsigned long nbytes){
	Pointer pt ;	
	pt.manager_addr = manager_addr ;
	pt.addr = addr ;
	pt.len = nbytes;	
	remove_exists_item(&__var_heap_list_head, &__var_heap_list_tail, pt);
	addnew_pointer(&__var_heap_list_head, &__var_heap_list_tail, pt) ;
		
} 
 /*
  *  ---------------------------------------------------------------------
  * Re-allocate heap memory
  * v = cape_reaalloc(p, size)
  * ---------------------------------------------------------------------
  */
void cape_reallocate_memory(unsigned long manager_addr, unsigned long old_addr,
						unsigned long addr, unsigned long nbytes){
	Pointer pt, pt_new ;	
	pt.manager_addr = manager_addr ;
	pt.addr = old_addr ;
	pt.len = 4;	
	remove_exists_item(&__var_heap_list_head, &__var_heap_list_tail, pt);
	
	pt_new.manager_addr = manager_addr;
	pt_new.addr = addr ;
	pt_new.len = nbytes ;
	addnew_pointer(&__var_heap_list_head, &__var_heap_list_tail, pt) ;
		
} 

/*
 * ---------------------------------------------------------------------
 * free(v);
 * ---------------------------------------------------------------------
 */
void cape_free_memory(unsigned long manager_addr){
	remove_heap_variables(&__var_heap_list_head,
						&__var_heap_list_tail,
						manager_addr);	
} 

/*
 * ---------------------------------------------------------------------
 * TODO: Initialize 
 * Version 7.0
 * ---------------------------------------------------------------------
 */
/*
 * Bootstrap helper: exchange UCX worker addresses via files on a shared
 * filesystem.  Each rank writes its address to a file, signals readiness,
 * then waits until every other rank has done the same before reading peers.
 *
 * Files are named  <dir>/cape_ucx_<jobid>_addr_<rank>
 *                  <dir>/cape_ucx_<jobid>_rdy_<rank>
 * so multiple concurrent jobs never collide.
 */
static void ucx_exchange_addresses_via_fs(const char *dir, const char *jobid,
                                          ucp_address_t *local_addr,
                                          size_t local_addr_len)
{
	char path[512];
	FILE *f;

	/* Write own address */
	snprintf(path, sizeof(path), "%s/cape_ucx_%s_addr_%d", dir, jobid, __node__);
	f = fopen(path, "wb");
	if (!f) { perror("CAPE UCX: write addr file"); exit(1); }
	fwrite(&local_addr_len, sizeof(size_t), 1, f);
	fwrite(local_addr, local_addr_len, 1, f);
	fclose(f);

	/* Signal ready */
	snprintf(path, sizeof(path), "%s/cape_ucx_%s_rdy_%d", dir, jobid, __node__);
	f = fopen(path, "w");
	if (!f) { perror("CAPE UCX: write rdy file"); exit(1); }
	fclose(f);

	/* Wait for all peers */
	for (int i = 0; i < __nnodes__; i++) {
		snprintf(path, sizeof(path), "%s/cape_ucx_%s_rdy_%d", dir, jobid, i);
		while (access(path, F_OK) != 0)
			usleep(10000); /* 10 ms */
	}

	/* Create endpoints */
	ucs_status_t st;
	ucp_endpoints = malloc(__nnodes__ * sizeof(ucp_ep_h));
	for (int i = 0; i < __nnodes__; i++) {
		snprintf(path, sizeof(path), "%s/cape_ucx_%s_addr_%d", dir, jobid, i);
		f = fopen(path, "rb");
		if (!f) { perror("CAPE UCX: read peer addr file"); exit(1); }
		size_t peer_len;
		fread(&peer_len, sizeof(size_t), 1, f);
		ucp_address_t *peer_addr = malloc(peer_len);
		fread(peer_addr, peer_len, 1, f);
		fclose(f);

		ucp_ep_params_t ep_params;
		memset(&ep_params, 0, sizeof(ep_params));
		ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
		ep_params.address    = peer_addr;
		st = ucp_ep_create(ucp_worker, &ep_params, &ucp_endpoints[i]);
		free(peer_addr);
		if (st != UCS_OK) {
			fprintf(stderr, "CAPE UCX: ucp_ep_create(%d) failed: %s\n",
			        i, ucs_status_string(st));
			exit(1);
		}
	}
}

void cape_init(){
	/* Keep default CAPE messages on eager path unless user explicitly overrides.
	 * This avoids rendezvous metadata appearing in the receive buffer on some
	 * UCX/IB setups when checkpoint payload is around 10KB. */
	setenv("UCX_RNDV_THRESH", "inf", 0);

	/* ------------------------------------------------------------------
	 * 1. Get rank and size.
	 *    With USE_PMIX: use the PMIx API (preferred, works with OpenMPI).
	 *    Without      : read SLURM_PROCID / SLURM_NTASKS set by srun.
	 * ------------------------------------------------------------------ */
#ifdef USE_PMIX
	PMIX_PROC_CONSTRUCT(&pmix_myproc);
	pmix_status_t pst = PMIx_Init(&pmix_myproc, NULL, 0);
	if (pst != PMIX_SUCCESS) {
		fprintf(stderr, "CAPE UCX: PMIx_Init failed: %s\n",
		        PMIx_Error_string(pst));
		exit(1);
	}
	__node__ = (int)pmix_myproc.rank;

	pmix_proc_t wildcard;
	PMIX_PROC_CONSTRUCT(&wildcard);
	PMIX_LOAD_NSPACE(wildcard.nspace, pmix_myproc.nspace);
	wildcard.rank = PMIX_RANK_WILDCARD;

	pmix_value_t *val;
	pst = PMIx_Get(&wildcard, PMIX_JOB_SIZE, NULL, 0, &val);
	if (pst != PMIX_SUCCESS) {
		fprintf(stderr, "CAPE UCX: PMIx_Get(PMIX_JOB_SIZE) failed: %s\n",
		        PMIx_Error_string(pst));
		exit(1);
	}
	__nnodes__ = (int)val->data.uint32;
	PMIX_VALUE_RELEASE(val);
#else
	/* srun sets SLURM_PROCID and SLURM_NTASKS for every task */
	const char *rank_str = getenv("SLURM_PROCID");
	const char *size_str = getenv("SLURM_NTASKS");
	/* Also accept OpenMPI env vars when running under mpirun locally */
	if (!rank_str) rank_str = getenv("OMPI_COMM_WORLD_RANK");
	if (!size_str) size_str = getenv("OMPI_COMM_WORLD_SIZE");
	if (!rank_str || !size_str) {
		fprintf(stderr, "CAPE UCX: cannot determine rank/size. "
		        "Run via srun (sets SLURM_PROCID/SLURM_NTASKS) "
		        "or compile with -DUSE_PMIX.\n");
		exit(1);
	}
	__node__   = atoi(rank_str);
	__nnodes__ = atoi(size_str);
#endif

	/* ------------------------------------------------------------------
	 * 2. Initialise UCX context.
	 * ------------------------------------------------------------------ */
	ucp_params_t ucp_params;
	memset(&ucp_params, 0, sizeof(ucp_params));
	ucp_params.field_mask   = UCP_PARAM_FIELD_FEATURES
	                        | UCP_PARAM_FIELD_REQUEST_SIZE
	                        | UCP_PARAM_FIELD_REQUEST_INIT;
	ucp_params.features     = UCP_FEATURE_TAG;
	ucp_params.request_size = sizeof(cape_ucx_req_t);
	ucp_params.request_init = cape_ucx_req_init;

	ucp_config_t *config;
	ucp_config_read(NULL, NULL, &config);
	ucs_status_t st = ucp_init(&ucp_params, config, &ucp_context);
	ucp_config_release(config);
	if (st != UCS_OK) {
		fprintf(stderr, "CAPE UCX: ucp_init failed: %s\n",
		        ucs_status_string(st));
		exit(1);
	}

	/* ------------------------------------------------------------------
	 * 3. Create a single-threaded UCX worker.
	 * ------------------------------------------------------------------ */
	ucp_worker_params_t wp;
	memset(&wp, 0, sizeof(wp));
	wp.field_mask  = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
	wp.thread_mode = UCS_THREAD_MODE_SINGLE;
	st = ucp_worker_create(ucp_context, &wp, &ucp_worker);
	if (st != UCS_OK) {
		fprintf(stderr, "CAPE UCX: ucp_worker_create failed: %s\n",
		        ucs_status_string(st));
		exit(1);
	}

	/* ------------------------------------------------------------------
	 * 4. Exchange worker addresses and build endpoints.
	 *    With USE_PMIX: use the PMIx KVS + Fence (in-memory, fast).
	 *    Without      : use files on the shared filesystem.
	 * ------------------------------------------------------------------ */
	ucp_address_t *local_addr;
	size_t         local_addr_len;
	ucp_worker_get_address(ucp_worker, &local_addr, &local_addr_len);

#ifdef USE_PMIX
	char pmix_key[64];
	snprintf(pmix_key, sizeof(pmix_key), "cape_ucx_addr_%d", __node__);

	pmix_value_t kval;
	PMIX_VALUE_CONSTRUCT(&kval);
	kval.type          = PMIX_BYTE_OBJECT;
	kval.data.bo.bytes = (char *)local_addr;
	kval.data.bo.size  = local_addr_len;

	pst = PMIx_Put(PMIX_GLOBAL, pmix_key, &kval);
	if (pst != PMIX_SUCCESS) {
		fprintf(stderr, "CAPE UCX: PMIx_Put failed: %s\n",
		        PMIx_Error_string(pst));
		exit(1);
	}
	PMIx_Commit();
	PMIx_Fence(&wildcard, 1, NULL, 0);

	ucp_endpoints = malloc(__nnodes__ * sizeof(ucp_ep_h));
	for (int i = 0; i < __nnodes__; i++) {
		pmix_proc_t peer;
		PMIX_PROC_CONSTRUCT(&peer);
		PMIX_LOAD_NSPACE(peer.nspace, pmix_myproc.nspace);
		peer.rank = (pmix_rank_t)i;

		snprintf(pmix_key, sizeof(pmix_key), "cape_ucx_addr_%d", i);
		pmix_value_t *peer_val;
		pst = PMIx_Get(&peer, pmix_key, NULL, 0, &peer_val);
		if (pst != PMIX_SUCCESS) {
			fprintf(stderr, "CAPE UCX: PMIx_Get(addr, rank=%d) failed: %s\n",
			        i, PMIx_Error_string(pst));
			exit(1);
		}
		ucp_ep_params_t ep_params;
		memset(&ep_params, 0, sizeof(ep_params));
		ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
		ep_params.address    = (ucp_address_t *)peer_val->data.bo.bytes;
		st = ucp_ep_create(ucp_worker, &ep_params, &ucp_endpoints[i]);
		PMIX_VALUE_RELEASE(peer_val);
		if (st != UCS_OK) {
			fprintf(stderr, "CAPE UCX: ucp_ep_create(%d) failed: %s\n",
			        i, ucs_status_string(st));
			exit(1);
		}
	}
#else
	/* Use shared filesystem for rendezvous.
	 * SLURM_JOB_ID namespaces the files so concurrent jobs don't collide.
	 * SLURM_SUBMIT_DIR is the shared NFS directory sbatch was run from.   */
	const char *jobid  = getenv("SLURM_JOB_ID");
	const char *sharedir = getenv("SLURM_SUBMIT_DIR");
	if (!jobid)    jobid    = "local";
	if (!sharedir) sharedir = ".";
	ucx_exchange_addresses_via_fs(sharedir, jobid, local_addr, local_addr_len);
#endif

	ucp_worker_release_address(ucp_worker, local_addr);
}

/*
 * ---------------------------------------------------------------------
 * Release the UCX environment (and PMIx if enabled).
 * ---------------------------------------------------------------------
 */
void cape_finalize(){
	/* Flush and close all endpoints */
	ucp_request_param_t close_param;
	memset(&close_param, 0, sizeof(close_param));
	for (int i = 0; i < __nnodes__; i++) {
		void *req = ucp_ep_close_nbx(ucp_endpoints[i], &close_param);
		if (UCS_PTR_IS_ERR(req)) {
			/* endpoint may already be in error state – skip */
			continue;
		}
		if (req == NULL)
			continue; /* completed immediately */
		/* Spin until close completes using ucp_request_check_status —
		 * ep close does not fire send/recv callbacks so r->completed
		 * is never set; poll the request status directly instead. */
		while (ucp_request_check_status(req) == UCS_INPROGRESS)
			ucp_worker_progress(ucp_worker);
		ucp_request_free(req);
	}
	free(ucp_endpoints);
	ucp_endpoints = NULL;

	ucp_worker_destroy(ucp_worker);
	ucp_cleanup(ucp_context);

#ifdef USE_PMIX
	PMIx_Finalize(NULL, 0);
#else
	/* Remove this rank's rendezvous files */
	const char *jobid    = getenv("SLURM_JOB_ID");
	const char *sharedir = getenv("SLURM_SUBMIT_DIR");
	if (!jobid)    jobid    = "local";
	if (!sharedir) sharedir = ".";
	char path[512];
	snprintf(path, sizeof(path), "%s/cape_ucx_%s_addr_%d", sharedir, jobid, __node__);
	unlink(path);
	snprintf(path, sizeof(path), "%s/cape_ucx_%s_rdy_%d",  sharedir, jobid, __node__);
	unlink(path);
#endif
}
/*
 * ---------------------------------------------------------------------
 * Setup variables environments for a block
 * ---------------------------------------------------------------------
 */
void cape_begin(unsigned char name_directive, long first, long second){
	switch(name_directive){
		case PARALLEL:
			open_parallel_window();
			break;
		case FOR:
			//ckpt_stop();
			__parallel_level__++;
			add_parallel_region(&__var_list_head, &__var_list_tail, __parallel_level__);
			__left__ = cape_get_left(second, first);
			__right__= cape_get_right(second, first);
			break;
		case FOR_NOWAIT:
			__left__ = cape_get_left(second, first);
			__right__= cape_get_right(second, first);
			break;
		case PARALLEL_FOR:
			open_parallel_window();
			__left__ = cape_get_left(second, first);
			__right__= cape_get_right(second, first);			
			break;
		case CRISTIAL:
			break;
		case SECTIONS_NOWAIT:
			__current_session__ = -1 ;
			break;
		case SECTIONS:
			__parallel_level__++;
			add_parallel_region(&__var_list_head, &__var_list_tail, __parallel_level__);
			__current_session__ = -1 ;
			break;
		default:
			break;
	}	
}

static int cape_sync_checkpoint()
{
	if (require_allreduce(EXIT_CHECKPOINT) < 0) {
		fprintf(stderr, "CAPE: allreduce checkpoint merge failed on node %d\n", __node__);
		return -1;
	}
	if (inject_checkpoint(after_ckpt, after_ckpt_size) < 0) {
		fprintf(stderr, "CAPE: checkpoint injection failed on node %d\n", __node__);
		return -1;
	}
	return 0;
}
/*
 * ---------------------------------------------------------------------
 * Release variables environments of a block
 * ---------------------------------------------------------------------
*/
void cape_end(unsigned char name_directive, unsigned char ops_flag){	
	switch(name_directive){
		case FOR:
			__time_stamp__ = __right__;			
			require_generate_checkpoint(ops_flag);
			ckpt_stop();
			if (cape_sync_checkpoint() < 0) {
				release_checkpoint();
				exit(1);
			}
			release_checkpoint();
			remove_parellel_region(__parallel_level__);
			__parallel_level__ --;
			ckpt_start();
			break;
		case FOR_NOWAIT:
			break;
		case SECTIONS_NOWAIT:
			break;		
		case PARALLEL:
			__time_stamp__ = __pc__;
			require_generate_checkpoint(ops_flag);
			ckpt_stop();
			if (cape_sync_checkpoint() < 0) {
				release_checkpoint();
				exit(1);
			}
			release_checkpoint();
			close_parallel_window();
			break;			
		case PARALLEL_FOR:	
			__time_stamp__ = __right__;
			require_generate_checkpoint(ops_flag);
			ckpt_stop();
			//print_data_in_checkpoint(after_ckpt, after_ckpt_size);
			if (cape_sync_checkpoint() < 0) {
				release_checkpoint();
				exit(1);
			}
			release_checkpoint();
			close_parallel_window();			
			break;
		case SECTIONS:
			__time_stamp__ = __pc__;
			require_generate_checkpoint(ops_flag);
			ckpt_stop();
			if (cape_sync_checkpoint() < 0) {
				release_checkpoint();
				exit(1);
			}
			release_checkpoint();
			remove_parellel_region(__parallel_level__);
			__parallel_level__ --;
			ckpt_start();
			break;
		default:
			break;
	}		
}
/*
 * --------------------------------------------------------------------
 * Copy data into ckpt_data in FILE in memory 
 * Structure of data {(addr, data), ....}
 * ---------------------------------------------------------------------
 */
int ckpt_start(){	
	//openfile
	ckpt_data_stream = open_memstream(&ckpt_data, &ckpt_data_size);		
	int size;		
	if(__var_list_head == NULL) return 0;
		
	VarList *tmp;
	tmp = __var_list_tail;
	
	//move to the first variable of current parallel level
	while((tmp->var.level == __parallel_level__) && (tmp->prev != NULL))
		tmp = tmp->prev ;
	
	while(tmp != NULL){
		if ((tmp->var.pro != CAPE_PRIVATE) &&
			(tmp->var.pro != CAPE_THREAD_PRIVATE)){
		
			fwrite(&tmp->var.addr, sizeof(unsigned long), 1, ckpt_data_stream);
			fwrite(tmp->var.addr, tmp->var.size, tmp->var.n, ckpt_data_stream);
			fflush(ckpt_data_stream);	
			// printf("Write to ckpt data file: Ox%lx - pro: %d  - size : %d \n", tmp->var.addr, tmp->var.pro, \
										sizeof(unsigned long)  + tmp->var.size * tmp->var.n);
		}
		
		tmp = tmp->next;
	}
	
	//printf("Size of CKPT_DATA_FILE: %ld", ckpt_data_size);
	
}
/*
 * --------------------------------------------------------------------
 * Copy data into ckpt_data in FILE in disk 
 * Structure of data {(addr, data), ....}
 * ---------------------------------------------------------------------
 */
int ckpt_start_2(){
	if (__node__< 0) __node__ = 1;
	sprintf(__ckpt_data_file, "ckpt_data%d.tmp", __node__);	
	__ckpt_data = fopen(__ckpt_data_file, "wb+");
	__ckpt_data_size == 0;
		
	int size;
		
	if(__var_list_head == NULL) return 0;
	
	VarList *tmp;
	tmp = __var_list_tail;
	
	//move to the first variable of current parallel level
	while((tmp->var.level == __parallel_level__) && (tmp->prev != NULL))
		tmp = tmp->prev ;
	
	while(tmp != NULL){
		if ((tmp->var.pro != CAPE_PRIVATE) &&
			(tmp->var.pro != CAPE_THREAD_PRIVATE)){			
			fwrite(&tmp->var.addr, sizeof(unsigned long), 1, __ckpt_data);
			size = tmp->var.n * tmp->var.size ;			
			fwrite(tmp->var.addr, tmp->var.size, tmp->var.n, __ckpt_data);
			fflush(__ckpt_data);	
			__ckpt_data_size += sizeof(unsigned long) \							
								+ tmp->var.size * tmp->var.n ; 
			
			printf("Write to ckpt data file: Ox%lx - pro: %d  - size : %d \n", tmp->var.addr, tmp->var.pro, 
										sizeof(unsigned long)  + tmp->var.size * tmp->var.n);
		}
		
		tmp = tmp->next;
	}
	
	printf("Size of CKPT_DATA_FILE: %ld", __ckpt_data_size);
	
}

/*
 * ---------------------------------------------------------------------
 * TODO: close __ckpt_data file and syncronization data
 * ---------------------------------------------------------------------
 */
void ckpt_stop(){
	if (ckpt_data_size > 0){
		fclose(ckpt_data_stream);
		free(ckpt_data);
		ckpt_data_size = 0;	
	}
}

void ckpt_stop_2(){
	fclose(__ckpt_data);
}

/*
 * ---------------------------------------------------------------------
 * Set flush()
 * ---------------------------------------------------------------------
 */
void cape_flush(){		
	require_generate_checkpoint(FALSE);
	ckpt_stop();
	if (cape_sync_checkpoint() < 0) {
		release_checkpoint();
		exit(1);
	}
	release_checkpoint();
	ckpt_start();
}
/*
 * ---------------------------------------------------------------------
 * Set barrier
 * ---------------------------------------------------------------------
 */
void cape_barrier(){
	__time_stamp__  = __node__;
	cape_flush();
}
/* --------------------*/

void CAPE_DEBUG()
{	
	print_var_list(__active_variable_head);	
	printf("\n-----------\n");
	//print_var_list(__active_variable_head);	
	print_var_head_list(__var_heap_list_head);
	
	
}

/*
 * -------------------------------------------------------------------
 * Enter function: v7.0
 * 	Setup function variable of function in activation tree *  
 * --------------------------------------------------------------------
 */
void __enter_func(){
	__activate_func_level__ ++;
}
/*
 * -------------------------------------------------------------------
 * Exit function: v7.0
 * 	Remove parameters and local variables from shared variable list *  
 * --------------------------------------------------------------------
 */
void __exit_func(){
	remove_active_variables(&__active_variable_head,
							&__active_variable_tail,
							__activate_func_level__);	
	__activate_func_level__ --;
}

void require_generate_checkpoint(char ops_flag){

	after_ckpt_stream = generate_checkpoint(__var_list_head,
								__parallel_level__,		\
								EXIT_CHECKPOINT,				\
								ops_flag,						\
								__time_stamp__,		\
								__pc__);
	/* Close the memstream so that after_ckpt becomes a plain malloc'd
	   buffer.  UCX RDMA (rendezvous protocol) needs a stable, registered
	   buffer — keeping the stream open can interfere with that. */
	if (after_ckpt_stream != NULL) {
		fclose(after_ckpt_stream);
		after_ckpt_stream = NULL;
	}
}
