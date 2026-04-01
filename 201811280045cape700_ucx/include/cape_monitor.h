#define PAGE_SIZE_DIV_4 PAGE_SIZE/4
#define CAPE_WORD 4

//define checkpoint flag
#define ENTRY_CHECKPOINT 1
#define EXIT_CHECKPOINT 2

//define file checkpoint flags
#define FINAL_CHECKPOINT 101
#define WORKSHARE_CHECKPOINTS 102
#define TOTAL_CHECKPOINT 103

//define large checkpoint
#define LARGE_CHECKPOINT 5242880 //5MB

//define a signal that can be used to detect the data structures of checkpoint
#define NA 1 //not apply - use to initialize
#define SD 2  //single data <addr, data>
#define SSD 3 //several successive data <addr, len, data>
#define MD 4 //many data .... <addr, b[0..m], data[0..k*4]>
#define EP 5 // Entire page  <addr, data>

//Data structure of checkpoint for memory in sharing-data variables list
struct shared_data_ckpt{
	unsigned long addr;
	char data[CAPE_WORD];
	struct shared_data_ckpt * prev;	
	struct shared_data_ckpt * next;
};

//Data structure of pages
struct page_node{
	unsigned long addr;
	int data[1024];  //char data[PAGE_SIZE] ???????
	struct page_node * before;
	struct page_node * next;
};


