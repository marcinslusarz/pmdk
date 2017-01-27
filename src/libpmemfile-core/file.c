/*
 * Copyright 2016-2017, Intel Corporation
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
 * file.c -- basic file operations
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "callbacks.h"
#include "data.h"
#include "dir.h"
#include "file.h"
#include "inode.h"
#include "inode_array.h"
#include "internal.h"
#include "locks.h"
#include "out.h"
#include "pool.h"
#include "sys_util.h"
#include "util.h"

static bool
is_tmpfile(int flags)
{
#ifdef O_TMPFILE
	return (flags & O_TMPFILE) == O_TMPFILE;
#else
	return false;
#endif
}

/*
 * check_flags -- (internal) open(2) flags tester
 */
static int
check_flags(int flags)
{
	if (flags & O_APPEND) {
		LOG(LSUP, "O_APPEND");
		flags &= ~O_APPEND;
	}

	if (flags & O_ASYNC) {
		LOG(LSUP, "O_ASYNC is not supported");
		errno = EINVAL;
		return -1;
	}

	if (flags & O_CREAT) {
		LOG(LTRC, "O_CREAT");
		flags &= ~O_CREAT;
	}

	// XXX: move to interposing layer
	if (flags & O_CLOEXEC) {
		LOG(LINF, "O_CLOEXEC is always enabled");
		flags &= ~O_CLOEXEC;
	}

	if (flags & O_DIRECT) {
		LOG(LINF, "O_DIRECT is always enabled");
		flags &= ~O_DIRECT;
	}

#ifdef O_TMPFILE
	/* O_TMPFILE contains O_DIRECTORY */
	if ((flags & O_TMPFILE) == O_TMPFILE) {
		LOG(LTRC, "O_TMPFILE");
		flags &= ~O_TMPFILE;
	}
#endif

	if (flags & O_DIRECTORY) {
		LOG(LSUP, "O_DIRECTORY");
		flags &= ~O_DIRECTORY;
	}

	if (flags & O_DSYNC) {
		LOG(LINF, "O_DSYNC is always enabled");
		flags &= ~O_DSYNC;
	}

	if (flags & O_EXCL) {
		LOG(LTRC, "O_EXCL");
		flags &= ~O_EXCL;
	}

	if (flags & O_NOCTTY) {
		LOG(LINF, "O_NOCTTY is always enabled");
		flags &= ~O_NOCTTY;
	}

	if (flags & O_NOATIME) {
		LOG(LTRC, "O_NOATIME");
		flags &= ~O_NOATIME;
	}

	if (flags & O_NOFOLLOW) {
		LOG(LSUP, "O_NOFOLLOW");
		// XXX we don't support symlinks yet, so we can just ignore it
		flags &= ~O_NOFOLLOW;
	}

	if (flags & O_NONBLOCK) {
		LOG(LINF, "O_NONBLOCK is ignored");
		flags &= ~O_NONBLOCK;
	}

	if (flags & O_PATH) {
		LOG(LSUP, "O_PATH is not supported (yet)");
		errno = EINVAL;
		return -1;
	}

	if (flags & O_SYNC) {
		LOG(LINF, "O_SYNC is always enabled");
		flags &= ~O_SYNC;
	}

	if (flags & O_TRUNC) {
		LOG(LTRC, "O_TRUNC");
		flags &= ~O_TRUNC;
	}

	if ((flags & O_ACCMODE) == O_RDONLY) {
		LOG(LTRC, "O_RDONLY");
		flags -= O_RDONLY;
	}

	if ((flags & O_ACCMODE) == O_WRONLY) {
		LOG(LTRC, "O_WRONLY");
		flags -= O_WRONLY;
	}

	if ((flags & O_ACCMODE) == O_RDWR) {
		LOG(LTRC, "O_RDWR");
		flags -= O_RDWR;
	}

	if (flags) {
		ERR("unknown flag 0x%x\n", flags);
		errno = EINVAL;
		return -1;
	}

	return 0;
}

static struct pmemfile_vinode *
create_file(PMEMfilepool *pfp, const char *filename, const char *full_path,
		struct pmemfile_vinode *parent_vinode, int flags, mode_t mode)
{
	struct pmemfile_time t;

	rwlock_tx_wlock(&parent_vinode->rwlock);

	struct pmemfile_vinode *vinode = inode_alloc(pfp, S_IFREG | mode, &t,
			parent_vinode, NULL, filename);

	if (is_tmpfile(flags))
		vinode_orphan(pfp, vinode);
	else
		vinode_add_dirent(pfp, parent_vinode, filename, vinode, &t);

	rwlock_tx_unlock_on_commit(&parent_vinode->rwlock);

	return vinode;
}

static void
open_file(const char *orig_pathname, struct pmemfile_vinode *vinode, int flags)
{
	if ((flags & O_DIRECTORY) && !vinode_is_dir(vinode))
		pmemobj_tx_abort(ENOTDIR);

	if (flags & O_TRUNC) {
		if (!vinode_is_regular_file(vinode)) {
			LOG(LUSR, "truncating non regular file");
			pmemobj_tx_abort(EINVAL);
		}

		if ((flags & O_ACCMODE) == O_RDONLY) {
			LOG(LUSR, "O_TRUNC without write permissions");
			pmemobj_tx_abort(EACCES);
		}

		rwlock_tx_wlock(&vinode->rwlock);

		vinode_truncate(vinode);

		rwlock_tx_unlock_on_commit(&vinode->rwlock);
	}
}

/*
 * _pmemfile_openat -- open file
 */
static PMEMfile *
_pmemfile_openat(PMEMfilepool *pfp, struct pmemfile_vinode *dir,
		const char *pathname, int flags, ...)
{
	LOG(LDBG, "pathname %s flags 0x%x", pathname, flags);

	const char *orig_pathname = pathname;

	if (check_flags(flags))
		return NULL;

	va_list ap;
	va_start(ap, flags);
	mode_t mode = 0;

	/* NOTE: O_TMPFILE contains O_DIRECTORY */
	if ((flags & O_CREAT) || is_tmpfile(flags)) {
		mode = va_arg(ap, mode_t);
		LOG(LDBG, "mode %o", mode);
		mode &= S_IRWXU | S_IRWXG | S_IRWXO |
				S_ISUID | S_ISGID | S_ISVTX;

		if (mode & (S_IXUSR | S_IXGRP | S_IXOTH)) {
			LOG(LSUP, "execute bits are not supported");
			mode = mode & ~(mode_t)(S_IXUSR | S_IXGRP | S_IXOTH);
		}
	}
	va_end(ap);

	int error = 0;
	PMEMfile *file = NULL;

	struct pmemfile_path_info info;

	struct pmemfile_vinode *volatile vparent = NULL;
	struct pmemfile_vinode *volatile vinode;
	traverse_path(pfp, dir, pathname, false, &info, 0);
	vinode = info.vinode;

	if (is_tmpfile(flags)) {
		if (!vinode_is_dir(vinode)) {
			error = ENOTDIR;
			goto end;
		}

		if (info.remaining[0]) {
			error = ENOENT;
			goto end;
		}

		if ((flags & O_ACCMODE) == O_RDONLY) {
			error = EINVAL;
			goto end;
		}
	} else if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
		if (info.remaining[0] == 0) {
			LOG(LUSR, "file %s already exists", pathname);
			error = EEXIST;
			goto end;
		}

		if (!vinode_is_dir(info.vinode)) {
			error = ENOTDIR;
			goto end;
		}

		if (strchr(info.remaining, '/')) {
			error = ENOENT;
			goto end;
		}
	} else if (flags & O_CREAT) {
		if (info.remaining[0] != 0) {
			if (!vinode_is_dir(info.vinode)) {
				error = ENOTDIR;
				goto end;
			}

			if (strchr(info.remaining, '/')) {
				error = ENOENT;
				goto end;
			}
		}
	} else if (info.remaining[0] != 0) {
		if (!vinode_is_dir(info.vinode))
			error = ENOTDIR;
		else
			error = ENOENT;
		goto end;
	}

	if (is_tmpfile(flags)) {
		vparent = vinode;
		vinode = NULL;
	} else if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
		vparent = vinode;
		vinode = NULL;
	} else if (flags & O_CREAT) {
		if (info.remaining[0] != 0) {
			vparent = vinode;
			vinode = NULL;
		}
	}

	TX_BEGIN_CB(pfp->pop, cb_queue, pfp) {
		if (vinode == NULL) {
			vinode = create_file(pfp, info.remaining,
					orig_pathname, vparent, flags, mode);
		} else {
			open_file(orig_pathname, vinode, flags);
		}

		file = Zalloc(sizeof(*file));
		if (!file) {
			pmemobj_tx_abort(errno);
			ASSERT(0);
		}

		file->vinode = vinode;

		if ((flags & O_ACCMODE) == O_RDONLY)
			file->flags = PFILE_READ;
		else if ((flags & O_ACCMODE) == O_WRONLY)
			file->flags = PFILE_WRITE;
		else if ((flags & O_ACCMODE) == O_RDWR)
			file->flags = PFILE_READ | PFILE_WRITE;

		if (flags & O_NOATIME)
			file->flags |= PFILE_NOATIME;
		if (flags & O_APPEND)
			file->flags |= PFILE_APPEND;
	} TX_ONABORT {
		error = errno;
	} TX_END

end:
	if (vparent)
		vinode_unref_tx(pfp, vparent);

	if (error) {
		if (vinode != NULL)
			vinode_unref_tx(pfp, vinode);

		errno = error;
		LOG(LDBG, "!");

		return NULL;
	}

	ASSERT(file != NULL);
	util_mutex_init(&file->mutex, NULL);

	LOG(LDBG, "pathname %s opened inode 0x%lx", orig_pathname,
			file->vinode->inode.oid.off);
	return file;
}

/*
 * pmemfile_openat -- open file
 */
PMEMfile *
pmemfile_openat(PMEMfilepool *pfp, PMEMfile *dir, const char *pathname,
		int flags, ...)
{
	if (!pathname) {
		LOG(LUSR, "NULL pathname");
		errno = ENOENT;
		return NULL;
	}

	va_list ap;
	va_start(ap, flags);
	mode_t mode = 0;
	if ((flags & O_CREAT) || is_tmpfile(flags))
		mode = va_arg(ap, mode_t);
	va_end(ap);

	struct pmemfile_vinode *at;
	bool at_unref;

	at = pool_get_dir_for_path(pfp, dir, pathname, &at_unref);

	PMEMfile *ret = _pmemfile_openat(pfp, at, pathname, flags, mode);

	if (at_unref) {
		int error;
		if (ret == NULL)
			error = errno;

		vinode_unref_tx(pfp, at);

		if (ret == NULL)
			errno = error;
	}

	return ret;
}

/*
 * pmemfile_open -- open file
 */
PMEMfile *
pmemfile_open(PMEMfilepool *pfp, const char *pathname, int flags, ...)
{
	va_list ap;
	va_start(ap, flags);
	mode_t mode = 0;
	if ((flags & O_CREAT) || is_tmpfile(flags))
		mode = va_arg(ap, mode_t);
	va_end(ap);

	return pmemfile_openat(pfp, PMEMFILE_AT_CWD, pathname, flags, mode);
}

/*
 * pmemfile_open_parent -- open a parent directory and return filename
 *
 * Together with *at interfaces it's very useful for path resolution when
 * pmemfile is mounted in place other than "/".
 */
PMEMfile *
pmemfile_open_parent(PMEMfilepool *pfp, PMEMfile *dir, char *path,
		size_t path_size, int flags)
{
	struct pmemfile_vinode *at;
	bool at_unref;

	at = pool_get_dir_for_path(pfp, dir, path, &at_unref);

	struct pmemfile_path_info info;
	traverse_path(pfp, at, path, true, &info, flags);

	struct pmemfile_vinode *parent;
	const char *name;

	if (info.remaining[0] != 0) {
		parent = info.vinode;
		name = info.remaining;
	} else {
		parent = info.parent;
		name = info.name;
	}
	vinode_ref(pfp, parent);

	PMEMfile *ret = Zalloc(sizeof(*ret));
	if (!ret)
		goto end;

	ret->vinode = parent;
	ret->flags = PFILE_READ | PFILE_NOATIME;
	util_mutex_init(&ret->mutex, NULL);
	size_t len = strlen(name);
	if (len >= path_size)
		len = path_size - 1;
	memmove(path, name, len);
	path[len] = 0;

end:
	if (info.vinode)
		vinode_unref_tx(pfp, info.vinode);
	if (info.parent)
		vinode_unref_tx(pfp, info.parent);
	if (info.name)
		free(info.name);
	if (at_unref) {
		int error;
		if (ret == NULL)
			error = errno;

		vinode_unref_tx(pfp, at);

		if (ret == NULL)
			errno = error;
	}

	return ret;
}

/*
 * pmemfile_close -- close file
 */
void
pmemfile_close(PMEMfilepool *pfp, PMEMfile *file)
{
	LOG(LDBG, "inode 0x%lx path %s", file->vinode->inode.oid.off,
			pmfi_path(file->vinode));

	vinode_unref_tx(pfp, file->vinode);

	util_mutex_destroy(&file->mutex);

	Free(file);
}

static int
_pmemfile_linkat(PMEMfilepool *pfp,
		struct pmemfile_vinode *olddir, const char *oldpath,
		struct pmemfile_vinode *newdir, const char *newpath,
		int flags)
{
	LOG(LDBG, "oldpath %s newpath %s", oldpath, newpath);

	flags &= ~AT_SYMLINK_FOLLOW; /* No symlinks for now XXX */

	if (oldpath[0] == 0 && (flags & AT_EMPTY_PATH)) {
		LOG(LSUP, "AT_EMPTY_PATH not supported yet");
		errno = EINVAL;
		return -1;
	}

	flags &= ~AT_EMPTY_PATH;

	if (flags != 0) {
		errno = EINVAL;
		return -1;
	}

	struct pmemfile_path_info src, dst;
	traverse_path(pfp, olddir, oldpath, false, &src, 0);
	traverse_path(pfp, newdir, newpath, false, &dst, 0);

	int error = 0;

	if (src.remaining[0] != 0 && !vinode_is_dir(src.vinode)) {
		error = ENOTDIR;
		goto end;
	}

	if (dst.remaining[0] != 0 && !vinode_is_dir(dst.vinode)) {
		error = ENOTDIR;
		goto end;
	}

	if (src.remaining[0] != 0 || strchr(dst.remaining, '/')) {
		error = ENOENT;
		goto end;
	}

	if (dst.remaining[0] == 0) {
		error = EEXIST;
		goto end;
	}

	if (vinode_is_dir(src.vinode)) {
		error = EPERM;
		goto end;
	}

	util_rwlock_wrlock(&dst.vinode->rwlock);

	TX_BEGIN_CB(pfp->pop, cb_queue, pfp) {
		struct pmemfile_time t;
		file_get_time(&t);
		vinode_add_dirent(pfp, dst.vinode, dst.remaining, src.vinode,
				&t);
	} TX_ONABORT {
		error = errno;
	} TX_END

	util_rwlock_unlock(&dst.vinode->rwlock);

	if (error == 0) {
		vinode_clear_debug_path(pfp, src.vinode);
		vinode_set_debug_path(pfp, dst.vinode, src.vinode, newpath);
	}

end:
	vinode_unref_tx(pfp, dst.vinode);
	vinode_unref_tx(pfp, src.vinode);

	if (error) {
		errno = error;
		return -1;
	}

	return 0;
}

int
pmemfile_linkat(PMEMfilepool *pfp, PMEMfile *olddir, const char *oldpath,
		PMEMfile *newdir, const char *newpath, int flags)
{
	struct pmemfile_vinode *olddir_at, *newdir_at;
	bool olddir_at_unref, newdir_at_unref;

	if (!oldpath || !newpath) {
		LOG(LUSR, "NULL pathname");
		errno = ENOENT;
		return -1;
	}

	olddir_at = pool_get_dir_for_path(pfp, olddir, oldpath,
			&olddir_at_unref);
	newdir_at = pool_get_dir_for_path(pfp, newdir, newpath,
			&newdir_at_unref);

	int ret = _pmemfile_linkat(pfp, olddir_at, oldpath, newdir_at, newpath,
			flags);
	int error;
	if (ret)
		error = errno;

	if (olddir_at_unref)
		vinode_unref_tx(pfp, olddir_at);

	if (newdir_at_unref)
		vinode_unref_tx(pfp, newdir_at);

	if (ret)
		errno = error;

	return ret;
}

/*
 * pmemfile_link -- make a new name for a file
 */
int
pmemfile_link(PMEMfilepool *pfp, const char *oldpath, const char *newpath)
{
	struct pmemfile_vinode *at;

	if (!oldpath || !newpath) {
		LOG(LUSR, "NULL pathname");
		errno = ENOENT;
		return -1;
	}

	if (oldpath[0] == '/' && newpath[0] == '/')
		at = NULL;
	else
		at = pool_get_cwd(pfp);

	int ret = _pmemfile_linkat(pfp, at, oldpath, at, newpath, 0);

	if (at) {
		int error;
		if (ret)
			error = errno;

		vinode_unref_tx(pfp, at);

		if (ret)
			errno = error;
	}

	return ret;
}

static int
_pmemfile_unlinkat(PMEMfilepool *pfp, struct pmemfile_vinode *dir,
		const char *pathname)
{
	LOG(LDBG, "pathname %s", pathname);

	int error = 0;

	struct pmemfile_path_info info;
	traverse_path(pfp, dir, pathname, true, &info, 0);
	struct pmemfile_vinode *vparent = info.parent;
	struct pmemfile_vinode *volatile vinode2 = NULL;
	volatile bool parent_refed = false;

	if (info.remaining[0]) {
		if (!vinode_is_dir(info.vinode))
			error = ENOTDIR;
		else
			error = ENOENT;
		goto end;
	}

	if (vinode_is_dir(info.vinode)) {
		error = EISDIR;
		goto end;
	}

	util_rwlock_wrlock(&vparent->rwlock);

	TX_BEGIN_CB(pfp->pop, cb_queue, pfp) {
		vinode_unlink_dirent(pfp, vparent, info.name, &vinode2,
				&parent_refed, true);
	} TX_ONABORT {
		error = errno;
	} TX_END

	util_rwlock_unlock(&vparent->rwlock);

end:
	if (info.vinode)
		vinode_unref_tx(pfp, info.vinode);
	if (vinode2)
		vinode_unref_tx(pfp, vinode2);
	if (vparent)
		vinode_unref_tx(pfp, vparent);
	if (info.name)
		free(info.name);

	if (error) {
		if (parent_refed)
			vinode_unref_tx(pfp, vparent);
		errno = error;
		return -1;
	}

	return 0;
}

int
pmemfile_unlinkat(PMEMfilepool *pfp, PMEMfile *dir, const char *pathname,
		int flags)
{
	struct pmemfile_vinode *at;
	bool at_unref;

	if (!pathname) {
		errno = ENOENT;
		return -1;
	}

	at = pool_get_dir_for_path(pfp, dir, pathname, &at_unref);

	int ret;

	if (flags & AT_REMOVEDIR)
		ret = _pmemfile_rmdirat(pfp, at, pathname);
	else {
		if (flags != 0) {
			errno = EINVAL;
			ret = -1;
		} else {
			ret = _pmemfile_unlinkat(pfp, at, pathname);
		}
	}

	if (at_unref) {
		int error;
		if (ret)
			error = errno;

		vinode_unref_tx(pfp, at);

		if (ret)
			errno = error;
	}

	return ret;
}

/*
 * pmemfile_unlink -- delete a name and possibly the file it refers to
 */
int
pmemfile_unlink(PMEMfilepool *pfp, const char *pathname)
{
	return pmemfile_unlinkat(pfp, PMEMFILE_AT_CWD, pathname, 0);
}

static int
_pmemfile_renameat2(PMEMfilepool *pfp,
		struct pmemfile_vinode *olddir, const char *oldpath,
		struct pmemfile_vinode *newdir, const char *newpath,
		unsigned flags)
{
	LOG(LDBG, "oldpath %s newpath %s", oldpath, newpath);

	if (flags) {
		LOG(LSUP, "0 flags supported in rename");
		errno = EINVAL;
		return -1;
	}

	struct pmemfile_vinode *volatile dst_unlinked = NULL;
	struct pmemfile_vinode *volatile src_unlinked = NULL;
	volatile bool dst_parent_refed = false;
	volatile bool src_parent_refed = false;

	struct pmemfile_path_info src, dst;
	traverse_path(pfp, olddir, oldpath, true, &src, 0);
	traverse_path(pfp, newdir, newpath, true, &dst, 0);

	int error = 0;

	if (src.remaining[0] != 0 && !vinode_is_dir(src.vinode)) {
		error = ENOTDIR;
		goto end;
	}

	if (dst.remaining[0] != 0 && !vinode_is_dir(dst.vinode)) {
		error = ENOTDIR;
		goto end;
	}

	if (src.remaining[0] != 0 || strchr(dst.remaining, '/')) {
		error = ENOENT;
		goto end;
	}

	struct pmemfile_vinode *src_parent;
	struct pmemfile_vinode *dst_parent;
	bool src_is_dir;
	const char *src_name;
	const char *dst_name;

	src_parent = src.parent;
	src_name = src.name;
	src_is_dir = vinode_is_dir(src.vinode);

	if (src_is_dir) {
		LOG(LSUP, "renaming directories is not supported yet");
		error = ENOTSUP;
		goto end;
	}

	if (dst.remaining[0] == 0) {
		dst_parent = dst.parent;
		dst_name = dst.name;
	} else {
		dst_parent = dst.vinode;
		dst_name = dst.remaining;
	}

	if (src_parent == dst_parent)
		util_rwlock_wrlock(&dst_parent->rwlock);
	else if (src_parent < dst_parent) {
		util_rwlock_wrlock(&src_parent->rwlock);
		util_rwlock_wrlock(&dst_parent->rwlock);
	} else {
		util_rwlock_wrlock(&dst_parent->rwlock);
		util_rwlock_wrlock(&src_parent->rwlock);
	}

	TX_BEGIN_CB(pfp->pop, cb_queue, pfp) {
		// XXX, when src dir == dst dir we can just update dirent,
		// without linking and unlinking

		vinode_unlink_dirent(pfp, dst_parent, dst_name,
				&dst_unlinked, &dst_parent_refed, false);

		struct pmemfile_time t;
		file_get_time(&t);
		vinode_add_dirent(pfp, dst_parent, dst_name, src.vinode, &t);

		vinode_unlink_dirent(pfp, src_parent, src_name,
				&src_unlinked, &src_parent_refed, true);

		if (src_unlinked != src.vinode) // XXX restart?
			pmemobj_tx_abort(ENOENT);

	} TX_ONABORT {
		error = errno;
	} TX_END

	if (src_parent == dst_parent)
		util_rwlock_unlock(&dst_parent->rwlock);
	else {
		util_rwlock_unlock(&src_parent->rwlock);
		util_rwlock_unlock(&dst_parent->rwlock);
	}

	if (dst_parent_refed)
		vinode_unref_tx(pfp, dst_parent);

	if (src_parent_refed)
		vinode_unref_tx(pfp, src_parent);

	if (dst_unlinked)
		vinode_unref_tx(pfp, dst_unlinked);

	if (src_unlinked)
		vinode_unref_tx(pfp, src_unlinked);

	if (error == 0) {
		vinode_clear_debug_path(pfp, src.vinode);
		vinode_set_debug_path(pfp, dst.vinode, src.vinode, newpath);
	}

end:
	vinode_unref_tx(pfp, dst.vinode);
	vinode_unref_tx(pfp, src.vinode);
	if (dst.parent)
		vinode_unref_tx(pfp, dst.parent);
	if (src.parent)
		vinode_unref_tx(pfp, src.parent);
	if (src.name)
		free(src.name);
	if (dst.name)
		free(dst.name);

	if (error) {
		if (dst_parent_refed)
			vinode_unref_tx(pfp, dst.vinode);

		errno = error;
		return -1;
	}

	return 0;
}

int
pmemfile_rename(PMEMfilepool *pfp, const char *old_path, const char *new_path)
{
	struct pmemfile_vinode *at;

	if (!old_path || !new_path) {
		LOG(LUSR, "NULL pathname");
		errno = ENOENT;
		return -1;
	}

	if (old_path[0] == '/' && new_path[0] == '/')
		at = NULL;
	else
		at = pool_get_cwd(pfp);

	int ret = _pmemfile_renameat2(pfp, at, old_path, at, new_path, 0);

	if (at) {
		int error;
		if (ret)
			error = errno;

		vinode_unref_tx(pfp, at);

		if (ret)
			errno = error;
	}

	return ret;
}

int
pmemfile_renameat2(PMEMfilepool *pfp, PMEMfile *old_at, const char *old_path,
		PMEMfile *new_at, const char *new_path, unsigned flags)
{
	struct pmemfile_vinode *olddir_at, *newdir_at;
	bool olddir_at_unref, newdir_at_unref;

	if (!old_path || !new_path) {
		LOG(LUSR, "NULL pathname");
		errno = ENOENT;
		return -1;
	}

	olddir_at = pool_get_dir_for_path(pfp, old_at, old_path,
			&olddir_at_unref);
	newdir_at = pool_get_dir_for_path(pfp, new_at, new_path,
			&newdir_at_unref);

	int ret = _pmemfile_renameat2(pfp, olddir_at, old_path, newdir_at,
			new_path, flags);
	int error;
	if (ret)
		error = errno;

	if (olddir_at_unref)
		vinode_unref_tx(pfp, olddir_at);

	if (newdir_at_unref)
		vinode_unref_tx(pfp, newdir_at);

	if (ret)
		errno = error;

	return ret;
}

int
pmemfile_renameat(PMEMfilepool *pfp, PMEMfile *old_at, const char *old_path,
		PMEMfile *new_at, const char *new_path)
{
	return pmemfile_renameat2(pfp, old_at, old_path, new_at, new_path, 0);
}

static int
_pmemfile_symlinkat(PMEMfilepool *pfp, const char *target,
		struct pmemfile_vinode *dir, const char *linkpath)
{
	LOG(LDBG, "target %s linkpath %s", target, linkpath);

	int error = 0;

	struct pmemfile_path_info info;
	traverse_path(pfp, dir, linkpath, false, &info, 0);
	struct pmemfile_vinode *volatile vinode = NULL;

	struct pmemfile_vinode *vparent = info.vinode;

	if (info.remaining[0] == 0) {
		error = EEXIST;
		goto end;
	}

	if (!vinode_is_dir(vparent)) {
		error = ENOTDIR;
		goto end;
	}

	if (strchr(info.remaining, '/')) {
		error = ENOENT;
		goto end;
	}

	size_t len = strlen(target);
	struct pmemfile_inode *inode;

	if (len >= sizeof(inode->file_data.data)) {
		error = ENAMETOOLONG;
		goto end;
	}

	util_rwlock_wrlock(&vparent->rwlock);

	TX_BEGIN_CB(pfp->pop, cb_queue, pfp) {
		struct pmemfile_time t;

		vinode = inode_alloc(pfp, S_IFLNK | 0777, &t, vparent, NULL,
				info.remaining);
		inode = D_RW(vinode->inode);
		pmemobj_memcpy_persist(pfp->pop, inode->file_data.data, target,
				len);
		inode->size = len;

		vinode_add_dirent(pfp, vparent, info.remaining, vinode, &t);
	} TX_ONABORT {
		error = errno;
	} TX_END

	util_rwlock_unlock(&vparent->rwlock);

end:
	if (info.vinode)
		vinode_unref_tx(pfp, info.vinode);

	if (vinode && error == 0)
		vinode_unref_tx(pfp, vinode);

	if (error) {
		errno = error;
		return -1;
	}

	return 0;
}

int
pmemfile_symlinkat(PMEMfilepool *pfp, const char *target, PMEMfile *newdir,
		const char *linkpath)
{
	struct pmemfile_vinode *at;
	bool at_unref;

	if (!target || !linkpath) {
		errno = ENOENT;
		return -1;
	}

	at = pool_get_dir_for_path(pfp, newdir, linkpath, &at_unref);

	int ret = _pmemfile_symlinkat(pfp, target, at, linkpath);

	if (at_unref) {
		int error;
		if (ret)
			error = errno;

		vinode_unref_tx(pfp, at);

		if (ret)
			errno = error;
	}

	return ret;
}

int
pmemfile_symlink(PMEMfilepool *pfp, const char *target, const char *linkpath)
{
	return pmemfile_symlinkat(pfp, target, PMEMFILE_AT_CWD, linkpath);
}

static ssize_t
_pmemfile_readlinkat(PMEMfilepool *pfp, struct pmemfile_vinode *dir,
		const char *pathname, char *buf, size_t bufsiz)
{
	ssize_t ret;
	struct pmemfile_path_info info;
	traverse_path(pfp, dir, pathname, false, &info, 0);

	if (info.remaining[0] != 0) {
		if (vinode_is_dir(info.vinode))
			errno = ENOENT;
		else
			errno = ENOTDIR;

		ret = -1;
		goto end;
	}

	if (!vinode_is_symlink(info.vinode)) {
		errno = EINVAL;
		ret = -1;
		goto end;
	}

	util_rwlock_rdlock(&info.vinode->rwlock);

	const struct pmemfile_inode *inode = D_RO(info.vinode->inode);
	size_t len = strlen(inode->file_data.data);
	if (len > bufsiz)
		len = bufsiz;
	memcpy(buf, inode->file_data.data, len);
	ret = (ssize_t)len;

	util_rwlock_unlock(&info.vinode->rwlock);

end:
	if (info.vinode)
		vinode_unref_tx(pfp, info.vinode);
	return ret;
}

ssize_t
pmemfile_readlinkat(PMEMfilepool *pfp, PMEMfile *dir, const char *pathname,
		char *buf, size_t bufsiz)
{
	struct pmemfile_vinode *at;
	bool at_unref;

	if (!pathname) {
		errno = ENOENT;
		return -1;
	}

	at = pool_get_dir_for_path(pfp, dir, pathname, &at_unref);

	ssize_t ret = _pmemfile_readlinkat(pfp, at, pathname, buf, bufsiz);

	if (at_unref) {
		/*
		 * initialized only because gcc 6.2 thinks "error" might not be
		 * initialized at the time of writing it back to "errno"
		 */
		int error = 0;
		if (ret < 0)
			error = errno;

		vinode_unref_tx(pfp, at);

		if (ret < 0)
			errno = error;
	}

	return ret;
}

ssize_t
pmemfile_readlink(PMEMfilepool *pfp, const char *pathname, char *buf,
		size_t bufsiz)
{
	return pmemfile_readlinkat(pfp, PMEMFILE_AT_CWD, pathname, buf, bufsiz);
}

int
pmemfile_fcntl(PMEMfilepool *pfp, PMEMfile *file, int cmd, ...)
{
	int ret = 0;

	(void) pfp;
	(void) file;

	switch (cmd) {
		case F_SETLK:
		case F_UNLCK:
			// XXX
			return 0;
		case F_GETFL:
			ret |= O_LARGEFILE;
			if (file->flags & PFILE_APPEND)
				ret |= O_APPEND;
			if (file->flags & PFILE_NOATIME)
				ret |= O_NOATIME;
			if ((file->flags & PFILE_READ) == PFILE_READ)
				ret |= O_RDONLY;
			if ((file->flags & PFILE_WRITE) == PFILE_WRITE)
				ret |= O_WRONLY;
			if ((file->flags & (PFILE_READ | PFILE_WRITE)) ==
					(PFILE_READ | PFILE_WRITE))
				ret |= O_RDWR;
			return ret;
	}

	errno = ENOTSUP;
	return -1;
}

/*
 * pmemfile_stats -- get pool statistics
 */
void
pmemfile_stats(PMEMfilepool *pfp, struct pmemfile_stats *stats)
{
	PMEMoid oid;
	unsigned inodes = 0, dirs = 0, block_arrays = 0, inode_arrays = 0,
			blocks = 0;

	POBJ_FOREACH(pfp->pop, oid) {
		unsigned t = (unsigned)pmemobj_type_num(oid);

		if (t == TOID_TYPE_NUM(struct pmemfile_inode))
			inodes++;
		else if (t == TOID_TYPE_NUM(struct pmemfile_dir))
			dirs++;
		else if (t == TOID_TYPE_NUM(struct pmemfile_block_array))
			block_arrays++;
		else if (t == TOID_TYPE_NUM(struct pmemfile_inode_array))
			inode_arrays++;
		else if (t == TOID_TYPE_NUM(char))
			blocks++;
		else
			FATAL("unknown type %u", t);
	}
	stats->inodes = inodes;
	stats->dirs = dirs;
	stats->block_arrays = block_arrays;
	stats->inode_arrays = inode_arrays;
	stats->blocks = blocks;
}
