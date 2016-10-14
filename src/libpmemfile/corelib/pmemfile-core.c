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

/*
 * pmemfile-core.c -- library constructor / destructor
 */

#include "callbacks.h"
#include "data.h"
#include "internal.h"
#include "locks.h"
#include "out.h"
#include "util.h"

#define PMEMFILE_LOG_PREFIX "libpmemfile-core"
#define PMEMFILE_LOG_LEVEL_VAR "PMEMFILECORE_LOG_LEVEL"
#define PMEMFILE_LOG_FILE_VAR "PMEMFILECORE_LOG_FILE"

size_t pmemfile_core_block_size = 2 << 20;
int pmemfile_optimized_list_walk = 0;
int pmemfile_optimized_tree_walk = 1;
int pmemfile_contention_level = 5;
int pmemfile_track_data = 0;
int pmemfile_replace_blocks = 0;

/*
 * libpmemfile_core_init -- load-time initialization for libpmemfile-core
 *
 * Called automatically by the run-time loader.
 */
__attribute__((constructor))
static void
libpmemfile_core_init(void)
{
	out_init(PMEMFILE_LOG_PREFIX, PMEMFILE_LOG_LEVEL_VAR,
			PMEMFILE_LOG_FILE_VAR, PMEMFILE_MAJOR_VERSION,
			PMEMFILE_MINOR_VERSION);
	LOG(LDBG, NULL);
	util_init();
	cb_init();

	char *tmp = getenv("PMEMFILECORE_BLOCK_SIZE");
	if (tmp)
		pmemfile_core_block_size = (size_t)atoll(tmp);
	LOG(LINF, "block size %lu", pmemfile_core_block_size);

	tmp = getenv("PMEMFILECORE_OPT_LIST_WALK");
	if (tmp)
		pmemfile_optimized_list_walk = atoi(tmp);
	LOG(LINF, "optimized list walk %d", pmemfile_optimized_list_walk);

	tmp = getenv("PMEMFILECORE_OPT_TREE_WALK");
	if (tmp)
		pmemfile_optimized_tree_walk = atoi(tmp);
	LOG(LINF, "optimized tree walk %d", pmemfile_optimized_tree_walk);

	tmp = getenv("PMEMFILECORE_CONTENTION_LEVEL");
	if (tmp) {
		pmemfile_contention_level = atoi(tmp);
		if (pmemfile_contention_level < 0 ||
				pmemfile_contention_level > 5)
			FATAL("invalid contention level %d",
					pmemfile_contention_level);
	}
	LOG(LINF, "contention level %d", pmemfile_contention_level);

	tmp = getenv("PMEMFILECORE_TRACK_DATA");
	if (tmp)
		pmemfile_track_data = atoi(tmp);
	LOG(LINF, "track data %d", pmemfile_track_data);

	tmp = getenv("PMEMFILECORE_REPLACE_BLOCKS");
	if (tmp)
		pmemfile_replace_blocks = atoi(tmp);
	LOG(LINF, "replace blocks %d", pmemfile_replace_blocks);
}

/*
 * libpmemfile_core_fini -- libpmemfile-core cleanup routine
 *
 * Called automatically when the process terminates.
 */
__attribute__((destructor))
static void
libpmemfile_core_fini(void)
{
	LOG(LDBG, NULL);
	cb_fini();
	out_fini();
}

/*
 * pmemfile_errormsg -- return last error message
 */
const char *
pmemfile_errormsg(void)
{
	return out_get_errormsg();
}
