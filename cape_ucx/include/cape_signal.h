/* 
 * cape_signal.h - defines signal number
 */
#define S_LOCK_PROCESS_MEMORY 1
#define S_UNLOCK_PROCESS_MEMORY 2

#define S_GENERATE_CHECKPOINT 3
#define S_GENERATE_TOTAL_CHECKPOINT 30
#define S_GENERATE_WORKSHARE_CHECKPOINT 33


#define S_SEND_CHECKPOINT 5
#define S_RECEIVE_CHECKPOINT 6

#define S_INJECT_CHECKPOINT 7
#define S_INJECT_WORKSHARE_CHECKPOINT 77

#define S_MERGE_CHECKPOINT 8
#define S_WAIT_FOR_CHECKPOINT 9

#define S_BROADCAST_CHECKPOINT 10
#define S_SCATTER_CHECKPOINT 11
#define S_ALL_REDUCE 12

#define S_START_SHARE_DATA 15
#define S_END_SHARE_DATA 16

#define S_APP_SEND_NUMBER_OF_JOBS 94
#define S_APP_SEND_TIMESPAN 100

/*
 * Data-Sharing Attributes clauses and construct
 */

#define D_THEAD_PRIVATE 150 //construct

#define D_PRIVATE 200
#define D_FIRST_PRIVATE 201
#define D_LAST_PRIVATE 202
#define D_COPY_IN 204
#define D_SHARED 205

#define D_DEFAULT_SHARED 206
#define D_DEFAULT_NONE 207

//Just define some oprations that is used in NAS Benchmak
#define D_REDUCTION_SUM 210
#define D_REDUCTION_MUL 211
#define D_REDUCTION_MAX 212
#define D_REDUCTION_MIN 213
#define D_REDUCTION_AND 214
#define D_REDUCTION_OR  215
#define D_REDUCTION_XOR 216

/* Declare a reduction variable to the monitor. App packs:
 *   rax = address of the variable
 *   rdx = S_DECLARE_REDUCTION | (datatype << 32) | (op << 40)
 * The monitor appends {addr, datatype, properties=op, len=size_of(datatype)}
 * to data_list_head. Used by merge_bitmap_sections to apply the reduction
 * op when both checkpoints dirty the same word. */
#define S_DECLARE_REDUCTION 220

/* Declare an entire reduction range to the bitmap DICKPT monitor.
 * App packs:
 *   rax = start address
 *   rsi = range length in bytes
 *   rdx = S_DECLARE_REDUCTION_REGION | (datatype << 32) | (op << 40)
 */
#define S_DECLARE_REDUCTION_REGION 221

//Define datatypes of CAPE
#define CAPE_CHAR 1 			//  This is the traditional ASCII character that is numbered by integers between 0 and 127.
#define CAPE_BYTE 3				//  This is an 8-bit positive integer betwee 0 and 255, i.e., a byte.

#define CAPE_UNSIGNED_CHAR 2 	// This is the extended character numbered by integers between 0 and 255.


#define CAPE_SHORT 5			//  This is a 16-bit integer between -32,768 and 32,767.
#define CAPE_UNSIGNED_SHORT 6	//  This is a 16-bit positive integer between 0 and 65,535.

#define CAPE_INT 7				//  This is a 32-bit integer between -2,147,483,648 and 2,147,483,647.
#define CAPE_LONG 9				// This is the same as CAPE_INT on IA32.

#define CAPE_UNSIGNED_INT 8		//  This is a 32-bit unsigned integer, i.e., a number between 0 and 4,294,967,295.
#define CAPE_UNSIGNED_LONG 10	// This is the same as CAPE_UNSIGNED_INT on IA32.

#define CAPE_FLOAT 11			//  This is a single precision, 32-bit long floating point number.


#define CAPE_DOUBLE 12			//  This is a double precision, 64-bit long floating point number.
#define CAPE_LONG_DOUBLE 13		//  This is a quadruple precision, 128-bit long floating point number.
#define CAPE_LONG_LONG_INT 14	//  This is a 64-bit long signed integer, i.e., an integer number between -9,223,372,036,854,775,808 and 9,223,372,036,854,775,807 (this reads: 9 quintillions 223 quadrillions 372 trillions 36 billions 854 millions 775 thousand 8 hundred and seven - not a large sum of money by Microsoft standards).
#define CAPE_LONG_LONG 15		//  Same as CAPE_LONG_LONG_INT.
#define CAPE_FLOAT_INT 16		//  This is a pair of a 32-bit floating point number followed by a 32-bit integer.
#define CAPE_DOUBLE_INT 17		//  This is a pair of a 64-bit floating point number followed by a 32-bit integer.
#define CAPE_LONG_INT 18		//  This is a pair of a long integer (which under IA32 is just a 32-bit integer) followed by a 32-bit integer.
#define CAPE_SHORT_INT 19		//  This is a pair of a 16-bit short integer followed by a 32-bit integer.

struct shared_data{
	unsigned long addr;
	unsigned int len;
	unsigned char datatype;
	unsigned char properties;
	unsigned char level;
	struct shared_data * prev;	
	struct shared_data * next;
};
