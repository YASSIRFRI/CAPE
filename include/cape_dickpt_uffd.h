#ifndef CAPE_DICKPT_UFFD_H
#define CAPE_DICKPT_UFFD_H

#include <stdint.h>

#define CAPE_DICKPT_ENV_SOCK_FD "CAPE_DICKPT_SOCK_FD"
#define CAPE_DICKPT_MAX_RANGES 64

enum cape_dickpt_ctl_type {
	CAPE_DICKPT_CTL_UFFD_SETUP = 1
};

struct cape_dickpt_range {
	uint64_t start;
	uint64_t len;
};

struct cape_dickpt_ctl_header {
	uint32_t type;
	uint32_t count;
};

#endif
