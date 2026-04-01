/* 
 * chardev9.h - the header file with the ioctl definitions 
 * The declarations here have to be in a header file, because
 * they need to be known both to the kernel module
 * (in chardev9.c) and the process calling ioctl (ioctl.c) - (monitor)
 */

#ifndef CHARDEV_H
#define CHARDEV_H

#define CHARDEV9_IOC_MAGIC 'k'

#include <linux/ioctl.h>


/* The major device number. We can't rely on dynamic 
 * registration any more, because ioctls need to know it. 
 */
#define MAJOR_NUM 250

/* 
 * IOCTL_WRITE_DATA - Write data to memory 
 * 
 * _IOR means that we're creating an ioctl command 
 * number for passing information from a user process
 * to the kernel module. 
 *
 * The first arguments, MAJOR_NUM, is the major device 
 * number we're using.
 *
 * The second argument is the number of the command 
 * (there could be several with different meanings).
 *
 * The third argument is the type we want to get from 
 * the process to the kernel.
 */
#define IOCTL_WRITE_DATA _IOR(MAJOR_NUM, 0, char *)


/* 
 * OCTL_READ_DATA - Get the message of the device driver 
 *  
 * This IOCTL is used for output, to get the message 
 * of the device driver. However, we still need the 
 * buffer to place the message in to be input, 
 * as it is allocated by the process.
 */
 #define IOCTL_READ_DATA _IOR(MAJOR_NUM, 1, char *)

/* IOCTL_SET_WRITE_PROTECT
 * This IOCTL is used for clearing the write permisson of a page that contains
 * the destination address.
 * Input:
 * 		pid: id of process
 *		dst_addr: an address in the page
 *		src_addr: ignored
 *		len: ignored
 * Output:
 *		0: success
 *		!=0: error
 */
#define IOCTL_SET_WRITE_PROTECT _IOR(MAJOR_NUM, 2, char *)

/* 
 * Set the write permission of a page 
 * This IOCTL is used for setting the write permisson of a page that contains
 * the destination address.
 * Input:
 * 		pid: id of process
 *		dst_addr: an address in the page
 *		src_addr: ignored
 *		len: ignored
 * Output:
 *		0: success
 *		!=0: error
 */
#define IOCTL_CLEAR_WRITE_PROTECT _IOR(MAJOR_NUM, 3, char *)

/* IOCTL_LOCK_PROCESS_MEMORY
 * This IOCTL is used for clear the write permisson of all pages 
 * of a process
 * Input:
 * 		pid: id of process
 *		dst_addr: ignored
 *		src_addr: ignored
 *		len: ignored
 * Output:
 *		0: success
 *		!=0: error
 */
#define IOCTL_LOCK_PROCESS_MEMORY _IOR(MAJOR_NUM, 4, char *)


/* IOCTL_LOCK_MEMORY_RANGE
 * This IOCTL is used for clear the write permisson of a set of pages 
 * of a process
 * Input:
 * 		pid: id of process
 *		dst_addr: begin
 *		src_addr: ignored
 *		len: number of pages
 * Output:
 *		0: success
 *		!=0: error
 */
#define IOCTL_LOCK_MEMORY_RANGE _IOR(MAJOR_NUM, 5, char *)


/* IOCTL_SET_WRITE_PROTECT_ALL_VMA
 * This IOCTL is used for clear the write permisson of all pages in VMA 
 * of a process
 * Input:
 * 		pid: id of process
 *		dst_addr: ignored
 *		src_addr: ignored
 *		len: ignored
 * Output:
 *		0: success
 *		!=0: error
 */
#define IOCTL_SET_WRITE_PROTECT_ALL_VMA _IOR(MAJOR_NUM, 6, char *)

/* 
 * IOCTL_UNLOCK_PROCESS_MEMORY
 * This IOCTL is used for set the write permisson of all pages 
 * of a process
 * Input:
 * 		pid: id of process
 *		dst_addr: ignored
 *		src_addr: ignored
 *		len: ignored
 * Output:
 *		0: success
 *		!=0: error
 */
#define IOCTL_UNLOCK_PROCESS_MEMORY _IOR(MAJOR_NUM, 7, char *)


#define IOCTL_READ_BRK _IOR(MAJOR_NUM, 8, char *)
#define IOCTL_CHANGE_PROCESS_STATE _IOR(MAJOR_NUM, 9, char *)
#define IOCTL_READ_NEXT_PAGE _IOR(MAJOR_NUM, 10, char *)
#define IOCTL_READ_STACK_START _IOR(MAJOR_NUM, 11, char *)
#define IOCTL_READ_ARG_START _IOR(MAJOR_NUM, 12, char *)

#define IOCTL_TEST2 _IOR(MAJOR_NUM, 98, char *)
#define IOCTL_READ_NUM_PTE _IOR(MAJOR_NUM, 97, char *)
 

/* 
 * The name of the device file 
 * Dev name as it appears in /proc/devices
 */
#define DEVICE_NAME "chardev9"	

#define SUCCESS 0
/*
 * Max length of the message from the device
 */
#define BUF_LEN 320


/*
 * pid: id of process
 * dst_addr: an address that contain in the page
 * src_addr:
 * len: number of page
 */
typedef struct{
	unsigned int pid; 
	unsigned long dst_addr;
	unsigned long src_addr;
	int len;
} data_change_t;

#endif
