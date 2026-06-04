#ifndef CAPE_H
#define CAPE_H

	#include <signal.h>
	#include "uthash.h"
	/*Status of page */
	#define PAGE_WRITE_PROTECTED 1
	#define PAGE_WRITABLE 2
	
	/*Types of checkpoints */
	#define ENTRY_CHECKPOINT 1
	#define EXIT_CHECKPOINT 2
	
	/*FLAGS */
	#define TRUE 1
	#define FALSE 0
	
	/*Size of page, word and double words*/
	#define CAPE_PAGE_SIZE 4096
	#define CAPE_WORD 4 //4 bytes
	#define DOUBLE_CAPE_WORD 8 //8 bytes
	
	/*OpenMP Directives */
	#define PARALLEL 2
	#define FOR 3	
	#define FOR_NOWAIT 4 //Combine for and nowait clause
	#define PARALLEL_FOR 5
	#define SECTIONS 6
	#define SECTIONS_NOWAIT 7
	#define MASTER 8
	#define SINGLE 9
	#define CRISTIAL 10
	#define OTOMIC 11
	
	/*OpenMP Clauses*/
	#define CAPE_THREAD_PRIVATE 1
	#define CAPE_PRIVATE 2
	#define CAPE_FIRST_PRIVATE 3
	#define CAPE_LAST_PRIVATE 4
	#define CAPE_COPY_IN 5
	#define CAPE_COPY_PRIVATE 6
	#define CAPE_SHARED 7
	#define CAPE_SUM 8
	#define CAPE_MUL 9
	#define CAPE_MAX 10
	#define CAPE_MIN 11
	
	
	/*CAPE's Datatypes*/	
	#define CAPE_CHAR 1 			//  This is the traditional ASCII character that is numbered by integers between 0 and 127.
	#define CAPE_UNSIGNED_CHAR 2 	// This is the extended character numbered by integers between 0 and 255.

	#define CAPE_SHORT 5			//  This is a 16-bit integer between -32,768 and 32,767.
	#define CAPE_UNSIGNED_SHORT 6	//  This is a 16-bit positive integer between 0 and 65,535.

	#define CAPE_INT 7				//  This is a 32-bit integer between -2,147,483,648 and 2,147,483,647.
	#define CAPE_LONG 9				// This is the same as CAPE_INT on IA32.

	#define CAPE_UNSIGNED_INT 8		//  This is a 32-bit unsigned integer, i.e., a number between 0 and 4,294,967,295.
	#define CAPE_UNSIGNED_LONG 10	// This is the same as CAPE_UNSIGNED_INT on IA32.

	#define CAPE_FLOAT 11			//  This is a single precision, 32-bit long floating point number.
	#define CAPE_DOUBLE 12			//  This is a double precision, 64-bit long floating point number.
	

	#define handle_error(msg) \
           do { perror(msg); exit(EXIT_FAILURE); } while (0)

	#if DDEBUG
	#define dprintf(fmt, args...) printf(fmt, ## args);
	#else
	#define dprintf(fmt, args...) ;
	#endif
	/* Data types*/
	
	typedef struct Var{
		unsigned long addr; //begin address
		unsigned int size; //Size is caculated by WORD (4bytes) of varabiles
		unsigned int n; //number of elements in current page
		unsigned char pro; //properties 
		unsigned char dtype; //cape data type
		unsigned char level;
		unsigned char ispointer;
	} Var;	
	
	/* Composite hash key for VarList: (addr, level) */
	typedef struct VarHashKey{
		unsigned long addr;
		unsigned char level;
	} VarHashKey;

	typedef struct VarList{
		struct Var var;
		struct VarList *next, *prev ;
		VarHashKey hash_key;       /* key for hash table lookup */
		UT_hash_handle hh;         /* uthash handle */
	} VarList;

	typedef struct Pointer{
		unsigned long manager_addr;
		unsigned long addr;
		unsigned int len;
	} Pointer;

	typedef struct PointerList{
		struct Pointer pointer;
		struct PointerList *next, *prev ;
		UT_hash_handle hh_mgr;     /* hash by manager_addr */
		UT_hash_handle hh_addr;    /* hash by addr */
	} PointerList;
	
	
/*
	typedef struct VarList{
		unsigned long addr; //begin address
		unsigned int size; //Size is caculated by WORD (4bytes) of varabiles
		unsigned int n; //number of elements in current page
		unsigned char pro; //properties 
		unsigned char dtype; //cape data type
		unsigned char level;	
		struct Var *var;
		struct VarList * next, *prev;
	} VarList;

	//Address and properties of pages
	typedef struct Page{
		unsigned long addr;
		unsigned char wp_flag; //Write protected flag (1: Write protected, 0: Normal)	
	} Page;

	typedef struct PageList{
		unsigned long addr;
		unsigned char wp_flag;
		struct PageList *next, *prev;
	} PageList;

	//Page contains data
	typedef struct DPageList{
		unsigned long addr;
		char data[CAPE_PAGE_SIZE];	
		struct DPageList *next, *prev ;
	} DPageList;

*/	
    /*CAPE's Variables */
    
    //level of checkpoints
    extern struct sigaction sa;
    extern int __left__, __right__, __i__;
    extern unsigned long __time_stamp__, __pc__;
    
    extern char buffer[4096];
    
    /*CAPE's Functions */
    void __enter_func();
    void __exit_func();
    void CAPE_DEBUG();
    void cape_init();
    void cape_finalize();
//    void CAPE_Sigsegv_Handler(int sig, siginfo_t *si, void *unused);
    void cape_flush();
    void cape_barrier();
    int cape_declare_variable(void *addr, unsigned char dtype, unsigned int n_elements, unsigned char ispointer);
    void cape_allocate_memory(unsigned long manager_addr, 
						unsigned long addr, unsigned long nbytes);
	void cape_reallocate_memory(unsigned long manager_addr, unsigned long old_addr,
						unsigned long addr, unsigned long nbytes);
	void cape_free_memory(unsigned long addr);
						
    void cape_begin(unsigned char begin_directive, long first, long second);
    void cape_end(unsigned char end_directive, unsigned char ops_flag);
    int ckpt_start();
    void ckpt_stop();
    int ckpt_start_2();
    void ckpt_stop_2();   
      
    //Execution Environment 
    void cape_set_nodes(int nnodes);
    void cape_set_time_stamp(int time_stamp);
    int cape_get_node();
    int cape_get_node_num();
    int cape_get_num_nodes();
    int cape_get_nodes();
    int cape_get_left(int n, int start);
    int cape_get_right(int n, int start);
    int cape_get_token();

    int cape_section();
    
    //Clauses
    void cape_set_default_none();
    void cape_set_threadprivate(void *addr);
    void cape_set_shared(void *addr);
    void cape_set_private(void *addr);
    void cape_set_firstprivate(void *addr);
    void cape_set_lastprivate(void *addr);
    void cape_set_reduction(void *addr, char OP);
    void cape_set_copyin(void *addr);
    void cape_set_copythread(void *addr);
    
    
    
        
    int unlock_memory(unsigned long start_addr, unsigned long len );
    int lock_memory(unsigned long start_addr, unsigned long len );

    /* OpenMP-task closure support: allocate a "task environment" record at
     * a stable cross-rank virtual address (tracked + shared). The transpiler
     * hoists a task's captured shared variables into this struct so the task
     * body no longer touches parent-stack addresses. */
    void *cape_task_env_alloc(size_t len);
 
	
 
 
#endif
