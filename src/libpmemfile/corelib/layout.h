/*
 * Copyright 2016, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef PMEMFILE_LAYOUT_H
#define PMEMFILE_LAYOUT_H

/*
 * On-media structures.
 */

#include "libpmemobj.h"
#include <stdbool.h>

POBJ_LAYOUT_BEGIN(pmemfile);
POBJ_LAYOUT_ROOT(pmemfile, struct pmemfile_super);
POBJ_LAYOUT_TOID(pmemfile, struct pmemfile_inode);
POBJ_LAYOUT_TOID(pmemfile, struct pmemfile_dir);
POBJ_LAYOUT_TOID(pmemfile, struct pmemfile_block_array);
POBJ_LAYOUT_TOID(pmemfile, struct pmemfile_inode_array);
POBJ_LAYOUT_TOID(pmemfile, char);
POBJ_LAYOUT_END(pmemfile);

struct pmemfile_block {
	TOID(char) data;
	uint64_t size;
};

/* File */
struct pmemfile_block_array {
	TOID(struct pmemfile_block_array) next;

	/* size of the blocks array */
	uint32_t length;

	uint32_t padding;

	struct pmemfile_block blocks[];
};

#define PMEMFILE_MAX_FILE_NAME 255
/* Directory entry */
struct pmemfile_dirent {
	TOID(struct pmemfile_inode) inode;
	char name[PMEMFILE_MAX_FILE_NAME + 1];
};

/* Directory */
struct pmemfile_dir {
	uint64_t num_elements;
	TOID(struct pmemfile_dir) next;
	struct pmemfile_dirent dentries[];
};

struct pmemfile_time {
	/* Seconds */
	int64_t sec;

	/* Nanoseconds */
	int64_t nsec;
};

/* Inode */
struct pmemfile_inode {
	/* Layout version */
	uint32_t version;

	/* Owner */
	uint32_t uid;

	/* Group */
	uint32_t gid;

	/* Padding */
	uint32_t padding;

	/* Time of last access. */
	struct pmemfile_time atime;

	/* Time of last status change. */
	struct pmemfile_time ctime;

	/* Time of last modification. */
	struct pmemfile_time mtime;

	/* Hard link counter. */
	nlink_t nlink;

	/* Size of file. */
	uint64_t size;

	/* File flags. */
	uint64_t flags;

	/* Number of bytes written in the last block */
	uint64_t last_block_fill;

	/* Data! */
	union {
		/* File specific data. */
		struct pmemfile_block_array blocks;

		/* Directory specific data. */
		struct pmemfile_dir dir;

		char padding[4096
				- 4  /* version */
				- 4  /* uid */
				- 4  /* gid */
				- 4  /* padding */
				- 16 /* atime */
				- 16 /* ctime */
				- 16 /* mtime */
				- 8  /* nlink */
				- 8  /* size */
				- 8  /* flags */
				- 8  /* last_block_fill */];
	} file_data;
};

#define NUMINODES_PER_ENTRY 249

struct pmemfile_inode_array {
	PMEMmutex mtx;
	TOID(struct pmemfile_inode_array) prev;
	TOID(struct pmemfile_inode_array) next;

	/* Number of used entries, <0, NUMINODES_PER_ENTRY>. */
	uint64_t used;

	TOID(struct pmemfile_inode) inodes[NUMINODES_PER_ENTRY];
	char padding[8];
};

/* Superblock */
struct pmemfile_super {
	/* XXX unused */
	uint64_t version;

	/* Root directory inode */
	TOID(struct pmemfile_inode) root_inode;

	/* List of arrays of inodes that were deleted, but are still opened. */
	TOID(struct pmemfile_inode_array) orphaned_inodes;

	/* Flag indicating mkfs finished its work. */
	char initialized;

	char padding[4096
			- 8  /* version */
			- 16 /* toid */
			- 16 /* toid */
			- 1  /* init */];
};

#endif