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
 * pmem_movnt_align.c -- unit test for functions with non-temporal stores
 *
 * usage: pmem_movnt_align [C|F|B|S]
 *
 * C - pmem_memcpy_persist()
 * B - pmem_memmove_persist() in backward direction
 * F - pmem_memmove_persist() in forward direction
 * S - pmem_memset_persist()
 */

#include <stdio.h>
#include <string.h>

#include "libpmem.h"
#include "unittest.h"

#define CACHELINE 64
#define N_BYTES 8192

typedef void *(*mem_fn)(void *, const void *, size_t);

static char *src, *dst;
static char *scratch;

/*
 * check_memmove -- invoke check function with pmem_memmove_persist
 */
static void
check_memmove(size_t doff, size_t soff, size_t len)
{
	memset(dst + doff, 1, len);
	memset(src + soff, 0, len);

	pmem_memmove_persist(dst + doff, src + soff, len);

	if (memcmp(dst + doff, src + soff, len))
		UT_FATAL("memcpy/memmove failed");
}

/*
 * check_memmove -- invoke check function with pmem_memcpy_persist
 */
static void
check_memcpy(size_t doff, size_t soff, size_t len)
{
	memset(dst, 2, N_BYTES);
	memset(src, 3, N_BYTES);
	memset(scratch, 2, N_BYTES);

	memset(dst + doff, 1, len);
	memset(src + soff, 0, len);
	memcpy(scratch + doff, src + soff, len);

	pmem_memcpy_persist(dst + doff, src + soff, len);

	if (memcmp(dst, scratch, N_BYTES))
		UT_FATAL("memcpy/memmove failed");
}

/*
 * check_memset -- check pmem_memset_no_drain function
 */
static void
check_memset(size_t off, size_t len)
{
	memset(scratch, 2, N_BYTES);
	memset(scratch + off, 1, len);

	memset(dst, 2, N_BYTES);
	pmem_memset_persist(dst + off, 1, len);

	if (memcmp(dst, scratch, N_BYTES))
		UT_FATAL("memset failed");
}

int
main(int argc, char *argv[])
{
	if (argc != 2)
		UT_FATAL("usage: %s type", argv[0]);

	char type = argv[1][0];
	const char *thr = getenv("PMEM_MOVNT_THRESHOLD");
	const char *avx = getenv("PMEM_AVX");
	const char *avx512f = getenv("PMEM_AVX512F");

	START(argc, argv, "pmem_movnt_align %c %s %savx %savx512f", type,
			thr ? thr : "default",
			avx ? "" : "!",
			avx512f ? "" : "!");

	size_t s;
	switch (type) {
	case 'C': /* memcpy */
		/* mmap with guard pages */
		src = MMAP_ANON_ALIGNED(N_BYTES, 0);
		dst = MMAP_ANON_ALIGNED(N_BYTES, 0);
		if (src == NULL || dst == NULL)
			UT_FATAL("!mmap");

		scratch = MALLOC(N_BYTES);

		/* check memcpy with 0 size */
		check_memcpy(0, 0, 0);

		/* check memcpy with unaligned size */
		for (s = 0; s < CACHELINE; s++)
			check_memcpy(0, 0, N_BYTES - s);

		/* check memcpy with unaligned begin */
		for (s = 0; s < CACHELINE; s++)
			check_memcpy(s, 0, N_BYTES - s);

		/* check memcpy with unaligned begin and end */
		for (s = 0; s < CACHELINE; s++)
			check_memcpy(s, s, N_BYTES - 2 * s);

		MUNMAP_ANON_ALIGNED(src, N_BYTES);
		MUNMAP_ANON_ALIGNED(dst, N_BYTES);
		FREE(scratch);

		break;
	case 'B': /* memmove backward */
		/* mmap with guard pages */
		src = MMAP_ANON_ALIGNED(2 * N_BYTES - 4096, 0);
		dst = src + N_BYTES - 4096;
		if (src == NULL)
			UT_FATAL("!mmap");

		/* check memmove in backward direction with 0 size */
		check_memmove(0, 0, 0);

		/* check memmove in backward direction with unaligned size */
		for (s = 0; s < CACHELINE; s++)
			check_memmove(0, 0, N_BYTES - s);

		/* check memmove in backward direction with unaligned begin */
		for (s = 0; s < CACHELINE; s++)
			check_memmove(s, 0, N_BYTES - s);

		/*
		 * check memmove in backward direction with unaligned begin
		 * and end
		 */
		for (s = 0; s < CACHELINE; s++)
			check_memmove(s, s, N_BYTES - 2 * s);

		MUNMAP_ANON_ALIGNED(src, 2 * N_BYTES - 4096);
		break;
	case 'F': /* memmove forward */
		/* mmap with guard pages */
		dst = MMAP_ANON_ALIGNED(2 * N_BYTES - 4096, 0);
		src = dst + N_BYTES - 4096;
		if (src == NULL)
			UT_FATAL("!mmap");

		/* check memmove in forward direction with 0 size */
		check_memmove(0, 0, 0);

		/* check memmove in forward direction with unaligned size */
		for (s = 0; s < CACHELINE; s++)
			check_memmove(0, 0, N_BYTES - s);

		/* check memmove in forward direction with unaligned begin */
		for (s = 0; s < CACHELINE; s++)
			check_memmove(s, 0, N_BYTES - s);

		/*
		 * check memmove in forward direction with unaligned begin
		 * and end
		 */
		for (s = 0; s < CACHELINE; s++)
			check_memmove(s, s, N_BYTES - 2 * s);

		MUNMAP_ANON_ALIGNED(dst, 2 * N_BYTES - 4096);

		break;
	case 'S': /* memset */
		/* mmap with guard pages */
		dst = MMAP_ANON_ALIGNED(N_BYTES, 0);
		if (dst == NULL)
			UT_FATAL("!mmap");

		scratch = MALLOC(N_BYTES);

		/* check memset with 0 size */
		check_memset(0, 0);

		/* check memset with unaligned size */
		for (s = 0; s < CACHELINE; s++)
			check_memset(0, N_BYTES - s);

		/* check memset with unaligned begin */
		for (s = 0; s < CACHELINE; s++)
			check_memset(s, N_BYTES - s);

		/* check memset with unaligned begin and end */
		for (s = 0; s < CACHELINE; s++)
			check_memset(s, N_BYTES - 2 * s);

		MUNMAP_ANON_ALIGNED(dst, N_BYTES);
		FREE(scratch);

		break;
	default:
		UT_FATAL("!wrong type of test");
		break;
	}

	DONE(NULL);
}
