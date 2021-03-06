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
 * Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 * Portions Copyright 2007 Apple Inc. All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#include <sys/types.h>
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/uio.h>
#include <sys/buf.h>
#ifndef __APPLE__
#include <sys/modctl.h>
#include <sys/open.h>
#endif /* !__APPLE__ */
#include <sys/file.h>
#include <sys/kmem.h>
#include <sys/conf.h>
#include <sys/cmn_err.h>
#include <sys/stat.h>
#include <sys/zfs_ioctl.h>
#include <sys/zap.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev.h>
#include <sys/vdev_impl.h>
#include <sys/dmu.h>
#include <sys/dsl_dir.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_prop.h>
#include <sys/dsl_deleg.h>
#include <sys/dmu_objset.h>
#ifndef __APPLE__
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/sunldi.h>
#include <sys/policy.h>
#include <sys/zone.h>
#endif /* !__APPLE__ */
#include <sys/nvpair.h>
#include <sys/pathname.h>
#include <sys/mount.h>
#ifndef __APPLE__
#include <sys/sdt.h>
#endif /* !__APPLE__ */
#include <sys/fs/zfs.h>
#include <sys/zfs_ctldir.h>
#include <sys/zvol.h>
#ifdef __APPLE__
#include <miscfs/devfs/devfs.h>
#else
#include <sharefs/share.h>
#endif /* __APPLE__ */
#include <sys/zfs_znode.h>

#include "zfs_namecheck.h"
#include "zfs_prop.h"
#include "zfs_deleg.h"

#ifndef __APPLE__
extern struct modlfs zfs_modlfs;

extern void zfs_init(void);
extern void zfs_fini(void);

ldi_ident_t zfs_li = NULL;
dev_info_t *zfs_dip;
#endif /* !__APPLE__ */

typedef int zfs_ioc_func_t(zfs_cmd_t *);
typedef int zfs_secpolicy_func_t(zfs_cmd_t *, cred_t *);

typedef struct zfs_ioc_vec {
	zfs_ioc_func_t		*zvec_func;
	zfs_secpolicy_func_t	*zvec_secpolicy;
	enum {
		NO_NAME,
		POOL_NAME,
		DATASET_NAME
	} zvec_namecheck;
	boolean_t		zvec_his_log;
} zfs_ioc_vec_t;

/* _NOTE(PRINTFLIKE(4)) - this is printf-like, but lint is too whiney */
void
__dprintf(const char *file, const char *func, int line, const char *fmt, ...)
{
	const char *newfile __attribute__((__unused__));
	char buf[256];
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

	va_start(adx, fmt);
	(void) vsnprintf(buf, sizeof (buf), fmt, adx);
	va_end(adx);

#ifdef __APPLE__
	if (zfs_dprintf_enabled)
		debug_msg("%s", buf);
#endif

#if 0
	/*
	 * To get this data, use the zfs-dprintf probe as so:
	 * dtrace -q -n 'zfs-dprintf \
	 *	/stringof(arg0) == "dbuf.c"/ \
	 *	{printf("%s: %s", stringof(arg1), stringof(arg3))}'
	 * arg0 = file name
	 * arg1 = function name
	 * arg2 = line number
	 * arg3 = message
	 */
	DTRACE_PROBE4(zfs__dprintf,
	    char *, newfile, char *, func, int, line, char *, buf);
#endif
}

static void
history_str_free(char *buf)
{
	kmem_free(buf, HIS_MAX_RECORD_LEN);
}

static char *
history_str_get(zfs_cmd_t *zc)
{
	char *buf;

	if (zc->zc_history == NULL)
		return (NULL);

	buf = kmem_alloc(HIS_MAX_RECORD_LEN, KM_SLEEP);
#ifdef __APPLE__
	if (xcopyin(zc->zc_history, buf, HIS_MAX_RECORD_LEN) != 0) 
#else
	if (copyinstr(zc->zc_history, buf, HIS_MAX_RECORD_LEN, NULL) != 0) 
#endif
	{
		history_str_free(buf);
		return (NULL);
	}
	buf[HIS_MAX_RECORD_LEN -1] = '\0';
	return (buf);
}

static void
zfs_log_history(zfs_cmd_t *zc)
{
	spa_t *spa;
	char *buf;

	if ((buf = history_str_get(zc)) == NULL)
		return;

	if (spa_open(zc->zc_name, &spa, FTAG) == 0) {
		if (spa_version(spa) >= SPA_VERSION_ZPOOL_HISTORY)
			(void) spa_history_log(spa, buf, LOG_CMD_NORMAL);
		spa_close(spa, FTAG);
	}
	history_str_free(buf);
}

/*
 * Policy for top-level read operations (list pools).  Requires no privileges,
 * and can be used in the local zone, as there is no associated dataset.
 */
/* ARGSUSED */
static int
zfs_secpolicy_none(zfs_cmd_t *zc, cred_t *cr)
{
	return (0);
}

/*
 * Policy for dataset read operations (list children, get statistics).  Requires
 * no privileges, but must be visible in the local zone.
 */
/* ARGSUSED */
static int
zfs_secpolicy_read(zfs_cmd_t *zc, cred_t *cr)
{
#ifndef __APPLE__
	if (INGLOBALZONE(curproc) ||
	    zone_dataset_visible(dataset, NULL))
		return (0);

	return (ENOENT);
#else
	return (0);
#endif
}

static int
zfs_dozonecheck(const char *dataset, cred_t *cr)
{
#ifndef __APPLE__
	uint64_t zoned;
	int writable = 1;

	/*
	 * The dataset must be visible by this zone -- check this first
	 * so they don't see EPERM on something they shouldn't know about.
	 */
	if (!INGLOBALZONE(curproc) &&
	    !zone_dataset_visible(dataset, &writable))
		return (ENOENT);

	if (dsl_prop_get_integer(dataset, "zoned", &zoned, NULL))
		return (ENOENT);

	if (INGLOBALZONE(curproc)) {
		/*
		 * If the fs is zoned, only root can access it from the
		 * global zone.
		 */
		if (secpolicy_zfs(cr) && zoned)
			return (EPERM);
	} else {
		/*
		 * If we are in a local zone, the 'zoned' property must be set.
		 */
		if (!zoned)
			return (EPERM);

		/* must be writable by this zone */
		if (!writable)
			return (EPERM);
	}
#endif /*!__APPLE__*/
	return (0);
}

int
zfs_secpolicy_write_perms(const char *name, const char *perm, cred_t *cr)
{
	int error;

	error = zfs_dozonecheck(name, cr);
	if (error == 0) {
		error = secpolicy_zfs(cr);
		if (error)
			error = dsl_deleg_access(name, perm, cr);
	}
	return (error);
}

static int
zfs_secpolicy_setprop(const char *name, zfs_prop_t prop, cred_t *cr)
{
	/*
	 * Check permissions for special properties.
	 */
	switch (prop) {
	case ZFS_PROP_ZONED:
		/*
		 * Disallow setting of 'zoned' from within a local zone.
		 */
		if (!INGLOBALZONE(curproc))
			return (EPERM);
		break;

	case ZFS_PROP_QUOTA:
		if (!INGLOBALZONE(curproc)) {
			uint64_t zoned;
			char setpoint[MAXNAMELEN];
			/*
			 * Unprivileged users are allowed to modify the
			 * quota on things *under* (ie. contained by)
			 * the thing they own.
			 */
			if (dsl_prop_get_integer(name, "zoned", &zoned,
			    setpoint))
				return (EPERM);
			if (!zoned || strlen(name) <= strlen(setpoint))
				return (EPERM);
		}
		break;
	}

	return (zfs_secpolicy_write_perms(name, zfs_prop_to_name(prop), cr));
}

int
zfs_secpolicy_fsacl(zfs_cmd_t *zc, cred_t *cr)
{
	int error;

	error = zfs_dozonecheck(zc->zc_name, cr);
	if (error)
		return (error);

	/*
	 * permission to set permissions will be evaluated later in
	 * dsl_deleg_can_allow()
	 */
	return (0);
}

int
zfs_secpolicy_rollback(zfs_cmd_t *zc, cred_t *cr)
{
	int error;
	error = zfs_secpolicy_write_perms(zc->zc_name,
	    ZFS_DELEG_PERM_ROLLBACK, cr);
	if (error == 0)
		error = zfs_secpolicy_write_perms(zc->zc_name,
		    ZFS_DELEG_PERM_MOUNT, cr);
	return (error);
}

int
zfs_secpolicy_send(zfs_cmd_t *zc, cred_t *cr)
{
	return (zfs_secpolicy_write_perms(zc->zc_name,
	    ZFS_DELEG_PERM_SEND, cr));
}

int
zfs_secpolicy_share(zfs_cmd_t *zc, cred_t *cr)
{
#ifndef __APPLE__
	if (!INGLOBALZONE(curproc))
		return (EPERM);

	if (secpolicy_nfs(CRED()) == 0) {
		return (0);
	} else {
		vnode_t *vp;
		int error;

		if ((error = lookupname(zc->zc_value, UIO_SYSSPACE,
		    NO_FOLLOW, NULL, &vp)) != 0)
			return (error);

		/* Now make sure mntpnt and dataset are ZFS */

		if (vp->v_vfsp->vfs_fstype != zfsfstype ||
		    (strcmp((char *)refstr_value(vp->v_vfsp->vfs_resource),
		    zc->zc_name) != 0)) {
			VN_RELE(vp);
			return (EPERM);
		}

		VN_RELE(vp);
		return (dsl_deleg_access(zc->zc_name,
		    ZFS_DELEG_PERM_SHARE, cr));
	}
#endif /* !__APPLE__*/
	return (0);
}

static int
zfs_get_parent(const char *datasetname, char *parent, int parentsize)
{
	char *cp;

	/*
	 * Remove the @bla or /bla from the end of the name to get the parent.
	 */
	(void) strncpy(parent, datasetname, parentsize);
	cp = strrchr(parent, '@');
	if (cp != NULL) {
		cp[0] = '\0';
	} else {
		cp = strrchr(parent, '/');
		if (cp == NULL)
			return (ENOENT);
		cp[0] = '\0';
	}

	return (0);
}

int
zfs_secpolicy_destroy_perms(const char *name, cred_t *cr)
{
	int error;

	if ((error = zfs_secpolicy_write_perms(name,
	    ZFS_DELEG_PERM_MOUNT, cr)) != 0)
		return (error);

	return (zfs_secpolicy_write_perms(name, ZFS_DELEG_PERM_DESTROY, cr));
}

static int
zfs_secpolicy_destroy(zfs_cmd_t *zc, cred_t *cr)
{
	return (zfs_secpolicy_destroy_perms(zc->zc_name, cr));
}

/*
 * Must have sys_config privilege to check the iscsi permission
 */
/* ARGSUSED */
static int
zfs_secpolicy_iscsi(zfs_cmd_t *zc, cred_t *cr)
{
	return (secpolicy_zfs(cr));
}

int
zfs_secpolicy_rename_perms(const char *from, const char *to, cred_t *cr)
{
	char 	parentname[MAXNAMELEN];
	int	error;

	if ((error = zfs_secpolicy_write_perms(from,
	    ZFS_DELEG_PERM_RENAME, cr)) != 0)
		return (error);

	if ((error = zfs_secpolicy_write_perms(from,
	    ZFS_DELEG_PERM_MOUNT, cr)) != 0)
		return (error);

	if ((error = zfs_get_parent(to, parentname,
	    sizeof (parentname))) != 0)
		return (error);

	if ((error = zfs_secpolicy_write_perms(parentname,
	    ZFS_DELEG_PERM_CREATE, cr)) != 0)
		return (error);

	if ((error = zfs_secpolicy_write_perms(parentname,
	    ZFS_DELEG_PERM_MOUNT, cr)) != 0)
		return (error);

	return (error);
}

static int
zfs_secpolicy_rename(zfs_cmd_t *zc, cred_t *cr)
{
	return (zfs_secpolicy_rename_perms(zc->zc_name, zc->zc_value, cr));
}

static int
zfs_secpolicy_promote(zfs_cmd_t *zc, cred_t *cr)
{
	char 	parentname[MAXNAMELEN];
	objset_t *clone;
	int error;

	error = zfs_secpolicy_write_perms(zc->zc_name,
	    ZFS_DELEG_PERM_PROMOTE, cr);
	if (error)
		return (error);

	error = dmu_objset_open(zc->zc_name, DMU_OST_ANY,
	    DS_MODE_STANDARD | DS_MODE_READONLY, &clone);

	if (error == 0) {
		dsl_dataset_t *pclone = NULL;
		dsl_dir_t *dd;
		dd = clone->os->os_dsl_dataset->ds_dir;

		rw_enter(&dd->dd_pool->dp_config_rwlock, RW_READER);
		error = dsl_dataset_open_obj(dd->dd_pool,
		    dd->dd_phys->dd_clone_parent_obj, NULL,
		    DS_MODE_NONE, FTAG, &pclone);
		rw_exit(&dd->dd_pool->dp_config_rwlock);
		if (error) {
			dmu_objset_close(clone);
			return (error);
		}

		error = zfs_secpolicy_write_perms(zc->zc_name,
		    ZFS_DELEG_PERM_MOUNT, cr);

		dsl_dataset_name(pclone, parentname);
		dmu_objset_close(clone);
		dsl_dataset_close(pclone, DS_MODE_NONE, FTAG);
		if (error == 0)
			error = zfs_secpolicy_write_perms(parentname,
			    ZFS_DELEG_PERM_PROMOTE, cr);
	}
	return (error);
}

static int
zfs_secpolicy_receive(zfs_cmd_t *zc, cred_t *cr)
{
	int error;

	if ((error = zfs_secpolicy_write_perms(zc->zc_name,
	    ZFS_DELEG_PERM_RECEIVE, cr)) != 0)
		return (error);

	if ((error = zfs_secpolicy_write_perms(zc->zc_name,
	    ZFS_DELEG_PERM_MOUNT, cr)) != 0)
		return (error);

	return (zfs_secpolicy_write_perms(zc->zc_name,
	    ZFS_DELEG_PERM_CREATE, cr));
}

int
zfs_secpolicy_snapshot_perms(const char *name, cred_t *cr)
{
	int error;

	if ((error = zfs_secpolicy_write_perms(name,
	    ZFS_DELEG_PERM_SNAPSHOT, cr)) != 0)
		return (error);

	error = zfs_secpolicy_write_perms(name,
	    ZFS_DELEG_PERM_MOUNT, cr);

	return (error);
}

static int
zfs_secpolicy_snapshot(zfs_cmd_t *zc, cred_t *cr)
{

	return (zfs_secpolicy_snapshot_perms(zc->zc_name, cr));
}

static int
zfs_secpolicy_create(zfs_cmd_t *zc, cred_t *cr)
{
	char 	parentname[MAXNAMELEN];
	int 	error;

	if ((error = zfs_get_parent(zc->zc_name, parentname,
	    sizeof (parentname))) != 0)
		return (error);

	if (zc->zc_value[0] != '\0') {
		if ((error = zfs_secpolicy_write_perms(zc->zc_value,
		    ZFS_DELEG_PERM_CLONE, cr)) != 0)
			return (error);
	}

	if ((error = zfs_secpolicy_write_perms(parentname,
	    ZFS_DELEG_PERM_CREATE, cr)) != 0)
		return (error);

	error = zfs_secpolicy_write_perms(parentname,
	    ZFS_DELEG_PERM_MOUNT, cr);

	return (error);
}

static int
zfs_secpolicy_umount(zfs_cmd_t *zc, cred_t *cr)
{
	int error;

#ifdef __APPLE__
	/* XXX:  This is undefined when I link */
	error = 0; 
#else
	error = secpolicy_fs_unmount(cr, NULL);
#endif /* __APPLE__ */
	if (error) {
		error = dsl_deleg_access(zc->zc_name, ZFS_DELEG_PERM_MOUNT, cr);
	}
	return (error);
}

/*
 * Policy for pool operations - create/destroy pools, add vdevs, etc.  Requires
 * SYS_CONFIG privilege, which is not available in a local zone.
 */
/* ARGSUSED */
static int
zfs_secpolicy_config(zfs_cmd_t *zc, cred_t *cr)
{
	if (secpolicy_sys_config(cr, B_FALSE) != 0)
		return (EPERM);

	return (0);
}

/*
 * Just like zfs_secpolicy_config, except that we will check for
 * mount permission on the dataset for permission to create/remove
 * the minor nodes.
 */
static int
zfs_secpolicy_minor(zfs_cmd_t *zc, cred_t *cr)
{
	if (secpolicy_sys_config(cr, B_FALSE) != 0) {
		return (dsl_deleg_access(zc->zc_name,
		    ZFS_DELEG_PERM_MOUNT, cr));
	}

	return (0);
}

/*
 * Policy for fault injection.  Requires all privileges.
 */
/* ARGSUSED */
static int
zfs_secpolicy_inject(zfs_cmd_t *zc, cred_t *cr)
{
	return (secpolicy_zinject(cr));
}

static int
zfs_secpolicy_inherit(zfs_cmd_t *zc, cred_t *cr)
{
	zfs_prop_t prop = zfs_name_to_prop(zc->zc_value);

	if (prop == ZFS_PROP_INVAL) {
		if (!zfs_prop_user(zc->zc_value))
			return (EINVAL);
		return (zfs_secpolicy_write_perms(zc->zc_name,
		    ZFS_DELEG_PERM_USERPROP, cr));
	} else {
		if (!zfs_prop_inheritable(prop))
			return (EINVAL);
		return (zfs_secpolicy_setprop(zc->zc_name, prop, cr));
	}
}

/*
 * Returns the nvlist as specified by the user in the zfs_cmd_t.
 */
static int
get_nvlist(zfs_cmd_t *zc, nvlist_t **nvp)
{
	char *packed;
	size_t size;
	int error;
	nvlist_t *config = NULL;

	/*
	 * Read in and unpack the user-supplied nvlist.
	 */
#ifdef __APPLE__
	/* Give an upper limit as well to be secure */
        size = zc->zc_nvlist_src_size;
        if (size == 0 || size > (1024 * 1024 * 16))
#else
	if ((size = zc->zc_nvlist_src_size) == 0)
#endif /* __APPLE__ */
		return (EINVAL);

	packed = kmem_alloc(size, KM_SLEEP);

#ifdef __APPLE__
	if ((error = xcopyin(zc->zc_nvlist_src, packed, size)) != 0) 
#else
	if ((error = xcopyin((void *)(uintptr_t)zc->zc_nvlist_src, packed,
	    size)) != 0) 
#endif /* __APPLE__ */
	{
		kmem_free(packed, size);
		return (error);
	}

	if ((error = nvlist_unpack(packed, size, &config, 0)) != 0) {
		kmem_free(packed, size);
		return (error);
	}

	kmem_free(packed, size);

	*nvp = config;
	return (0);
}

static int
put_nvlist(zfs_cmd_t *zc, nvlist_t *nvl)
{
	char *packed = NULL;
	size_t size;
	int error;

	VERIFY(nvlist_size(nvl, &size, NV_ENCODE_NATIVE) == 0);

	if (size > zc->zc_nvlist_dst_size) {
		error = ENOMEM;
	} else {
		packed = kmem_alloc(size, KM_SLEEP);
		VERIFY(nvlist_pack(nvl, &packed, &size, NV_ENCODE_NATIVE,
		    KM_SLEEP) == 0);
#ifdef __APPLE__
		error = xcopyout(packed, zc->zc_nvlist_dst, size);
#else
		error = xcopyout(packed, (void *)(uintptr_t)zc->zc_nvlist_dst,
		    size);
#endif /* __APPLE__ */
		kmem_free(packed, size);
	}

	zc->zc_nvlist_dst_size = size;
	return (error);
}

static int
zfs_ioc_pool_create(zfs_cmd_t *zc)
{
	int error;
	nvlist_t *config;
	char *buf;

	if ((error = get_nvlist(zc, &config)) != 0)
		return (error);

	buf = history_str_get(zc);

	error = spa_create(zc->zc_name, config, zc->zc_value[0] == '\0' ?
	    NULL : zc->zc_value, buf);

	if (buf != NULL)
		history_str_free(buf);
	nvlist_free(config);

	return (error);
}

static int
zfs_ioc_pool_destroy(zfs_cmd_t *zc)
{
	int error;
	zfs_log_history(zc);
	error = spa_destroy(zc->zc_name);
	return (error);
}

static int
zfs_ioc_pool_import(zfs_cmd_t *zc)
{
	int error;
	nvlist_t *config;
	uint64_t guid;

	if ((error = get_nvlist(zc, &config)) != 0)
		return (error);

	if (nvlist_lookup_uint64(config, ZPOOL_CONFIG_POOL_GUID, &guid) != 0 ||
	    guid != zc->zc_guid)
		error = EINVAL;
	else
		error = spa_import(zc->zc_name, config,
		    zc->zc_value[0] == '\0' ? NULL : zc->zc_value);

	nvlist_free(config);

	return (error);
}

static int
zfs_ioc_pool_export(zfs_cmd_t *zc)
{
	int error;
	zfs_log_history(zc);
	error = spa_export(zc->zc_name, NULL);
	return (error);
}

static int
zfs_ioc_pool_configs(zfs_cmd_t *zc)
{
	nvlist_t *configs;
	int error;

	if ((configs = spa_all_configs(&zc->zc_cookie)) == NULL)
		return (EEXIST);

	error = put_nvlist(zc, configs);

	nvlist_free(configs);

	return (error);
}

static int
zfs_ioc_pool_stats(zfs_cmd_t *zc)
{
	nvlist_t *config;
	int error;
	int ret = 0;

	error = spa_get_stats(zc->zc_name, &config, zc->zc_value,
	    sizeof (zc->zc_value));

	if (config != NULL) {
		ret = put_nvlist(zc, config);
		nvlist_free(config);

		/*
		 * The config may be present even if 'error' is non-zero.
		 * In this case we return success, and preserve the real errno
		 * in 'zc_cookie'.
		 */
		zc->zc_cookie = error;
	} else {
		ret = error;
	}

	return (ret);
}

/*
 * Try to import the given pool, returning pool stats as appropriate so that
 * user land knows which devices are available and overall pool health.
 */
static int
zfs_ioc_pool_tryimport(zfs_cmd_t *zc)
{
	nvlist_t *tryconfig, *config;
	int error;

	if ((error = get_nvlist(zc, &tryconfig)) != 0)
		return (error);

	config = spa_tryimport(tryconfig);

	nvlist_free(tryconfig);

	if (config == NULL)
		return (EINVAL);

	error = put_nvlist(zc, config);
	nvlist_free(config);
	
	/* 
	 * We must return the error in the zc structure instead of letting
	 * the ioctl return error, otherwise the ioctl will *NOT* xcopyout
	 * the new data into userland
	 */
	return (error);
}

static int
zfs_ioc_pool_scrub(zfs_cmd_t *zc)
{
	spa_t *spa;
	int error;

	if ((error = spa_open(zc->zc_name, &spa, FTAG)) != 0)
		return (error);

	mutex_enter(&spa_namespace_lock);
	error = spa_scrub(spa, zc->zc_cookie, B_FALSE);
	mutex_exit(&spa_namespace_lock);

	spa_close(spa, FTAG);

	return (error);
}

static int
zfs_ioc_pool_freeze(zfs_cmd_t *zc)
{
	spa_t *spa;
	int error;

	error = spa_open(zc->zc_name, &spa, FTAG);
	if (error == 0) {
		spa_freeze(spa);
		spa_close(spa, FTAG);
	}
	return (error);
}

static int
zfs_ioc_pool_upgrade(zfs_cmd_t *zc)
{
	spa_t *spa;
	int error;

	if ((error = spa_open(zc->zc_name, &spa, FTAG)) != 0)
		return (error);

	spa_upgrade(spa);
	spa_close(spa, FTAG);

	return (error);
}

static int
zfs_ioc_pool_get_history(zfs_cmd_t *zc)
{
	spa_t *spa;
	char *hist_buf;
	uint64_t size;
	int error;

	if ((size = zc->zc_history_len) == 0)
		return (EINVAL);

	if ((error = spa_open(zc->zc_name, &spa, FTAG)) != 0)
		return (error);

	if (spa_version(spa) < SPA_VERSION_ZPOOL_HISTORY) {
		spa_close(spa, FTAG);
		return (ENOTSUP);
	}

	hist_buf = kmem_alloc(size, KM_SLEEP);
	if ((error = spa_history_get(spa, &zc->zc_history_offset,
	    &zc->zc_history_len, hist_buf)) == 0) {
#ifdef __APPLE__
		error = xcopyout(hist_buf, zc->zc_history, zc->zc_history_len);
#else
		error = xcopyout(hist_buf,
		    (char *)(uintptr_t)zc->zc_history,
		    zc->zc_history_len);
#endif /* __APPLE__ */
	}

	spa_close(spa, FTAG);
	kmem_free(hist_buf, size);
	return (error);
}

static int
zfs_ioc_dsobj_to_dsname(zfs_cmd_t *zc)
{
	int error;

	if (error = dsl_dsobj_to_dsname(zc->zc_name, zc->zc_obj, zc->zc_value))
		return (error);

	return (0);
}

static int
zfs_ioc_obj_to_path(zfs_cmd_t *zc)
{
	objset_t *osp;
	int error;

	if ((error = dmu_objset_open(zc->zc_name, DMU_OST_ZFS,
	    DS_MODE_NONE | DS_MODE_READONLY, &osp)) != 0)
		return (error);

	error = zfs_obj_to_path(osp, zc->zc_obj, zc->zc_value,
	    sizeof (zc->zc_value));
	dmu_objset_close(osp);

	return (error);
}

static int
zfs_ioc_vdev_add(zfs_cmd_t *zc)
{
	spa_t *spa;
	int error;
	nvlist_t *config;

	error = spa_open(zc->zc_name, &spa, FTAG);
	if (error != 0)
		return (error);

	/*
	 * A root pool with concatenated devices is not supported.
	 * Thus, can not add a device to a root pool with one device.
	 */
	if (spa->spa_root_vdev->vdev_children == 1 && spa->spa_bootfs != 0) {
		spa_close(spa, FTAG);
		return (EDOM);
	}

	if ((error = get_nvlist(zc, &config)) == 0) {
		error = spa_vdev_add(spa, config);
		nvlist_free(config);
	}
	spa_close(spa, FTAG);
	return (error);
}

static int
zfs_ioc_vdev_remove(zfs_cmd_t *zc)
{
	spa_t *spa;
	int error;

	error = spa_open(zc->zc_name, &spa, FTAG);
	if (error != 0)
		return (error);
	error = spa_vdev_remove(spa, zc->zc_guid, B_FALSE);
	spa_close(spa, FTAG);
	return (error);
}

static int
zfs_ioc_vdev_set_state(zfs_cmd_t *zc)
{
	spa_t *spa;
	int error;
	vdev_state_t newstate = VDEV_STATE_UNKNOWN;

	if ((error = spa_open(zc->zc_name, &spa, FTAG)) != 0)
		return (error);
	switch (zc->zc_cookie) {
	case VDEV_STATE_ONLINE:
		error = vdev_online(spa, zc->zc_guid, zc->zc_obj, &newstate);
		break;

	case VDEV_STATE_OFFLINE:
		error = vdev_offline(spa, zc->zc_guid, zc->zc_obj);
		break;

	case VDEV_STATE_FAULTED:
		error = vdev_fault(spa, zc->zc_guid);
		break;

	case VDEV_STATE_DEGRADED:
		error = vdev_degrade(spa, zc->zc_guid);
		break;

	default:
		error = EINVAL;
	}
	zc->zc_cookie = newstate;
	spa_close(spa, FTAG);
	return (error);
}

static int
zfs_ioc_vdev_attach(zfs_cmd_t *zc)
{
	spa_t *spa;
	int replacing = zc->zc_cookie;
	nvlist_t *config;
	int error;

	if ((error = spa_open(zc->zc_name, &spa, FTAG)) != 0)
		return (error);

	if ((error = get_nvlist(zc, &config)) == 0) {
		error = spa_vdev_attach(spa, zc->zc_guid, config, replacing);
		nvlist_free(config);
	}

	spa_close(spa, FTAG);
	return (error);
}

static int
zfs_ioc_vdev_detach(zfs_cmd_t *zc)
{
	spa_t *spa;
	int error;

	if ((error = spa_open(zc->zc_name, &spa, FTAG)) != 0)
		return (error);

	error = spa_vdev_detach(spa, zc->zc_guid, B_FALSE);

	spa_close(spa, FTAG);
	return (error);
}

static int
zfs_ioc_vdev_setpath(zfs_cmd_t *zc)
{
	spa_t *spa;
	char *path = zc->zc_value;
	uint64_t guid = zc->zc_guid;
	int error;

	error = spa_open(zc->zc_name, &spa, FTAG);
	if (error != 0)
		return (error);

	error = spa_vdev_setpath(spa, guid, path);
	spa_close(spa, FTAG);
	return (error);
}

static int
zfs_ioc_objset_stats(zfs_cmd_t *zc)
{
	objset_t *os = NULL;
	int error;
	nvlist_t *nv;

retry:
	error = dmu_objset_open(zc->zc_name, DMU_OST_ANY,
	    DS_MODE_STANDARD | DS_MODE_READONLY, &os);
	if (error != 0) {
		/*
		 * This is ugly: dmu_objset_open() can return EBUSY if
		 * the objset is held exclusively. Fortunately this hold is
		 * only for a short while, so we retry here.
		 * This avoids user code having to handle EBUSY,
		 * for example for a "zfs list".
		 */
		if (error == EBUSY) {
			delay(1);
			goto retry;
		}
		return (error);
	}

	dmu_objset_fast_stat(os, &zc->zc_objset_stats);

	if (zc->zc_nvlist_dst != 0 &&
	    (error = dsl_prop_get_all(os, &nv)) == 0) {
		dmu_objset_stats(os, nv);
		/*
		 * NB: {zpl,zvol}_get_stats() will read the objset contents,
		 * which we aren't supposed to do with a
		 * DS_MODE_STANDARD open, because it could be
		 * inconsistent.  So this is a bit of a workaround...
		 */
		if (!zc->zc_objset_stats.dds_inconsistent) {
			if (dmu_objset_type(os) == DMU_OST_ZVOL)
				VERIFY(zvol_get_stats(os, nv) == 0);
			else if (dmu_objset_type(os) == DMU_OST_ZFS)
				(void) zfs_get_stats(os, nv);
		}
		error = put_nvlist(zc, nv);
		nvlist_free(nv);
	}

	spa_altroot(dmu_objset_spa(os), zc->zc_value, sizeof (zc->zc_value));

	dmu_objset_close(os);
	return (error);
}

static int
zfs_ioc_dataset_list_next(zfs_cmd_t *zc)
{
	objset_t *os;
	int error;
	char *p;

retry:
	error = dmu_objset_open(zc->zc_name, DMU_OST_ANY,
	    DS_MODE_STANDARD | DS_MODE_READONLY, &os);
	if (error != 0) {
		/*
		 * This is ugly: dmu_objset_open() can return EBUSY if
		 * the objset is held exclusively. Fortunately this hold is
		 * only for a short while, so we retry here.
		 * This avoids user code having to handle EBUSY,
		 * for example for a "zfs list".
		 */
		if (error == EBUSY) {
			delay(1);
			goto retry;
		}
		if (error == ENOENT)
			error = ESRCH;
		return (error);
	}

	p = strrchr(zc->zc_name, '/');
	if (p == NULL || p[1] != '\0')
		(void) strlcat(zc->zc_name, "/", sizeof (zc->zc_name));
	p = zc->zc_name + strlen(zc->zc_name);

	do {
		error = dmu_dir_list_next(os,
		    sizeof (zc->zc_name) - (p - zc->zc_name), p,
		    NULL, &zc->zc_cookie);
		if (error == ENOENT)
			error = ESRCH;
	} while (error == 0 && !INGLOBALZONE(curproc) &&
	    !zone_dataset_visible(zc->zc_name, NULL));

	/*
	 * If it's a hidden dataset (ie. with a '$' in its name), don't
	 * try to get stats for it.  Userland will skip over it.
	 */
	if (error == 0 && strchr(zc->zc_name, '$') == NULL)
		error = zfs_ioc_objset_stats(zc); /* fill in the stats */

	dmu_objset_close(os);
	return (error);
}

static int
zfs_ioc_snapshot_list_next(zfs_cmd_t *zc)
{
	objset_t *os;
	int error;

retry:
	error = dmu_objset_open(zc->zc_name, DMU_OST_ANY,
	    DS_MODE_STANDARD | DS_MODE_READONLY, &os);
	if (error != 0) {
		/*
		 * This is ugly: dmu_objset_open() can return EBUSY if
		 * the objset is held exclusively. Fortunately this hold is
		 * only for a short while, so we retry here.
		 * This avoids user code having to handle EBUSY,
		 * for example for a "zfs list".
		 */
		if (error == EBUSY) {
			delay(1);
			goto retry;
		}
		if (error == ENOENT)
			error = ESRCH;
		return (error);
	}

	/*
	 * A dataset name of maximum length cannot have any snapshots,
	 * so exit immediately.
	 */
	if (strlcat(zc->zc_name, "@", sizeof (zc->zc_name)) >= MAXNAMELEN) {
		dmu_objset_close(os);
		return (ESRCH);
	}

	error = dmu_snapshot_list_next(os,
	    sizeof (zc->zc_name) - strlen(zc->zc_name),
	    zc->zc_name + strlen(zc->zc_name), NULL, &zc->zc_cookie);
	if (error == ENOENT)
		error = ESRCH;

	if (error == 0)
		error = zfs_ioc_objset_stats(zc); /* fill in the stats */

	dmu_objset_close(os);
	return (error);
}

static int
// In the 10a286 bits, the 'dev' parameter wasn't used/needed
#ifdef __APPLE__
zfs_set_prop_nvlist(const char *name, dev_t dev, nvlist_t *nvl)
#else
zfs_set_prop_nvlist(const char *name, nvlist_t *nvl)
#endif
{
	nvpair_t *elem;
	int error;
	uint64_t intval;
	char *strval;

	/*
	 * First validate permission to set all of the properties
	 */
	elem = NULL;
	while ((elem = nvlist_next_nvpair(nvl, elem)) != NULL) {
		const char *propname = nvpair_name(elem);
		zfs_prop_t prop = zfs_name_to_prop(propname);

		if (prop == ZFS_PROP_INVAL) {
			/*
			 * If this is a user-defined property, it must be a
			 * string, and there is no further validation to do.
			 */
			if (!zfs_prop_user(propname) ||
			    nvpair_type(elem) != DATA_TYPE_STRING)
				return (EINVAL);

			error = zfs_secpolicy_write_perms(name,
			    ZFS_DELEG_PERM_USERPROP, CRED());
			if (error)
				return (error);
			continue;
		}

		if ((error = zfs_secpolicy_setprop(name, prop, CRED())) != 0)
			return (error);

		/*
		 * Check that this value is valid for this pool version
		 */
		switch (prop) {
		case ZFS_PROP_COMPRESSION:
			/*
			 * If the user specified gzip compression, make sure
			 * the SPA supports it. We ignore any errors here since
			 * we'll catch them later.
			 */
			if (nvpair_type(elem) == DATA_TYPE_UINT64 &&
			    nvpair_value_uint64(elem, &intval) == 0 &&
			    intval >= ZIO_COMPRESS_GZIP_1 &&
			    intval <= ZIO_COMPRESS_GZIP_9) {
				spa_t *spa;

				if (spa_open(name, &spa, FTAG) == 0) {
					if (spa_version(spa) <
					    SPA_VERSION_GZIP_COMPRESSION) {
						spa_close(spa, FTAG);
						return (ENOTSUP);
					}

					spa_close(spa, FTAG);
				}
			}
			break;

		case ZFS_PROP_COPIES:
		{
			spa_t *spa;

			if (spa_open(name, &spa, FTAG) == 0) {
				if (spa_version(spa) <
				    SPA_VERSION_DITTO_BLOCKS) {
					spa_close(spa, FTAG);
					return (ENOTSUP);
				}
				spa_close(spa, FTAG);
			}
			break;
		}
		}
	}

	elem = NULL;
	while ((elem = nvlist_next_nvpair(nvl, elem)) != NULL) {
		const char *propname = nvpair_name(elem);
		zfs_prop_t prop = zfs_name_to_prop(propname);

		if (prop == ZFS_PROP_INVAL) {
			VERIFY(nvpair_value_string(elem, &strval) == 0);
			error = dsl_prop_set(name, propname, 1,
			    strlen(strval) + 1, strval);
			if (error == 0)
				continue;
			else
				return (error);
		}

		switch (prop) {
		case ZFS_PROP_QUOTA:
			if ((error = nvpair_value_uint64(elem, &intval)) != 0 ||
			    (error = dsl_dir_set_quota(name, intval)) != 0)
				return (error);
			break;

		case ZFS_PROP_RESERVATION:
			if ((error = nvpair_value_uint64(elem, &intval)) != 0 ||
			    (error = dsl_dir_set_reservation(name,
			    intval)) != 0)
				return (error);
			break;

// In the 10a286 bits, VOLSIZE and VOLBLOCKSIZE were ifndef'd out
// As a result, the signature didn't need 'dev' any more
		case ZFS_PROP_VOLSIZE:
			if ((error = nvpair_value_uint64(elem, &intval)) != 0 ||
#ifdef __APPLE__
			    (error = zvol_set_volsize(name, dev,
			     intval)) != 0
#else
			    (error = zvol_set_volsize(name,
			    ddi_driver_major(zfs_dip), intval)) != 0
#endif
			)
				return (error);
			break;

		case ZFS_PROP_VOLBLOCKSIZE:
			if ((error = nvpair_value_uint64(elem, &intval)) != 0 ||
			    (error = zvol_set_volblocksize(name, intval)) != 0)
				return (error);
			break;

		case ZFS_PROP_VERSION:
			if ((error = nvpair_value_uint64(elem, &intval)) != 0 ||
			    (error = zfs_set_version(name, intval)) != 0)
				return (error);
			break;

		default:
			if (nvpair_type(elem) == DATA_TYPE_STRING) {
				if (zfs_prop_get_type(prop) !=
				    PROP_TYPE_STRING)
					return (EINVAL);
				VERIFY(nvpair_value_string(elem, &strval) == 0);
				if ((error = dsl_prop_set(name,
				    nvpair_name(elem), 1, strlen(strval) + 1,
				    strval)) != 0)
					return (error);
			} else if (nvpair_type(elem) == DATA_TYPE_UINT64) {
				const char *unused;

				VERIFY(nvpair_value_uint64(elem, &intval) == 0);

				switch (zfs_prop_get_type(prop)) {
				case PROP_TYPE_NUMBER:
					break;
				case PROP_TYPE_STRING:
					return (EINVAL);
				case PROP_TYPE_INDEX:
					if (zfs_prop_index_to_string(prop,
					    intval, &unused) != 0)
						return (EINVAL);
					break;
				default:
					cmn_err(CE_PANIC,
					    "unknown property type");
					break;
				}

				if ((error = dsl_prop_set(name, propname,
				    8, 1, &intval)) != 0)
					return (error);
			} else {
				return (EINVAL);
			}
			break;
		}
	}

	return (0);
}

static int
zfs_ioc_set_prop(zfs_cmd_t *zc)
{
	nvlist_t *nvl;
	int error;

	if ((error = get_nvlist(zc, &nvl)) != 0)
		return (error);

// In the 10a286 bits, the 'zc->zc_dev' wasn't needed
	error = zfs_set_prop_nvlist(zc->zc_name, zc->zc_dev, nvl);

	nvlist_free(nvl);
	return (error);
}

static int
zfs_ioc_inherit_prop(zfs_cmd_t *zc)
{
	/* the property name has been validated by zfs_secpolicy_inherit() */
	return (dsl_prop_set(zc->zc_name, zc->zc_value, 0, 0, NULL));
}

static int
zfs_ioc_pool_set_props(zfs_cmd_t *zc)
{
	nvlist_t *nvl;
	int error, reset_bootfs = 0;
	uint64_t objnum;
	uint64_t intval;
	zpool_prop_t prop;
	nvpair_t *elem;
	char *propname, *strval;
	spa_t *spa;
	vdev_t *rvdev;
	char *vdev_type;
	objset_t *os;

	if ((error = get_nvlist(zc, &nvl)) != 0)
		return (error);

	if ((error = spa_open(zc->zc_name, &spa, FTAG)) != 0) {
		nvlist_free(nvl);
		return (error);
	}

	if (spa_version(spa) < SPA_VERSION_BOOTFS) {
		nvlist_free(nvl);
		spa_close(spa, FTAG);
		return (ENOTSUP);
	}

	elem = NULL;
	while ((elem = nvlist_next_nvpair(nvl, elem)) != NULL) {

		propname = nvpair_name(elem);

		if ((prop = zpool_name_to_prop(propname)) ==
		    ZFS_PROP_INVAL) {
			nvlist_free(nvl);
			spa_close(spa, FTAG);
			return (EINVAL);
		}

		switch (prop) {
		case ZPOOL_PROP_DELEGATION:
			VERIFY(nvpair_value_uint64(elem, &intval) == 0);
			if (intval > 1)
				error = EINVAL;
			break;
		case ZPOOL_PROP_BOOTFS:
			/*
			 * A bootable filesystem can not be on a RAIDZ pool
			 * nor a striped pool with more than 1 device.
			 */
			rvdev = spa->spa_root_vdev;
			vdev_type =
			    rvdev->vdev_child[0]->vdev_ops->vdev_op_type;
			if (strcmp(vdev_type, VDEV_TYPE_RAIDZ) == 0 ||
			    (strcmp(vdev_type, VDEV_TYPE_MIRROR) != 0 &&
			    rvdev->vdev_children > 1)) {
				error = ENOTSUP;
				break;
			}

			reset_bootfs = 1;

			VERIFY(nvpair_value_string(elem, &strval) == 0);
			if (strval == NULL || strval[0] == '\0') {
				objnum = zpool_prop_default_numeric(
				    ZPOOL_PROP_BOOTFS);
				break;
			}

			if (error = dmu_objset_open(strval, DMU_OST_ZFS,
			    DS_MODE_STANDARD | DS_MODE_READONLY, &os))
				break;
			objnum = dmu_objset_id(os);
			dmu_objset_close(os);
			break;

		case ZPOOL_PROP_ASHIFT:
			/* 
			 * Property can only be set at pool create time, and
			 * that code path does not go through this function.
			 * So unconditionally fail here.
			 */
			error = EPERM;
			break;
		}

		if (error)
			break;
	}
	if (error == 0) {
		if (reset_bootfs) {
			VERIFY(nvlist_remove(nvl,
			    zpool_prop_to_name(ZPOOL_PROP_BOOTFS),
			    DATA_TYPE_STRING) == 0);
			VERIFY(nvlist_add_uint64(nvl,
			    zpool_prop_to_name(ZPOOL_PROP_BOOTFS),
			    objnum) == 0);
		}
		error = spa_set_props(spa, nvl);
	}

	nvlist_free(nvl);
	spa_close(spa, FTAG);

	return (error);
}

static int
zfs_ioc_pool_get_props(zfs_cmd_t *zc)
{
	spa_t *spa;
	int error;
	nvlist_t *nvp = NULL;

	if ((error = spa_open(zc->zc_name, &spa, FTAG)) != 0)
		return (error);

	error = spa_get_props(spa, &nvp);

	if (error == 0 && zc->zc_nvlist_dst != NULL)
		error = put_nvlist(zc, nvp);
	else
		error = EFAULT;

	spa_close(spa, FTAG);

	if (nvp)
		nvlist_free(nvp);
	return (error);
}

static int
zfs_ioc_iscsi_perm_check(zfs_cmd_t *zc)
{
#ifndef __APPLE__
	nvlist_t *nvp;
	int error;
	uint32_t uid;
	uint32_t gid;
	uint32_t *groups;
	uint_t group_cnt;
	cred_t	*usercred;

	if ((error = get_nvlist(zc, &nvp)) != 0) {
		return (error);
	}

	if ((error = nvlist_lookup_uint32(nvp,
	    ZFS_DELEG_PERM_UID, &uid)) != 0) {
		nvlist_free(nvp);
		return (EPERM);
	}

	if ((error = nvlist_lookup_uint32(nvp,
	    ZFS_DELEG_PERM_GID, &gid)) != 0) {
		nvlist_free(nvp);
		return (EPERM);
	}

	if ((error = nvlist_lookup_uint32_array(nvp, ZFS_DELEG_PERM_GROUPS,
	    &groups, &group_cnt)) != 0) {
		nvlist_free(nvp);
		return (EPERM);
	}
	usercred = cralloc();
	if ((crsetugid(usercred, uid, gid) != 0) ||
	    (crsetgroups(usercred, group_cnt, (gid_t *)groups) != 0)) {
		nvlist_free(nvp);
		crfree(usercred);
		return (EPERM);
	}
	nvlist_free(nvp);
	error = dsl_deleg_access(zc->zc_name,
	    zfs_prop_to_name(ZFS_PROP_SHAREISCSI), usercred);
	crfree(usercred);
	return (error);
#endif /* !__APPLE__ */
}

static int
zfs_ioc_set_fsacl(zfs_cmd_t *zc)
{
	int error;
	nvlist_t *fsaclnv = NULL;

	if ((error = get_nvlist(zc, &fsaclnv)) != 0)
		return (error);

	/*
	 * Verify nvlist is constructed correctly
	 */
	if (zfs_deleg_verify_nvlist(fsaclnv) != 0) {
		nvlist_free(fsaclnv);
		return (EINVAL);
	}

	/*
	 * If we don't have PRIV_SYS_MOUNT, then validate
	 * that user is allowed to hand out each permission in
	 * the nvlist(s)
	 */

	error = secpolicy_zfs(CRED());
	if (error) {
		if (zc->zc_perm_action == B_FALSE) {
			error = dsl_deleg_can_allow(zc->zc_name,
			    fsaclnv, CRED());
		} else {
			error = dsl_deleg_can_unallow(zc->zc_name,
			    fsaclnv, CRED());
		}
	}

	if (error == 0)
		error = dsl_deleg_set(zc->zc_name, fsaclnv, zc->zc_perm_action);

	nvlist_free(fsaclnv);
	return (error);
}

static int
zfs_ioc_get_fsacl(zfs_cmd_t *zc)
{
	nvlist_t *nvp;
	int error;

	if ((error = dsl_deleg_get(zc->zc_name, &nvp)) == 0) {
		error = put_nvlist(zc, nvp);
		nvlist_free(nvp);
	}

	return (error);
}

static int
zfs_ioc_create_minor(zfs_cmd_t *zc)
{
#ifdef __APPLE__
	return (zvol_create_minor(zc->zc_name, zc->zc_dev));
#else
	return (zvol_create_minor(zc->zc_name, ddi_driver_major(zfs_dip)));
#endif /* __APPLE __*/
}

static int
zfs_ioc_remove_minor(zfs_cmd_t *zc)
{
	return (zvol_remove_minor(zc->zc_name));
}

/*
 * Search the vfs list for a specified resource.  Returns a pointer to it
 * or NULL if no suitable entry is found. The caller of this routine
 * is responsible for releasing the returned vfs pointer.
 */
static vfs_t *
zfs_get_vfs(const char *resource)
{
#ifdef __APPLE__
	return (NULL);
#else
	struct vfs *vfsp;
	struct vfs *vfs_found = NULL;

	vfs_list_read_lock();
	vfsp = rootvfs;
	do {
		if (strcmp(refstr_value(vfsp->vfs_resource), resource) == 0) {
			VFS_HOLD(vfsp);
			vfs_found = vfsp;
			break;
		}
		vfsp = vfsp->vfs_next;
	} while (vfsp != rootvfs);
	vfs_list_unlock();
	return (vfs_found);
#endif
}

/* ARGSUSED */
static void
zfs_create_cb(objset_t *os, void *arg, cred_t *cr, dmu_tx_t *tx)
{
	nvlist_t *nvprops = arg;
	uint64_t version = ZPL_VERSION;

	(void) nvlist_lookup_uint64(nvprops,
	    zfs_prop_to_name(ZFS_PROP_VERSION), &version);

	zfs_create_fs(os, cr, version, tx);
}

static int
zfs_ioc_create(zfs_cmd_t *zc)
{
	objset_t *clone;
	int error = 0;
	nvlist_t *nvprops = NULL;
	void (*cbfunc)(objset_t *os, void *arg, cred_t *cr, dmu_tx_t *tx);
	dmu_objset_type_t type = zc->zc_objset_type;

	switch (type) {

	case DMU_OST_ZFS:
		cbfunc = zfs_create_cb;
		break;

	case DMU_OST_ZVOL:
		cbfunc = zvol_create_cb;
		break;

	default:
		cbfunc = NULL;
	}
	if (strchr(zc->zc_name, '@'))
		return (EINVAL);

	if (zc->zc_nvlist_src != NULL &&
	    (error = get_nvlist(zc, &nvprops)) != 0)
		return (error);

	if (zc->zc_value[0] != '\0') {
		/*
		 * We're creating a clone of an existing snapshot.
		 */
		zc->zc_value[sizeof (zc->zc_value) - 1] = '\0';
		if (dataset_namecheck(zc->zc_value, NULL, NULL) != 0) {
			nvlist_free(nvprops);
			return (EINVAL);
		}

		error = dmu_objset_open(zc->zc_value, type,
		    DS_MODE_STANDARD | DS_MODE_READONLY, &clone);
		if (error) {
			nvlist_free(nvprops);
			return (error);
		}
		error = dmu_objset_create(zc->zc_name, type, clone, NULL, NULL);
		dmu_objset_close(clone);
	} else {
		if (cbfunc == NULL) {
			nvlist_free(nvprops);
			return (EINVAL);
		}

		if (type == DMU_OST_ZVOL) {
			uint64_t volsize, volblocksize;

			if (nvprops == NULL ||
			    nvlist_lookup_uint64(nvprops,
			    zfs_prop_to_name(ZFS_PROP_VOLSIZE),
			    &volsize) != 0) {
				nvlist_free(nvprops);
				return (EINVAL);
			}

			if ((error = nvlist_lookup_uint64(nvprops,
			    zfs_prop_to_name(ZFS_PROP_VOLBLOCKSIZE),
			    &volblocksize)) != 0 && error != ENOENT) {
				nvlist_free(nvprops);
				return (EINVAL);
			}

			if (error != 0)
				volblocksize = zfs_prop_default_numeric(
				    ZFS_PROP_VOLBLOCKSIZE);

			if ((error = zvol_check_volblocksize(
			    volblocksize)) != 0 ||
			    (error = zvol_check_volsize(volsize,
			    volblocksize)) != 0) {
				nvlist_free(nvprops);
				return (error);
			}
		} else if (type == DMU_OST_ZFS) {
			uint64_t version;

			if (0 == nvlist_lookup_uint64(nvprops,
			    zfs_prop_to_name(ZFS_PROP_VERSION), &version) &&
			    (version < ZPL_VERSION_INITIAL ||
			    version > ZPL_VERSION)) {
				nvlist_free(nvprops);
				return (EINVAL);
			}
		}

		error = dmu_objset_create(zc->zc_name, type, NULL, cbfunc,
		    nvprops);
	}

	/*
	 * It would be nice to do this atomically.
	 */
	if (error == 0) {
#ifdef __APPLE__
		if ((error = zfs_set_prop_nvlist(zc->zc_name, zc->zc_dev, 
						nvprops)) != 0)
#else
		if ((error = zfs_set_prop_nvlist(zc->zc_name, nvprops)) != 0)
#endif /* __APPLE__ */
			(void) dmu_objset_destroy(zc->zc_name);
	}

	nvlist_free(nvprops);
	return (error);
}

static int
zfs_ioc_snapshot(zfs_cmd_t *zc)
{
	if (snapshot_namecheck(zc->zc_value, NULL, NULL) != 0)
		return (EINVAL);
	return (dmu_objset_snapshot(zc->zc_name,
	    zc->zc_value, zc->zc_cookie));
}

int
zfs_unmount_snap(char *name, void *arg)
{
	char *snapname = arg;
	char *cp;
	vfs_t *vfsp = NULL;

	/*
	 * Snapshots (which are under .zfs control) must be unmounted
	 * before they can be destroyed.
	 */

	if (snapname) {
		(void) strcat(name, "@");
		(void) strcat(name, snapname);
		/* XXX Noel-Until the GFS is ported, snapshots are not mounted,
		 * so there is no need to unmount them
		 */
#ifndef __APPLE__
		vfsp = zfs_get_vfs(name);
#endif /* !__APPLE__ */
		cp = strchr(name, '@');
		*cp = '\0';
	} else if (strchr(name, '@')) {
#ifndef __APPLE__
		vfsp = zfs_get_vfs(name);
#endif /* !__APPLE__ */
	}
#if 0
	if (vfsp) {
		/*
		 * Always force the unmount for snapshots.
		 */
		int flag = MS_FORCE;
		int err;

		if ((err = vn_vfswlock(vfsp->vfs_vnodecovered)) != 0) {
			VFS_RELE(vfsp);
			return (err);
		}
		VFS_RELE(vfsp);
		if ((err = dounmount(vfsp, flag, kcred)) != 0)
			return (err);
	}
#endif
	return (0);
}

static int
zfs_ioc_destroy_snaps(zfs_cmd_t *zc)
{
	int err;

	if (snapshot_namecheck(zc->zc_value, NULL, NULL) != 0)
		return (EINVAL);
	err = dmu_objset_find(zc->zc_name,
	    zfs_unmount_snap, zc->zc_value, DS_FIND_CHILDREN);
	if (err)
		return (err);
	return (dmu_snapshots_destroy(zc->zc_name, zc->zc_value));
}

static int
zfs_ioc_destroy(zfs_cmd_t *zc)
{
	if (strchr(zc->zc_name, '@') && zc->zc_objset_type == DMU_OST_ZFS) {
		int err = zfs_unmount_snap(zc->zc_name, NULL);
		if (err)
			return (err);
	}

	return (dmu_objset_destroy(zc->zc_name));
}

static int
zfs_ioc_rollback(zfs_cmd_t *zc)
{
	return (dmu_objset_rollback(zc->zc_name));
}

static int
zfs_ioc_rename(zfs_cmd_t *zc)
{
	boolean_t recursive = zc->zc_cookie & 1;
	zc->zc_value[sizeof (zc->zc_value) - 1] = '\0';
	if (dataset_namecheck(zc->zc_value, NULL, NULL) != 0)
		return (EINVAL);

	/*
	 * Unmount snapshot unless we're doing a recursive rename,
	 * in which case the dataset code figures out which snapshots
	 * to unmount.
	 */
	if (!recursive && strchr(zc->zc_name, '@') != NULL &&
	    zc->zc_objset_type == DMU_OST_ZFS) {
		int err = zfs_unmount_snap(zc->zc_name, NULL);
		if (err)
			return (err);
	}

	return (dmu_objset_rename(zc->zc_name, zc->zc_value, recursive));
}

#if ZFS_LEOPARD_ONLY
#define file_vnode_withvid(a, b, c) file_vnode(a, b)
#endif
	
static int
zfs_ioc_recvbackup(zfs_cmd_t *zc)
{
#ifdef __APPLE__
	vnode_t *vp;
#else
	file_t *fp;
#endif
	int error, fd;
	offset_t new_off;

	if (dataset_namecheck(zc->zc_value, NULL, NULL) != 0 ||
	    strchr(zc->zc_value, '@') == NULL)
		return (EINVAL);

	fd = zc->zc_cookie;
#ifdef __APPLE__
	/*XXX NOEL: due to the fact that BSD doesn't support 
	 * vnodes for things not of f_type DTYPE_VNODE we
	 * currently can't handle pipes. This will be fixed as
	 * soon as we have signed kexts so we are allowed to use the kernel 
	 * interface to write to PIPE objects.
	 */
	if (file_vnode_withvid(fd, &vp, NULL))
	       return (EBADF);	

	error = dmu_recvbackup(zc->zc_value, &zc->zc_begin_record,
	    &zc->zc_cookie, (boolean_t)zc->zc_guid, vp,
	    zc->zc_history_offset);

	new_off = zc->zc_history_offset + zc->zc_cookie;
	
	/* This was implemented as VOP_SEEK but we don't support that
	 * and all the SEEK does is this boundry checking
	 */
	if ((new_off < 0 || new_off > MAXOFFSET_T))
			error = EINVAL;
	else
		zc->zc_history_offset = new_off;
	
	file_drop(fd);
#else
	fp = getf(fd);
	if (fp == NULL)
		return (EBADF);
	error = dmu_recvbackup(zc->zc_value, &zc->zc_begin_record,
	    &zc->zc_cookie, (boolean_t)zc->zc_guid, fp->f_vnode,
	    fp->f_offset);

	new_off = fp->f_offset + zc->zc_cookie;
	if (VOP_SEEK(fp->f_vnode, fp->f_offset, &new_off) == 0)
		fp->f_offset = new_off;

	releasef(fd);
#endif /* __APPLE__ */
	return (error);
}

static int
zfs_ioc_sendbackup(zfs_cmd_t *zc)
{
	objset_t *fromsnap = NULL;
	objset_t *tosnap;
#ifdef __APPLE__
	vnode_t *vp;
#else
	file_t *fp;
#endif /* __APPLE__ */
	int error;

	error = dmu_objset_open(zc->zc_name, DMU_OST_ANY,
	    DS_MODE_STANDARD | DS_MODE_READONLY, &tosnap);
	if (error)
		return (error);

	if (zc->zc_value[0] != '\0') {
		char buf[MAXPATHLEN];
		char *cp;

		(void) strncpy(buf, zc->zc_name, sizeof (buf));
		cp = strchr(buf, '@');
		if (cp)
			*(cp+1) = 0;
		(void) strncat(buf, zc->zc_value, sizeof (buf));
		error = dmu_objset_open(buf, DMU_OST_ANY,
		    DS_MODE_STANDARD | DS_MODE_READONLY, &fromsnap);
		if (error) {
			dmu_objset_close(tosnap);
			return (error);
		}
	}

#ifdef __APPLE__
	/*XXX NOEL: due to the fact that BSD doesn't support 
	 * vnodes for things not of f_type DTYPE_VNODE we
	 * currently can't handle pipes. This will be fixed as
	 * soon as we have signed kexts so we are allowed to use the kernel 
	 * interface to write to PIPE objects.
	 */
	if (file_vnode_withvid(zc->zc_cookie, &vp, NULL))
#else
	fp = getf(zc->zc_cookie);
	if (fp == NULL) 
#endif /* __APPLE__ */
	{
		dmu_objset_close(tosnap);
		if (fromsnap)
			dmu_objset_close(fromsnap);
		return (EBADF);
	}

#ifdef __APPLE__
	error = dmu_sendbackup(tosnap, fromsnap, vp);

	file_drop(zc->zc_cookie);
#else
	error = dmu_sendbackup(tosnap, fromsnap, fp->f_vnode);

	releasef(zc->zc_cookie);
#endif /* __APPLE__ */
	if (fromsnap)
		dmu_objset_close(fromsnap);
	dmu_objset_close(tosnap);
	return (error);
}

static int
zfs_ioc_inject_fault(zfs_cmd_t *zc)
{
	int id, error;

	error = zio_inject_fault(zc->zc_name, (int)zc->zc_guid, &id,
	    &zc->zc_inject_record);

	if (error == 0)
		zc->zc_guid = (uint64_t)id;

	return (error);
}

static int
zfs_ioc_clear_fault(zfs_cmd_t *zc)
{
	return (zio_clear_fault((int)zc->zc_guid));
}

static int
zfs_ioc_inject_list_next(zfs_cmd_t *zc)
{
	int id = (int)zc->zc_guid;
	int error;

	error = zio_inject_list_next(&id, zc->zc_name, sizeof (zc->zc_name),
	    &zc->zc_inject_record);

	zc->zc_guid = id;

	return (error);
}

static int
zfs_ioc_error_log(zfs_cmd_t *zc)
{
	spa_t *spa;
	int error;
	size_t count = (size_t)zc->zc_nvlist_dst_size;

	if ((error = spa_open(zc->zc_name, &spa, FTAG)) != 0)
		return (error);

	error = spa_get_errlog(spa, (void *)(uintptr_t)zc->zc_nvlist_dst,
	    &count);
	if (error == 0)
		zc->zc_nvlist_dst_size = count;
	else
		zc->zc_nvlist_dst_size = spa_get_errlog_size(spa);

	spa_close(spa, FTAG);

	return (error);
}

static int
zfs_ioc_clear(zfs_cmd_t *zc)
{
	spa_t *spa;
	vdev_t *vd;
	uint64_t txg;
	int error;

	if ((error = spa_open(zc->zc_name, &spa, FTAG)) != 0)
		return (error);

	txg = spa_vdev_enter(spa);

	if (zc->zc_guid == 0) {
		vd = NULL;
	} else if ((vd = spa_lookup_by_guid(spa, zc->zc_guid)) == NULL) {
		(void) spa_vdev_exit(spa, NULL, txg, ENODEV);
		spa_close(spa, FTAG);
		return (ENODEV);
	}

	vdev_clear(spa, vd);

	(void) spa_vdev_exit(spa, NULL, txg, 0);

	spa_close(spa, FTAG);

	return (0);
}

static int
zfs_ioc_promote(zfs_cmd_t *zc)
{
	char *cp;

	/*
	 * We don't need to unmount *all* the origin fs's snapshots, but
	 * it's easier.
	 */
	cp = strchr(zc->zc_value, '@');
	if (cp)
		*cp = '\0';
	(void) dmu_objset_find(zc->zc_value,
	    zfs_unmount_snap, NULL, DS_FIND_SNAPSHOTS);
	return (dsl_dataset_promote(zc->zc_name));
}

/*
 * We don't want to have a hard dependency
 * against some special symbols in sharefs
 * and nfs.  Determine them if needed when
 * the first file system is shared.
 * Neither sharefs or nfs are unloadable modules.
 */
int (*zexport_fs)(void *arg);
#ifndef __APPLE__
int (*zshare_fs)(enum sharefs_sys_op, share_t *, uint32_t);
#endif

int zfs_share_inited;
#ifndef __APPLE__
ddi_modhandle_t nfs_mod;
ddi_modhandle_t sharefs_mod;
#endif
kmutex_t zfs_share_lock;

static int
zfs_ioc_share(zfs_cmd_t *zc)
{
#ifndef __APPLE__
	int error;
	int opcode;

	if (zfs_share_inited == 0) {
		mutex_enter(&zfs_share_lock);
		nfs_mod = ddi_modopen("fs/nfs", KRTLD_MODE_FIRST, &error);
		sharefs_mod = ddi_modopen("fs/sharefs",
		    KRTLD_MODE_FIRST, &error);
		if (nfs_mod == NULL || sharefs_mod == NULL) {
			mutex_exit(&zfs_share_lock);
			return (ENOSYS);
		}
		if (zexport_fs == NULL && ((zexport_fs = (int (*)(void *))
		    ddi_modsym(nfs_mod, "nfs_export", &error)) == NULL)) {
			mutex_exit(&zfs_share_lock);
			return (ENOSYS);
		}

		if (zshare_fs == NULL && ((zshare_fs =
		    (int (*)(enum sharefs_sys_op, share_t *, uint32_t))
		    ddi_modsym(sharefs_mod, "sharefs_impl", &error)) == NULL)) {
			mutex_exit(&zfs_share_lock);
			return (ENOSYS);
		}
		zfs_share_inited = 1;
		mutex_exit(&zfs_share_lock);
	}

	if (error = zexport_fs((void *)(uintptr_t)zc->zc_share.z_exportdata))
		return (error);

	opcode = (zc->zc_share.z_sharetype == B_TRUE) ?
	    SHAREFS_ADD : SHAREFS_REMOVE;

	error = zshare_fs(opcode,
	    (void *)(uintptr_t)zc->zc_share.z_sharedata,
	    zc->zc_share.z_sharemax);

	return (error);

#endif /*!__APPLE__ */
}

/*
 * pool create, destroy, and export don't log the history as part of
 * zfsdev_ioctl, but rather zfs_ioc_pool_create, and zfs_ioc_pool_export
 * do the logging of those commands.
 */
static zfs_ioc_vec_t zfs_ioc_vec[] = {
	{ zfs_ioc_pool_create, zfs_secpolicy_config, POOL_NAME, B_FALSE },
	{ zfs_ioc_pool_destroy,	zfs_secpolicy_config, POOL_NAME, B_FALSE },
	{ zfs_ioc_pool_import, zfs_secpolicy_config, POOL_NAME, B_TRUE },
	{ zfs_ioc_pool_export, zfs_secpolicy_config, POOL_NAME, B_FALSE },
	{ zfs_ioc_pool_configs,	zfs_secpolicy_none, NO_NAME, B_FALSE },
	{ zfs_ioc_pool_stats, zfs_secpolicy_read, POOL_NAME, B_FALSE },
	{ zfs_ioc_pool_tryimport, zfs_secpolicy_config, NO_NAME, B_FALSE },
	{ zfs_ioc_pool_scrub, zfs_secpolicy_config, POOL_NAME, B_TRUE },
	{ zfs_ioc_pool_freeze, zfs_secpolicy_config, NO_NAME, B_FALSE },
	{ zfs_ioc_pool_upgrade,	zfs_secpolicy_config, POOL_NAME, B_TRUE },
	{ zfs_ioc_pool_get_history, zfs_secpolicy_config, POOL_NAME, B_FALSE },
	{ zfs_ioc_vdev_add, zfs_secpolicy_config, POOL_NAME, B_TRUE },
	{ zfs_ioc_vdev_remove, zfs_secpolicy_config, POOL_NAME, B_TRUE },
	{ zfs_ioc_vdev_set_state, zfs_secpolicy_config,	POOL_NAME, B_TRUE },
	{ zfs_ioc_vdev_attach, zfs_secpolicy_config, POOL_NAME, B_TRUE },
	{ zfs_ioc_vdev_detach, zfs_secpolicy_config, POOL_NAME, B_TRUE },
	{ zfs_ioc_vdev_setpath,	zfs_secpolicy_config, POOL_NAME, B_FALSE },
	{ zfs_ioc_objset_stats,	zfs_secpolicy_read, DATASET_NAME, B_FALSE },
	{ zfs_ioc_dataset_list_next, zfs_secpolicy_read,
	    DATASET_NAME, B_FALSE },
	{ zfs_ioc_snapshot_list_next, zfs_secpolicy_read,
	    DATASET_NAME, B_FALSE },
	{ zfs_ioc_set_prop, zfs_secpolicy_none, DATASET_NAME, B_TRUE },
	{ zfs_ioc_create_minor,	zfs_secpolicy_minor, DATASET_NAME, B_FALSE },
	{ zfs_ioc_remove_minor,	zfs_secpolicy_minor, DATASET_NAME, B_FALSE },
	{ zfs_ioc_create, zfs_secpolicy_create, DATASET_NAME, B_TRUE },
	{ zfs_ioc_destroy, zfs_secpolicy_destroy, DATASET_NAME, B_TRUE },
	{ zfs_ioc_rollback, zfs_secpolicy_rollback, DATASET_NAME, B_TRUE },
	{ zfs_ioc_rename, zfs_secpolicy_rename,	DATASET_NAME, B_TRUE },
	{ zfs_ioc_recvbackup, zfs_secpolicy_receive, DATASET_NAME, B_TRUE },
	{ zfs_ioc_sendbackup, zfs_secpolicy_send, DATASET_NAME, B_TRUE },
	{ zfs_ioc_inject_fault,	zfs_secpolicy_inject, NO_NAME, B_FALSE },
	{ zfs_ioc_clear_fault, zfs_secpolicy_inject, NO_NAME, B_FALSE },
	{ zfs_ioc_inject_list_next, zfs_secpolicy_inject, NO_NAME, B_FALSE },
	{ zfs_ioc_error_log, zfs_secpolicy_inject, POOL_NAME, B_FALSE },
	{ zfs_ioc_clear, zfs_secpolicy_config, POOL_NAME, B_TRUE },
	{ zfs_ioc_promote, zfs_secpolicy_promote, DATASET_NAME, B_TRUE },
	{ zfs_ioc_destroy_snaps, zfs_secpolicy_destroy,	DATASET_NAME, B_TRUE },
	{ zfs_ioc_snapshot, zfs_secpolicy_snapshot, DATASET_NAME, B_TRUE },
	{ zfs_ioc_dsobj_to_dsname, zfs_secpolicy_config, POOL_NAME, B_FALSE },
	{ zfs_ioc_obj_to_path, zfs_secpolicy_config, NO_NAME, B_FALSE },
	{ zfs_ioc_pool_set_props, zfs_secpolicy_config,	POOL_NAME, B_TRUE },
	{ zfs_ioc_pool_get_props, zfs_secpolicy_read, POOL_NAME, B_FALSE },
	{ zfs_ioc_set_fsacl, zfs_secpolicy_fsacl, DATASET_NAME, B_TRUE },
	{ zfs_ioc_get_fsacl, zfs_secpolicy_read, DATASET_NAME, B_FALSE },
	{ zfs_ioc_iscsi_perm_check, zfs_secpolicy_iscsi,
	    DATASET_NAME, B_FALSE },
	{ zfs_ioc_share, zfs_secpolicy_share, DATASET_NAME, B_FALSE },
	{ zfs_ioc_inherit_prop, zfs_secpolicy_inherit, DATASET_NAME, B_TRUE },
};

#ifdef __APPLE__
kmutex_t zfs_ioctl_users_list_mtx;
list_t zfs_ioctl_users_list;

struct zfs_ioctl_user {
	list_node_t ziu_node;
	const struct proc *ziu_proc;
};

typedef struct zfs_ioctl_user zfs_ioctl_user_t;

/* check if process p is in the list of version-checked ioctl() users. */
zfs_ioctl_user_t *zfs_ioctl_users_find_impl(const struct proc *p) {
	zfs_ioctl_user_t *ioctl_user = list_head(&zfs_ioctl_users_list);
	while (ioctl_user != 0) {
		if (ioctl_user->ziu_proc == p)
			break;
		ioctl_user = list_next(&zfs_ioctl_users_list, ioctl_user);
	}
	return(ioctl_user);
}

zfs_ioctl_user_t *zfs_ioctl_users_find(const struct proc *p) {
	mutex_enter(&zfs_ioctl_users_list_mtx);
	zfs_ioctl_user_t *ioctl_user = zfs_ioctl_users_find_impl(p);
	mutex_exit(&zfs_ioctl_users_list_mtx);
	return(ioctl_user);
}

/* add process p to list of version-checked processes. */
void zfs_ioctl_users_add(const struct proc *p) {
	zfs_ioctl_user_t *ioctl_user = kmem_zalloc(sizeof (zfs_ioctl_user_t), KM_SLEEP);
	ioctl_user->ziu_proc = p;
	mutex_enter(&zfs_ioctl_users_list_mtx);
	if (zfs_ioctl_users_find_impl(p)) {
		/* someone else was quicker. */
		kmem_free(ioctl_user, sizeof(zfs_ioctl_user_t));
	} else {
		list_insert_tail(&zfs_ioctl_users_list, ioctl_user);
	}
	mutex_exit(&zfs_ioctl_users_list_mtx);
}

/* remove process p from list of version-checked processes. */
void zfs_ioctl_users_remove(const struct proc *p) {
	mutex_enter(&zfs_ioctl_users_list_mtx);
	zfs_ioctl_user_t *ioctl_user = zfs_ioctl_users_find_impl(p);
	if (ioctl_user) {
		list_remove(&zfs_ioctl_users_list, ioctl_user);
		kmem_free(ioctl_user, sizeof(zfs_ioctl_user_t));
	}
	mutex_exit(&zfs_ioctl_users_list_mtx);
}
#endif

static int
#ifdef __APPLE__
 zfsdev_ioctl(dev_t dev, u_long cmd, caddr_t data,  __unused int flag, struct proc *p)
#else
 zfsdev_ioctl(dev_t dev, int cmd, intptr_t arg, int flag, cred_t *cr, int *rvalp)
#endif
{
	zfs_cmd_t *zc;
	uint_t vec;
	int error, rc;
#ifdef __APPLE__
	cred_t *cr;
	// 10a286 vfs_context_t ctx = vfs_context_create(NULL)
#else
	if (getminor(dev) != 0)
		return (zvol_ioctl(dev, cmd, data, p));
#endif /* __APPLE__ */

#ifdef __APPLE__
	vec = ZFS_IOC_NUM(cmd);
	zc = (zfs_cmd_t *)data;
	// 10a286 ctx = vfs_context_create(NULL) // again?
	cr = (uintptr_t)NOCRED;    /* wants vfs_context_current() */
	zc->zc_dev = dev;
	/* check vec number for out-of-bound. */
	if ( (vec != ZFS_IOC_NUM(ZFS_IOC__VERSION_CHECK)) &&
		 (vec >= sizeof (zfs_ioc_vec) / sizeof (zfs_ioc_vec[0])) ) {
		printf("zfs_ioctl.c: %s: ioctl vec %d out of bounds, proc: %p\n", __func__, vec, p);
		return (EINVAL);
	}

	/* check if the calling process has successfully proved its zfs version.
	 * if not, reject everything except the version check. */
	zfs_ioctl_user_t *ioctl_user = zfs_ioctl_users_find(p);
	if (!ioctl_user) {
		/* version check not yet performed. */
		if (vec != ZFS_IOC_NUM(ZFS_IOC__VERSION_CHECK)) {
			return(EINVAL);
		}
		if (strncmp(zc->zc_name, __STRING(MACZFS_ID), sizeof(zc->zc_name)))
			return (EINVAL);
		if (zc->zc_value[0] != ZFS_IOC_NUM(ZFS_IOC__LAST_USED))
			return (EINVAL);
		if (zc->zc_value[1] != MACZFS_VERS_MAJOR)
			return (EINVAL);
		if (zc->zc_value[2] != MACZFS_VERS_MINOR)
			return (EINVAL);
		/* patch level not checked, it is not supposed to generated
		 * incompatibilities */
		zfs_ioctl_users_add(p);
		return 0;
	} else {
		/* intercept repeated version check and return success. */
		if (vec == ZFS_IOC_NUM(ZFS_IOC__VERSION_CHECK))
			return 0;
	}

	error = zfs_ioc_vec[vec].zvec_secpolicy(zc, cr);
	// 10a286 vfs_context_rele(ctx);
#else
	vec = cmd - ZFS_IOC;
	ASSERT3U(getmajor(dev), ==, ddi_driver_major(zfs_dip));

	if (vec >= sizeof (zfs_ioc_vec) / sizeof (zfs_ioc_vec[0]))
		return (EINVAL);

	zc = kmem_zalloc(sizeof (zfs_cmd_t), KM_SLEEP);

	error = xcopyin((void *)arg, zc, sizeof (zfs_cmd_t));

	if (error == 0)
		error = zfs_ioc_vec[vec].zvec_secpolicy(zc, cr);
#endif

	/*
	 * Ensure that all pool/dataset names are valid before we pass down to
	 * the lower layers.
	 */
	if (error == 0) {
		zc->zc_name[sizeof (zc->zc_name) - 1] = '\0';
		switch (zfs_ioc_vec[vec].zvec_namecheck) {
		case POOL_NAME:
			if (pool_namecheck(zc->zc_name, NULL, NULL) != 0)
				error = EINVAL;
			break;

		case DATASET_NAME:
			if (dataset_namecheck(zc->zc_name, NULL, NULL) != 0)
				error = EINVAL;
			break;

		case NO_NAME:
			break;
		}
	}

	if (error == 0)
		error = zfs_ioc_vec[vec].zvec_func(zc);
#ifdef __APPLE__
	if (error == 0 && zfs_ioc_vec[vec].zvec_his_log == B_TRUE)
		zfs_log_history(zc);

	/* 
	 * Return the real error in zc_ioc_error so the ioctl
	 * call always does a copyout of the zc data
	 */
	zc->zc_ioc_error = error;
	error = 0;
#else
	rc = xcopyout(zc, (void *)arg, sizeof (zfs_cmd_t));
	if (error == 0) {
		error = rc;
		if (zfs_ioc_vec[vec].zvec_his_log == B_TRUE)
			zfs_log_history(zc);
	}

	kmem_free(zc, sizeof (zfs_cmd_t));
#endif /* !__APPLE__ */
	return (error);
}

#ifdef __APPLE__
int zfsdev_open(void) {
	return(0);
}

int zfsdev_close(__unused dev_t dev, __unused int flag, __unused int mode, struct proc *p) {
	zfs_ioctl_users_remove(p);
	return(0);
}
#endif

#ifndef __APPLE__
static int
zfs_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	if (cmd != DDI_ATTACH)
		return (DDI_FAILURE);

	if (ddi_create_minor_node(dip, "zfs", S_IFCHR, 0,
	    DDI_PSEUDO, 0) == DDI_FAILURE)
		return (DDI_FAILURE);

	zfs_dip = dip;

	ddi_report_dev(dip);

	return (DDI_SUCCESS);
}

static int
zfs_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	if (spa_busy() || zfs_busy() || zvol_busy())
		return (DDI_FAILURE);

	if (cmd != DDI_DETACH)
		return (DDI_FAILURE);

	zfs_dip = NULL;

	ddi_prop_remove_all(dip);
	ddi_remove_minor_node(dip, NULL);

	return (DDI_SUCCESS);
}

/*ARGSUSED*/
static int
zfs_info(dev_info_t *dip, ddi_info_cmd_t infocmd, void *arg, void **result)
{
	switch (infocmd) {
	case DDI_INFO_DEVT2DEVINFO:
		*result = zfs_dip;
		return (DDI_SUCCESS);

	case DDI_INFO_DEVT2INSTANCE:
		*result = (void *)0;
		return (DDI_SUCCESS);
	}

	return (DDI_FAILURE);
}
#endif /*!__APPLE__*/

#ifdef __APPLE__
static struct cdevsw zfs_cdevsw =
{
	zfsdev_open,		/* open */
	zfsdev_close,		/* close */
	zvol_read,		/* read */
	zvol_write,		/* write */
	zfsdev_ioctl,		/* ioctl */
	(stop_fcn_t *)&nulldev,	/* stop */
	(reset_fcn_t *)&nulldev,/* reset */
	NULL,			/* tty's */
	eno_select,		/* select */
	eno_mmap,		/* mmap */
	eno_strat,		/* strategy */
	eno_getc,		/* getc */
	eno_putc,		/* putc */
	0			/* type */
};

static int zfs_ioctl_installed = 0;
static int zfs_major = 0;
static void * zfs_devnode = NULL;

#define ZFS_MAJOR  -24

void
zfs_ioctl_init(void)
{
	dev_t dev;

	if (zfs_ioctl_installed)
		return;

	zfs_major = cdevsw_add(ZFS_MAJOR, &zfs_cdevsw);
	if (zfs_major < 0) {
		printf("zfs_ioctl_init: failed to allocate a major number!\n");
		return;
	}
	zfs_ioctl_installed = 1;

	list_create(&zfs_ioctl_users_list, sizeof(zfs_ioctl_user_t), 0);
	mutex_init(&zfs_ioctl_users_list_mtx, NULL, MUTEX_DEFAULT, NULL);
	
	dev = zfs_major << 24;
	zfs_devnode = devfs_make_node(dev, DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "zfs", 0);

	spa_init(FREAD | FWRITE);
	zvol_init(); // Removd in 10a286
}

void
zfs_ioctl_fini(void)
{
	if (spa_busy() || zvol_busy() || zio_injection_enabled) {
		printf("zfs_ioctl_fini: sorry we're busy\n");
		return;
	}

	zvol_fini(); // Removed in 10a286
	spa_fini();

	if (zfs_devnode) {
		devfs_remove(zfs_devnode);
		zfs_devnode = NULL;
	}

	mutex_destroy(&zfs_ioctl_users_list_mtx);
	list_destroy(&zfs_ioctl_users_list);
	
	if (zfs_major) {
		cdevsw_remove(zfs_major, &zfs_cdevsw);
		zfs_major = 0;
	}
}

#else /* Open Solaris */

/*
 * OK, so this is a little weird.
 *
 * /dev/zfs is the control node, i.e. minor 0.
 * /dev/zvol/[r]dsk/pool/dataset are the zvols, minor > 0.
 *
 * /dev/zfs has basically nothing to do except serve up ioctls,
 * so most of the standard driver entry points are in zvol.c.
 */
static struct cb_ops zfs_cb_ops = {
	zvol_open,	/* open */
	zvol_close,	/* close */
	zvol_strategy,	/* strategy */
	nodev,		/* print */
	nodev,		/* dump */
	zvol_read,	/* read */
	zvol_write,	/* write */
	zfsdev_ioctl,	/* ioctl */
	nodev,		/* devmap */
	nodev,		/* mmap */
	nodev,		/* segmap */
	nochpoll,	/* poll */
	ddi_prop_op,	/* prop_op */
	NULL,		/* streamtab */
	D_NEW | D_MP | D_64BIT,		/* Driver compatibility flag */
	CB_REV,		/* version */
	nodev,		/* async read */
	nodev,		/* async write */
};

static struct dev_ops zfs_dev_ops = {
	DEVO_REV,	/* version */
	0,		/* refcnt */
	zfs_info,	/* info */
	nulldev,	/* identify */
	nulldev,	/* probe */
	zfs_attach,	/* attach */
	zfs_detach,	/* detach */
	nodev,		/* reset */
	&zfs_cb_ops,	/* driver operations */
	NULL		/* no bus operations */
};

static struct modldrv zfs_modldrv = {
	&mod_driverops, "ZFS storage pool version " SPA_VERSION_STRING,
	    &zfs_dev_ops
};

static struct modlinkage modlinkage = {
	MODREV_1,
	(void *)&zfs_modlfs,
	(void *)&zfs_modldrv,
	NULL
};


uint_t zfs_fsyncer_key;

int
_init(void)
{
	int error;

	spa_init(FREAD | FWRITE);
	zfs_init();
	zvol_init();

	if ((error = mod_install(&modlinkage)) != 0) {
		zvol_fini();
		zfs_fini();
		spa_fini();
		return (error);
	}

	tsd_create(&zfs_fsyncer_key, NULL);

	error = ldi_ident_from_mod(&modlinkage, &zfs_li);
	ASSERT(error == 0);
	mutex_init(&zfs_share_lock, NULL, MUTEX_DEFAULT, NULL);

	return (0);
}

int
_fini(void)
{
	int error;

	if (spa_busy() || zfs_busy() || zvol_busy() || zio_injection_enabled)
		return (EBUSY);

	if ((error = mod_remove(&modlinkage)) != 0)
		return (error);

	zvol_fini();
	zfs_fini();
	spa_fini();
	if (zfs_share_inited) {
		(void) ddi_modclose(nfs_mod);
		(void) ddi_modclose(sharefs_mod);
	}

	tsd_destroy(&zfs_fsyncer_key);
	ldi_ident_release(zfs_li);
	zfs_li = NULL;
	mutex_destroy(&zfs_share_lock);

	return (error);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&modlinkage, modinfop));
}
#endif /*__APPLE__*/
