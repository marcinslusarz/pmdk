/*
 * Copyright 2015-2017, Intel Corporation
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
 * redo.c -- redo log implementation
 */

#include <inttypes.h>
#include <string.h>

#include "redo.h"
#include "out.h"
#include "util.h"
#include "valgrind_internal.h"

/*
 * Finish flag at the least significant bit
 */
#define REDO_FINISH_FLAG	((uint64_t)1<<0)
#define REDO_FLAG_MASK		(~REDO_FINISH_FLAG)

/*
 * assert_addr_cl_aligned -- verify that address is cache-line aligned
 *
 * If it's not algorithm is still correct, but is not optimal.
 */
static inline void
assert_addr_cl_aligned(void *addr)
{
	ASSERTeq(((uintptr_t)addr) & 63, 0);
}

struct redo_ctx {
	void *base;

	struct pmem_ops p_ops;

	redo_check_offset_fn check_offset;
	void *check_offset_ctx;

	unsigned redo_num_entries;
};

/*
 * redo_log_config_new -- allocates redo context
 */
struct redo_ctx *
redo_log_config_new(void *base,
		const struct pmem_ops *p_ops,
		redo_check_offset_fn check_offset,
		void *check_offset_ctx,
		unsigned redo_num_entries)
{
	struct redo_ctx *cfg = Malloc(sizeof(*cfg));
	if (!cfg) {
		ERR("!can't create redo log config");
		return NULL;
	}

	cfg->base = base;
	cfg->p_ops = *p_ops;
	cfg->check_offset = check_offset;
	cfg->check_offset_ctx = check_offset_ctx;
	cfg->redo_num_entries = redo_num_entries;

	return cfg;
}

/*
 * redo_log_config_delete -- frees redo context
 */
void
redo_log_config_delete(struct redo_ctx *ctx)
{
	Free(ctx);
}

struct redo_log_state *
redo_log_state_new(struct redo_ctx *ctx, struct redo_log *redo, size_t size)
{
	struct redo_log_state *state;
	state = Zalloc(sizeof(*state));
	if (!state)
		return NULL;
	state->pmem_data = redo;
	state->vmem_data = Zalloc(size);
	if (!state->vmem_data) {
		Free(state);
		return NULL;
	}
	state->size = size;
	state->ctx = ctx;

	memcpy(state->vmem_data, state->pmem_data, size);
	state->sync = SYNCHRONIZED;

	return state;
}

void
redo_log_state_delete(struct redo_log_state *state)
{
	ASSERTeq(state->sync, SYNCHRONIZED);
	Free(state->vmem_data);
	Free(state);
}

void
redo_log_state_acquire(struct redo_log_state *state)
{
	VALGRIND_ANNOTATE_NEW_MEMORY(state, sizeof(*state));
	if (state->vmem_data)
		VALGRIND_ANNOTATE_NEW_MEMORY(state->vmem_data, state->size);
}

/*
 * redo_log_finish_offset -- (internal) find offset of the finish flag
 */
size_t
redo_log_finish_offset(const struct redo_log *redo, size_t nentries)
{
	for (size_t i = 1; i <= nentries; i++) {
		if (redo[i].offset & REDO_FINISH_FLAG) {
			LOG(15, "redo %p nentries %zu idx %zu", redo, nentries,
					i);
			return i;
		}
	}

	return SIZE_MAX;
}

/*
 * redo_log_store -- (internal) store redo log entry at specified index
 */
void
redo_log_store(struct redo_log_state *redo_state, size_t index,
		uint64_t offset, uint64_t value)
{
	const struct redo_ctx *ctx = redo_state->ctx;
	struct redo_log *pmem_redo = redo_state->pmem_data;
	struct redo_log *vmem_redo = redo_state->vmem_data;

	LOG(15, "redo %p index %zu offset %" PRIu64 " value %" PRIu64,
			pmem_redo, index, offset, value);

	ASSERTne(redo_state->sync, PMEM_NEWER);

	ASSERTeq(offset & REDO_FINISH_FLAG, 0);
	ASSERT(index < ctx->redo_num_entries);

	redo_state->sync = VMEM_NEWER;
	vmem_redo[index + 1].offset = offset;
	vmem_redo[index + 1].value = value;
}

static void
redo_log_calc_csum(struct redo_log *redo, size_t size,
		uint64_t *off, uint64_t *val)
{
	uint64_t csum;
	util_checksum(&redo[1], size * sizeof(*redo), &csum, 1, 0);
	if (csum == 0)
		csum = 1;

	*off = csum;
	*val = csum;
}

static void
redo_log_persist(struct redo_log_state *redo_state, size_t size)
{
	struct redo_log *pmem_redo = redo_state->pmem_data;
	struct redo_log *vmem_redo = redo_state->vmem_data;
	const struct pmem_ops *p_ops = &redo_state->ctx->p_ops;

	ASSERTeq(redo_state->sync, VMEM_NEWER);

	redo_log_calc_csum(vmem_redo, size, &vmem_redo[0].offset,
			&vmem_redo[0].value);

	assert_addr_cl_aligned(pmem_redo);
	size_t dsz =  (size + 1) * sizeof(struct redo_log);
	size_t sz = roundup(dsz, 64U);
	if (sz != dsz)
		memset(((char *)vmem_redo) + dsz, 0xff, sz - dsz);
	pmemops_memcpy(p_ops, pmem_redo, vmem_redo, sz, PMEM_MEM_WC);
	redo_state->sync = SYNCHRONIZED;
}

/*
 * redo_log_store_last -- (internal) store last entry at specified index
 */
void
redo_log_store_last(struct redo_log_state *redo_state,
		size_t index, uint64_t offset, uint64_t value)
{
	const struct redo_ctx *ctx = redo_state->ctx;
	struct redo_log *pmem_redo = redo_state->pmem_data;
	struct redo_log *vmem_redo = redo_state->vmem_data;

	LOG(15, "redo %p index %zu offset %" PRIu64 " value %" PRIu64,
			pmem_redo, index, offset, value);

	ASSERTne(redo_state->sync, PMEM_NEWER);
	ASSERTeq(offset & REDO_FINISH_FLAG, 0);
	ASSERT(index + 1 < ctx->redo_num_entries);

	vmem_redo[index + 1].offset = offset | REDO_FINISH_FLAG;
	vmem_redo[index + 1].value = value;
	redo_state->sync = VMEM_NEWER;

	redo_log_persist(redo_state, index + 1);
}

/*
 * redo_log_set_last -- (internal) set finish flag in specified entry
 */
void
redo_log_set_last(struct redo_log_state *redo_state,
		size_t index)
{
	const struct redo_ctx *ctx = redo_state->ctx;
	struct redo_log *pmem_redo = redo_state->pmem_data;
	struct redo_log *vmem_redo = redo_state->vmem_data;

	LOG(15, "redo %p index %zu", pmem_redo, index);

	ASSERTne(redo_state->sync, PMEM_NEWER);
	ASSERT(index < ctx->redo_num_entries);

	/* set finish flag of last entry and persist */
	vmem_redo[index + 1].offset |= REDO_FINISH_FLAG;
	redo_state->sync = VMEM_NEWER;

	redo_log_persist(redo_state, index + 1);
}

/*
 * redo_log_process -- (internal) process redo log entries
 */
void
redo_log_process(struct redo_log_state *redo_state, size_t nentries)
{
	const struct redo_ctx *ctx = redo_state->ctx;
	struct redo_log *pmem_redo = redo_state->pmem_data;
	struct redo_log *vmem_redo = redo_state->vmem_data;
	const struct pmem_ops *p_ops = &ctx->p_ops;

	LOG(15, "redo %p nentries %zu", pmem_redo, nentries);

	if (redo_state->sync == PMEM_NEWER) {
		memcpy(vmem_redo, pmem_redo,
				(nentries + 1) * sizeof(*pmem_redo));
		redo_state->sync = SYNCHRONIZED;
	}

	ASSERTeq(redo_state->sync, SYNCHRONIZED);

#ifdef DEBUG
	if (redo_log_check(redo_state, nentries) != 0)
		ASSERTeq(redo_log_check(redo_state, nentries), 0);
#endif

	vmem_redo++;
	uint64_t *val;
	while ((vmem_redo->offset & REDO_FINISH_FLAG) == 0) {
		val = (uint64_t *)((uintptr_t)ctx->base + vmem_redo->offset);
		VALGRIND_ADD_TO_TX(val, sizeof(*val));
		*val = vmem_redo->value;
		VALGRIND_REMOVE_FROM_TX(val, sizeof(*val));

		pmemops_flush(p_ops, val, sizeof(uint64_t));

		vmem_redo++;
	}

	uint64_t offset = vmem_redo->offset & REDO_FLAG_MASK;
	val = (uint64_t *)((uintptr_t)ctx->base + offset);
	VALGRIND_ADD_TO_TX(val, sizeof(*val));
	*val = vmem_redo->value;
	VALGRIND_REMOVE_FROM_TX(val, sizeof(*val));

	pmemops_persist(p_ops, val, sizeof(uint64_t));

	assert_addr_cl_aligned(redo_state->pmem_data);
	pmemops_memset(p_ops, pmem_redo, 0, 64, PMEM_MEM_WC);
}

/*
 * redo_log_verify -- returns 0 when redo log is empty, !0 when it's not empty,
 * -1 when checksum is invalid, 1 when checksum is valid
 */
static int
redo_log_verify(struct redo_log *redo, size_t nentries)
{
	/* already processed? */
	if (redo[0].offset == 0 && redo[0].value == 0)
		return 0;

	size_t finish_off = redo_log_finish_offset(redo, nentries);
	/* never used? */
	if (finish_off == SIZE_MAX)
		return 0;

	uint64_t off_csum, val_csum;
	redo_log_calc_csum(redo, finish_off, &off_csum, &val_csum);

	/* partially stored? */
	if (off_csum != redo[0].offset || val_csum != redo[0].value) {
		LOG(7, "partially filled redo log %p", redo);
		return -1;
	}

	return 1;
}

/*
 * redo_log_recover -- (internal) recovery of redo log
 *
 * The redo_log_recover shall be preceded by redo_log_check call.
 */
void
redo_log_recover(struct redo_log_state *redo_state, size_t nentries)
{
	const struct redo_ctx *ctx = redo_state->ctx;
	struct redo_log *redo = redo_state->pmem_data;
	const struct pmem_ops *p_ops = &ctx->p_ops;

	LOG(15, "redo %p nentries %zu", redo, nentries);
	ASSERTne(ctx, NULL);

	int r = redo_log_verify(redo, nentries);
	if (r == 0)
		return;

	if (r == -1) {
		assert_addr_cl_aligned(redo);
		pmemops_memset(p_ops, redo, 0, 64, PMEM_MEM_WC);
		memset(redo_state->vmem_data, 0, 64);
		redo_state->sync = SYNCHRONIZED;
		return;
	}

	redo_log_process(redo_state, nentries);
}

/*
 * redo_log_check -- (internal) check consistency of redo log entries
 */
int
redo_log_check(struct redo_log_state *redo_state, size_t nentries)
{
	const struct redo_ctx *ctx = redo_state->ctx;
	struct redo_log *redo = redo_state->pmem_data;

	LOG(15, "redo %p nentries %zu", redo, nentries);
	ASSERTne(ctx, NULL);

	int r = redo_log_verify(redo, nentries);
	if (r != 1)
		return 0;

	redo++;

	void *cctx = ctx->check_offset_ctx;

	while ((redo->offset & REDO_FINISH_FLAG) == 0) {
		if (!ctx->check_offset(cctx, redo->offset)) {
			LOG(15, "redo %p invalid offset %" PRIu64,
					redo, redo->offset);
			return -1;
		}
		redo++;
	}

	uint64_t offset = redo->offset & REDO_FLAG_MASK;
	if (!ctx->check_offset(cctx, offset)) {
		LOG(15, "redo %p invalid offset %" PRIu64,
		    redo, offset);
		return -1;
	}

	return 0;
}

/*
 * redo_log_offset -- returns offset
 */
uint64_t
redo_log_offset(const struct redo_log *redo)
{
	return redo->offset & REDO_FLAG_MASK;
}

/*
 * redo_log_is_last -- returns 1/0
 */
int
redo_log_is_last(const struct redo_log *redo)
{
	return redo->offset & REDO_FINISH_FLAG;
}

/*
 * redo_get_pmem_ops -- returns pmem_ops
 */
const struct pmem_ops *
redo_get_pmem_ops(const struct redo_ctx *ctx)
{
	return &ctx->p_ops;
}
