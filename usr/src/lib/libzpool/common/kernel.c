/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2008 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 *
 * Portions Copyright 2009 Apple Inc. All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <sys/spa.h>
#include <sys/stat.h>
#ifndef __APPLE__
#include <sys/processor.h>
#endif
#include <sys/zfs_context.h>
#include <sys/zmod.h>
#include <sys/utsname.h>

/*
 * Emulation of kernel services in userland.
 */

uint64_t physmem;
vnode_t *rootdir = (vnode_t *)0xabcd1234;
char hw_serial[11];

#ifndef __APPLE__
struct utsname utsname = {
	"userland", "libzpool", "1", "1", "na"
};
#endif

/*
 * =========================================================================
 * threads
 * =========================================================================
 */
/*ARGSUSED*/
kthread_t *
zk_thread_create(void (*func)(), void *arg)
{
	thread_t tid;

	VERIFY(thr_create(0, 0, (void *(*)(void *))func, arg, THR_DETACHED,
	    &tid) == 0);

	return ((void *)(uintptr_t)tid);
}

#ifndef __APPLE__
/*
 * =========================================================================
 * kstats
 * =========================================================================
 */
/*ARGSUSED*/
kstat_t *
kstat_create(char *module, int instance, char *name, char *class,
    uchar_t type, ulong_t ndata, uchar_t ks_flag)
{
	return (NULL);
}

/*ARGSUSED*/
void
kstat_install(kstat_t *ksp)
{}

/*ARGSUSED*/
void
kstat_delete(kstat_t *ksp)
{}
#endif

/*
 * =========================================================================
 * mutexes
 * =========================================================================
 */
void
zmutex_init(kmutex_t *mp)
{
	mp->m_owner = NULL;
	mp->initialized = B_TRUE;
	(void) _mutex_init(&mp->m_lock, USYNC_THREAD, NULL);
}

void
zmutex_destroy(kmutex_t *mp)
{
	ASSERT(mp->initialized == B_TRUE);
	ASSERT(mp->m_owner == NULL);
	(void) _mutex_destroy(&(mp)->m_lock);
	mp->m_owner = (void *)-1UL;
	mp->initialized = B_FALSE;
}

void
mutex_enter(kmutex_t *mp)
{
	ASSERT(mp->initialized == B_TRUE);
	ASSERT(mp->m_owner != (void *)-1UL);
	ASSERT(mp->m_owner != curthread);
	VERIFY(mutex_lock(&mp->m_lock) == 0);
	ASSERT(mp->m_owner == NULL);
	mp->m_owner = curthread;
}

int
mutex_tryenter(kmutex_t *mp)
{
	ASSERT(mp->initialized == B_TRUE);
	ASSERT(mp->m_owner != (void *)-1UL);
	if (0 == mutex_trylock(&mp->m_lock)) {
		ASSERT(mp->m_owner == NULL);
		mp->m_owner = curthread;
		return (1);
	} else {
		return (0);
	}
}

void
mutex_exit(kmutex_t *mp)
{
	ASSERT(mp->initialized == B_TRUE);
	ASSERT(mutex_owner(mp) == curthread);
	mp->m_owner = NULL;
	VERIFY(mutex_unlock(&mp->m_lock) == 0);
}

void *
mutex_owner(kmutex_t *mp)
{
	ASSERT(mp->initialized == B_TRUE);
	return (mp->m_owner);
}

/*
 * =========================================================================
 * rwlocks
 * =========================================================================
 */
/*ARGSUSED*/
void
rw_init(krwlock_t *rwlp, char *name, int type, void *arg)
{
#ifdef __APPLE__
	VERIFY(rwlock_init(&rwlp->rw_lock, USYNC_THREAD, NULL) == 0);
#else
	rwlock_init(&rwlp->rw_lock, USYNC_THREAD, NULL);
#endif
	rwlp->rw_owner = NULL;
#ifdef __APPLE__
	zmutex_init(&rwlp->mutex);
	rwlp->reader_thr_count = 0;
#endif
	rwlp->initialized = B_TRUE;
}

void
rw_destroy(krwlock_t *rwlp)
{
	rwlock_destroy(&rwlp->rw_lock);
	rwlp->rw_owner = (void *)-1UL;
#ifdef __APPLE__
	zmutex_destroy(&rwlp->mutex);
	rwlp->reader_thr_count = -2;
#endif
	rwlp->initialized = B_FALSE;
}

void
rw_enter(krwlock_t *rwlp, krw_t rw)
{
#ifndef __APPLE__
	ASSERT(!RW_LOCK_HELD(rwlp));
#endif
	ASSERT(rwlp->initialized == B_TRUE);
	ASSERT(rwlp->rw_owner != (void *)-1UL);
	ASSERT(rwlp->rw_owner != curthread);

#ifdef __APPLE__
	if (rw == RW_READER) {
		VERIFY(rw_rdlock(&rwlp->rw_lock) == 0);
		
		mutex_enter(&rwlp->mutex);
		ASSERT(rwlp->reader_thr_count >= 0);
		rwlp->reader_thr_count++;
		mutex_exit(&rwlp->mutex);
		ASSERT(rwlp->rw_owner == NULL);
	} else {
		VERIFY(rw_wrlock(&rwlp->rw_lock) == 0);
		
		ASSERT(rwlp->rw_owner == NULL);
		ASSERT(rwlp->reader_thr_count == 0);
		rwlp->reader_thr_count = -1;
		rwlp->rw_owner = curthread;
	}
#else
	if (rw == RW_READER)
		(void) rw_rdlock(&rwlp->rw_lock);
	else
		(void) rw_wrlock(&rwlp->rw_lock);

	rwlp->rw_owner = curthread;
#endif
}

void
rw_exit(krwlock_t *rwlp)
{
	ASSERT(rwlp->initialized == B_TRUE);
	ASSERT(rwlp->rw_owner != (void *)-1UL);

#ifdef __APPLE__
	if(rwlp->rw_owner == curthread) {
		/* Write locked */
		ASSERT(rwlp->reader_thr_count == -1);
		rwlp->reader_thr_count = 0;
		rwlp->rw_owner = NULL;
	} else {
		/* Read locked */
		ASSERT(rwlp->rw_owner == NULL);
		mutex_enter(&rwlp->mutex);
		ASSERT(rwlp->reader_thr_count >= 1);
		rwlp->reader_thr_count--;
		mutex_exit(&rwlp->mutex);
	}
#else
	rwlp->rw_owner = NULL;
#endif
	(void) rw_unlock(&rwlp->rw_lock);
}

int
rw_tryenter(krwlock_t *rwlp, krw_t rw)
{
	int rv;

	ASSERT(rwlp->initialized == B_TRUE);
	ASSERT(rwlp->rw_owner != (void *)-1UL);
#ifdef __APPLE__
	ASSERT(rwlp->rw_owner != curthread);
#endif

	if (rw == RW_READER)
		rv = rw_tryrdlock(&rwlp->rw_lock);
	else
		rv = rw_trywrlock(&rwlp->rw_lock);

	if (rv == 0) {
#ifdef __APPLE__
		if(rw == RW_READER) {
			mutex_enter(&rwlp->mutex);
			ASSERT(rwlp->reader_thr_count >= 0);
			rwlp->reader_thr_count++;
			mutex_exit(&rwlp->mutex);
			ASSERT(rwlp->rw_owner == NULL);
		} else {
			ASSERT(rwlp->rw_owner == NULL);
			ASSERT(rwlp->reader_thr_count == 0);
			rwlp->reader_thr_count = -1;
			rwlp->rw_owner = curthread;
		}
#else
		rwlp->rw_owner = curthread;
#endif
		return (1);
	}

	return (0);
}

/*ARGSUSED*/
int
rw_tryupgrade(krwlock_t *rwlp)
{
	ASSERT(rwlp->initialized == B_TRUE);
	ASSERT(rwlp->rw_owner != (void *)-1UL);

	return (0);
}

/*
 * =========================================================================
 * condition variables
 * =========================================================================
 */
/*ARGSUSED*/
void
cv_init(kcondvar_t *cv, char *name, int type, void *arg)
{
#ifdef __APPLE__
	ASSERT(type == CV_DEFAULT);
#endif
	VERIFY(cond_init(cv, type, NULL) == 0);
}

void
cv_destroy(kcondvar_t *cv)
{
#ifdef __APPLE__
	int ret = cond_destroy(cv);
	VERIFY(ret == 0 || ret == EINVAL); /* XXX/ztest: ok to ignore EINVAL? */
#else
	VERIFY(cond_destroy(cv) == 0);
#endif
}

void
cv_wait(kcondvar_t *cv, kmutex_t *mp)
{
	ASSERT(mutex_owner(mp) == curthread);
	mp->m_owner = NULL;
	int ret = cond_wait(cv, &mp->m_lock);
	VERIFY(ret == 0 || ret == EINTR);
	mp->m_owner = curthread;
}

clock_t
cv_timedwait(kcondvar_t *cv, kmutex_t *mp, clock_t abstime)
{
	int error;
	timestruc_t ts;
	clock_t delta;
#ifdef __APPLE__
	struct timeval tv;
	uint64_t dsec;
#endif

top:
	delta = abstime - lbolt;
	if (delta <= 0)
		return (-1);

#ifdef __APPLE__
	VERIFY(gettimeofday(&tv, NULL) == 0);
	
	dsec = MAX(1, delta / hz);
	ASSERT(dsec >= 1);
	ts.tv_sec = tv.tv_sec + dsec;
	ts.tv_nsec = tv.tv_usec * 1000 + (delta % hz) * (NANOSEC / hz);
	ASSERT(ts.tv_nsec >= 0);
	
	if (ts.tv_nsec >= NANOSEC) {
		ts.tv_sec++;
		ts.tv_nsec -= NANOSEC;
	}
#else
	ts.tv_sec = delta / hz;
	ts.tv_nsec = (delta % hz) * (NANOSEC / hz);
#endif

	ASSERT(mutex_owner(mp) == curthread);
	mp->m_owner = NULL;
#ifdef __APPLE__
	error = cond_timedwait(cv, &mp->m_lock, &ts);
#else
	error = cond_reltimedwait(cv, &mp->m_lock, &ts);
#endif
	mp->m_owner = curthread;

#ifdef __APPLE__
	if (error == ETIMEDOUT)
		return (-1);
#else
	if (error == ETIME)
		return (-1);
#endif

	if (error == EINTR)
		goto top;

	ASSERT(error == 0);

	return (1);
}

void
cv_signal(kcondvar_t *cv)
{
	VERIFY(cond_signal(cv) == 0);
}

void
cv_broadcast(kcondvar_t *cv)
{
	VERIFY(cond_broadcast(cv) == 0);
}

/*
 * =========================================================================
 * vnode operations
 * =========================================================================
 */
/*
 * Note: for the xxxat() versions of these functions, we assume that the
 * starting vp is always rootdir (which is true for spa_directory.c, the only
 * ZFS consumer of these interfaces).  We assert this is true, and then emulate
 * them by adding '/' in front of the path.
 */

/*ARGSUSED*/
int
vn_open(char *path, int x1, int flags, int mode, vnode_t **vpp, int x2, int x3)
{
	int fd;
	vnode_t *vp;
	int old_umask;
	char realpath[MAXPATHLEN];
#if _DARWIN_FEATURE_64_BIT_INODE
	struct stat st;
#else
	struct stat64 st;
#endif

	/*
	 * If we're accessing a real disk from userland, we need to use
	 * the character interface to avoid caching.  This is particularly
	 * important if we're trying to look at a real in-kernel storage
	 * pool from userland, e.g. via zdb, because otherwise we won't
	 * see the changes occurring under the segmap cache.
	 * On the other hand, the stupid character device returns zero
	 * for its size.  So -- gag -- we open the block device to get
	 * its size, and remember it for subsequent VOP_GETATTR().
	 */
	if (strncmp(path, "/dev/", 5) == 0) {
		char *dsk;
		fd = open64(path, O_RDONLY);
		if (fd == -1)
			return (errno);
#if _DARWIN_FEATURE_64_BIT_INODE
		if (fstat(fd, &st) == -1) {
#else
		if (fstat64(fd, &st) == -1) {
#endif
			close(fd);
			return (errno);
		}
		close(fd);
		(void) sprintf(realpath, "%s", path);
		dsk = strstr(path, "/dsk/");
		if (dsk != NULL)
			(void) sprintf(realpath + (dsk - path) + 1, "r%s",
			    dsk + 1);
	} else {
		(void) sprintf(realpath, "%s", path);
#if _DARWIN_FEATURE_64_BIT_INODE
		if (!(flags & FCREAT) && stat(realpath, &st) == -1)
#else
		if (!(flags & FCREAT) && stat64(realpath, &st) == -1)
#endif
			return (errno);
	}

	if (flags & FCREAT)
		old_umask = umask(0);

	/*
	 * The construct 'flags - FREAD' conveniently maps combinations of
	 * FREAD and FWRITE to the corresponding O_RDONLY, O_WRONLY, and O_RDWR.
	 */
	fd = open64(realpath, flags - FREAD, mode);

	if (flags & FCREAT)
		(void) umask(old_umask);

	if (fd == -1)
		return (errno);

#if _DARWIN_FEATURE_64_BIT_INODE
	if (fstat(fd, &st) == -1) {		
#else
	if (fstat64(fd, &st) == -1) {
#endif
		close(fd);
		return (errno);
	}

/*
 * OSX fstat on a block device will return an st_size of 0 instead of
 * the actual size of the device.  So we need to ioctl directly to the disk
 * instead in order to get its size
 */
#ifdef __APPLE__
      if (S_ISBLK(st.st_mode)) {
		  if ((st.st_size = get_disk_size(fd)) == -1) {
			  st.st_size = 0;
			  return (errno);
		  }
	  }
#endif

	(void) fcntl(fd, F_SETFD, FD_CLOEXEC);

	*vpp = vp = umem_zalloc(sizeof (vnode_t), UMEM_NOFAIL);

	vp->v_fd = fd;
	vp->v_size = st.st_size;
	vp->v_path = spa_strdup(path);

	return (0);
}

int
vn_openat(char *path, int x1, int flags, int mode, vnode_t **vpp, int x2,
    int x3, vnode_t *startvp)
{
	char *realpath = umem_alloc(strlen(path) + 2, UMEM_NOFAIL);
	int ret;

	ASSERT(startvp == rootdir);
	(void) sprintf(realpath, "/%s", path);

	ret = vn_open(realpath, x1, flags, mode, vpp, x2, x3);

	umem_free(realpath, strlen(path) + 2);

	return (ret);
}

/*ARGSUSED*/
int
vn_rdwr(int uio, vnode_t *vp, void *addr, ssize_t len, offset_t offset,
	int x1, int x2, rlim64_t x3, void *x4, ssize_t *residp)
{
	ssize_t iolen, split;

	if (uio == UIO_READ) {
		iolen = pread64(vp->v_fd, addr, len, offset);
	} else {
		/*
		 * To simulate partial disk writes, we split writes into two
		 * system calls so that the process can be killed in between.
		 */
		split = (len > 0 ? rand() % len : 0);
		iolen = pwrite64(vp->v_fd, addr, split, offset);
		iolen += pwrite64(vp->v_fd, (char *)addr + split,
		    len - split, offset + split);
	}

	if (iolen == -1)
		return (errno);
	if (residp)
		*residp = len - iolen;
	else if (iolen != len)
		return (EIO);
	return (0);
}

void
vn_close(vnode_t *vp)
{
	close(vp->v_fd);
	spa_strfree(vp->v_path);
	umem_free(vp, sizeof (vnode_t));
}

#ifdef ZFS_DEBUG

/*
 * =========================================================================
 * Figure out which debugging statements to print
 * =========================================================================
 */

static char *dprintf_string;
static int dprintf_print_all;

int
dprintf_find_string(const char *string)
{
	char *tmp_str = dprintf_string;
	int len = strlen(string);

	/*
	 * Find out if this is a string we want to print.
	 * String format: file1.c,function_name1,file2.c,file3.c
	 */

	while (tmp_str != NULL) {
		if (strncmp(tmp_str, string, len) == 0 &&
		    (tmp_str[len] == ',' || tmp_str[len] == '\0'))
			return (1);
		tmp_str = strchr(tmp_str, ',');
		if (tmp_str != NULL)
			tmp_str++; /* Get rid of , */
	}
	return (0);
}

void
dprintf_setup(int *argc, char **argv)
{
	int i, j;

	/*
	 * Debugging can be specified two ways: by setting the
	 * environment variable ZFS_DEBUG, or by including a
	 * "debug=..."  argument on the command line.  The command
	 * line setting overrides the environment variable.
	 */

	for (i = 1; i < *argc; i++) {
		int len = strlen("debug=");
		/* First look for a command line argument */
		if (strncmp("debug=", argv[i], len) == 0) {
			dprintf_string = argv[i] + len;
			/* Remove from args */
			for (j = i; j < *argc; j++)
				argv[j] = argv[j+1];
			argv[j] = NULL;
			(*argc)--;
		}
	}

	if (dprintf_string == NULL) {
		/* Look for ZFS_DEBUG environment variable */
		dprintf_string = getenv("ZFS_DEBUG");
	}

	/*
	 * Are we just turning on all debugging?
	 */
	if (dprintf_find_string("on"))
		dprintf_print_all = 1;
}

/*
 * =========================================================================
 * debug printfs
 * =========================================================================
 */
void
__dprintf(const char *file, const char *func, int line, const char *fmt, ...)
{
	const char *newfile;
	va_list adx;

	/*
	 * Get rid of annoying "../common/" prefix to filename.
	 */
	newfile = strrchr(file, '/');
	if (newfile != NULL) {
		newfile = newfile + 1; /* Get rid of leading / */
	} else {
		newfile = file;
	}

	if (dprintf_print_all ||
	    dprintf_find_string(newfile) ||
	    dprintf_find_string(func)) {
		/* Print out just the function name if requested */
		flockfile(stdout);
		if (dprintf_find_string("pid"))
			(void) printf("%d ", getpid());
		if (dprintf_find_string("tid"))
			(void) printf("%u ", thr_self());
		#ifndef __APPLE__
		if (dprintf_find_string("cpu"))
			(void) printf("%u ", getcpuid());
		#endif
		if (dprintf_find_string("time"))
			(void) printf("%llu ", gethrtime());
		if (dprintf_find_string("long"))
			(void) printf("%s, line %d: ", newfile, line);
		(void) printf("%s: ", func);
		va_start(adx, fmt);
		(void) vprintf(fmt, adx);
		va_end(adx);
		funlockfile(stdout);
	}
}

#endif /* ZFS_DEBUG */

/*
 * =========================================================================
 * cmn_err() and panic()
 * =========================================================================
 */
static char ce_prefix[CE_IGNORE][10] = { "", "NOTICE: ", "WARNING: ", "" };
static char ce_suffix[CE_IGNORE][2] = { "", "\n", "\n", "" };

void
vpanic(const char *fmt, va_list adx)
{
	(void) fprintf(stderr, "error: ");
	(void) vfprintf(stderr, fmt, adx);
	(void) fprintf(stderr, "\n");

	abort();	/* think of it as a "user-level crash dump" */
}

void
panic(const char *fmt, ...)
{
	va_list adx;

	va_start(adx, fmt);
	vpanic(fmt, adx);
	va_end(adx);
}

void
vcmn_err(int ce, const char *fmt, va_list adx)
{
	if (ce == CE_PANIC)
		vpanic(fmt, adx);
	if (ce != CE_NOTE) {	/* suppress noise in userland stress testing */
		(void) fprintf(stderr, "%s", ce_prefix[ce]);
		(void) vfprintf(stderr, fmt, adx);
		(void) fprintf(stderr, "%s", ce_suffix[ce]);
	}
}

/*PRINTFLIKE2*/
void
cmn_err(int ce, const char *fmt, ...)
{
	va_list adx;

	va_start(adx, fmt);
	vcmn_err(ce, fmt, adx);
	va_end(adx);
}

/*
 * =========================================================================
 * kobj interfaces
 * =========================================================================
 */
struct _buf *
kobj_open_file(char *name)
{
	struct _buf *file;
	vnode_t *vp;

	/* set vp as the _fd field of the file */
	if (vn_openat(name, UIO_SYSSPACE, FREAD, 0, &vp, 0, 0, rootdir) != 0)
		return ((void *)-1UL);

	file = umem_zalloc(sizeof (struct _buf), UMEM_NOFAIL);
#ifdef __APPLE__
	file->_fd = vp;
#else
	file->_fd = (intptr_t)vp;
#endif
	return (file);
}

int
kobj_read_file(struct _buf *file, char *buf, unsigned size, unsigned off)
{
	ssize_t resid;

	vn_rdwr(UIO_READ, (vnode_t *)file->_fd, buf, size, (offset_t)off,
	    UIO_SYSSPACE, 0, 0, 0, &resid);

	return (size - resid);
}

void
kobj_close_file(struct _buf *file)
{
	vn_close((vnode_t *)file->_fd);
	umem_free(file, sizeof (struct _buf));
}

int
kobj_get_filesize(struct _buf *file, uint64_t *size)
{
#if _DARWIN_FEATURE_64_BIT_INODE
	struct stat st;
#else
	struct stat64 st;
#endif
	vnode_t *vp = (vnode_t *)file->_fd;

#if _DARWIN_FEATURE_64_BIT_INODE
	if (fstat(vp->v_fd, &st) == -1) {	
#else
	if (fstat64(vp->v_fd, &st) == -1) {
#endif
		vn_close(vp);
		return (errno);
	}
	*size = st.st_size;
	return (0);
}

/*
 * =========================================================================
 * misc routines
 * =========================================================================
 */

void
delay(clock_t ticks)
{
	poll(0, 0, ticks * (1000 / hz));
}

/*
 * Find highest one bit set.
 *	Returns bit number + 1 of highest bit that is set, otherwise returns 0.
 * High order bit is 31 (or 63 in _LP64 kernel).
 */
int
highbit(ulong_t i)
{
	register int h = 1;

	if (i == 0)
		return (0);
#ifdef _LP64
	if (i & 0xffffffff00000000ul) {
		h += 32; i >>= 32;
	}
#endif
	if (i & 0xffff0000) {
		h += 16; i >>= 16;
	}
	if (i & 0xff00) {
		h += 8; i >>= 8;
	}
	if (i & 0xf0) {
		h += 4; i >>= 4;
	}
	if (i & 0xc) {
		h += 2; i >>= 2;
	}
	if (i & 0x2) {
		h += 1;
	}
	return (h);
}

static int
random_get_bytes_common(uint8_t *ptr, size_t len, char *devname)
{
	int fd = open(devname, O_RDONLY);
	size_t resid = len;
	ssize_t bytes;

	ASSERT(fd != -1);

	while (resid != 0) {
		bytes = read(fd, ptr, resid);
		ASSERT(bytes >= 0);
		ptr += bytes;
		resid -= bytes;
	}

	close(fd);

	return (0);
}

int
random_get_bytes(uint8_t *ptr, size_t len)
{
	return (random_get_bytes_common(ptr, len, "/dev/random"));
}

int
random_get_pseudo_bytes(uint8_t *ptr, size_t len)
{
	return (random_get_bytes_common(ptr, len, "/dev/urandom"));
}

#ifndef __APPLE__
int
ddi_strtoul(const char *hw_serial, char **nptr, int base, unsigned long *result)
{
	char *end;

	*result = strtoul(hw_serial, &end, base);
	if (*result == 0)
		return (errno);
	return (0);
}
#endif

/*
 * =========================================================================
 * kernel emulation setup & teardown
 * =========================================================================
 */
static int
umem_out_of_memory(void)
{
	char errmsg[] = "out of memory -- generating core dump\n";

	write(fileno(stderr), errmsg, sizeof (errmsg));
	abort();
	return (0);
}

void
kernel_init(int mode)
{
	pthread_mutex_init(&zfs_global_atomic_mutex, 0);
	umem_nofail_callback(umem_out_of_memory);

#ifdef __APPLE__
	int mib[2] = {CTL_HW, HW_MEMSIZE};
	u_int mib_array_size = sizeof(mib)/sizeof(mib[0]);
	uint64_t memsize;
	size_t len = sizeof(memsize);
	
	if (sysctl(mib, mib_array_size, &memsize, &len, NULL, 0) == 0) {
		physmem = memsize / sysconf(_SC_PAGE_SIZE);	
		dprintf("physmem = %llu pages (%.2f GB)\n", physmem,
	    (double)memsize / (1ULL << 30));
	} else {
		dprintf("Couldn't determine the physical memory with sysctl\n");
	}
#else
	physmem = sysconf(_SC_PHYS_PAGES);

	dprintf("physmem = %llu pages (%.2f GB)\n", physmem,
	    (double)physmem * sysconf(_SC_PAGE_SIZE) / (1ULL << 30));
#endif

	snprintf(hw_serial, sizeof (hw_serial), "%ld", gethostid());

	spa_init(mode);
}

void
kernel_fini(void)
{
	spa_fini();
}

//#ifndef __APPLE__
int
z_uncompress(void *dst, size_t *dstlen, const void *src, size_t srclen)
{
	int ret;
	uLongf len = *dstlen;

	if ((ret = uncompress(dst, &len, src, srclen)) == Z_OK)
		*dstlen = (size_t)len;

	return (ret);
}

int
z_compress_level(void *dst, size_t *dstlen, const void *src, size_t srclen,
    int level)
{
	int ret;
	uLongf len = *dstlen;

	if ((ret = compress2(dst, &len, src, srclen, level)) == Z_OK)
		*dstlen = (size_t)len;

	return (ret);
}
//#endif

uid_t
crgetuid(cred_t *cr)
{
	return (0);
}

gid_t
crgetgid(cred_t *cr)
{
	return (0);
}

#ifndef __APPLE__
int
crgetngroups(cred_t *cr)
{
	return (0);
}

gid_t *
crgetgroups(cred_t *cr)
{
	return (NULL);
}
#endif

int
zfs_secpolicy_snapshot_perms(const char *name, cred_t *cr)
{
	return (0);
}

int
zfs_secpolicy_rename_perms(const char *from, const char *to, cred_t *cr)
{
	return (0);
}

#ifndef __APPLE__
int
zfs_secpolicy_destroy_perms(const char *name, cred_t *cr)
{
	return (0);
}

#endif
