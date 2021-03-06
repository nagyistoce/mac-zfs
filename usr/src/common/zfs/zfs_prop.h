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
 */

#ifndef	_ZFS_PROP_H
#define	_ZFS_PROP_H

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#include <sys/fs/zfs.h>
#include <sys/types.h>

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * For index types (e.g. compression and checksum), we want the numeric value
 * in the kernel, but the string value in userland.
 */
typedef enum {
	PROP_TYPE_NUMBER,	/* numeric value */
	PROP_TYPE_STRING,	/* string value */
	PROP_TYPE_INDEX		/* numeric value indexed by string */
} zfs_proptype_t;

zfs_proptype_t zfs_prop_get_type(zfs_prop_t);
zfs_proptype_t zpool_prop_get_type(zpool_prop_t);
size_t zfs_prop_width(zfs_prop_t, boolean_t *);
boolean_t zfs_prop_delegatable(zfs_prop_t prop);
void zfs_prop_init(void);

#ifdef	__cplusplus
}
#endif

#endif	/* _ZFS_PROP_H */
