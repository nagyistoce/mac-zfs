/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
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
 * Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#include <stdio.h>
#include <stdlib.h>
#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/dmu.h>
#include <sys/zap.h>
#include <sys/fs/zfs.h>
#include <sys/zfs_znode.h>
#include <sys/vdev.h>
#include <sys/vdev_impl.h>
#include <sys/metaslab_impl.h>
#include <sys/dmu_objset.h>
#include <sys/dsl_dir.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_pool.h>
#include <sys/dbuf.h>
#include <sys/zil.h>
#include <sys/zil_impl.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/dmu_traverse.h>
#include <sys/zio_checksum.h>
#include <sys/zio_compress.h>

const char cmdname[] = "zdb";
uint8_t dump_opt[256];

typedef void object_viewer_t(objset_t *, uint64_t, void *data, size_t size);

extern void dump_intent_log(zilog_t *);
uint64_t *zopt_object = NULL;
int zopt_objects = 0;
int zdb_advance = ADVANCE_PRE;
zbookmark_t zdb_noread = { 0, 0, ZB_NO_LEVEL, 0 };

/*
 * These libumem hooks provide a reasonable set of defaults for the allocator's
 * debugging facilities.
 */
const char *
_umem_debug_init()
{
	return ("default,verbose"); /* $UMEM_DEBUG setting */
}

const char *
_umem_logging_init(void)
{
	return ("fail,contents"); /* $UMEM_LOGGING setting */
}

static void
usage(void)
{
	(void) fprintf(stderr,
	    "Usage: %s [-udibcsvLU] [-O order] [-B os:obj:level:blkid] "
	    "dataset [object...]\n"
	    "       %s -C [pool]\n"
	    "       %s -l dev\n",
	    cmdname, cmdname, cmdname);

	(void) fprintf(stderr, "	-u uberblock\n");
	(void) fprintf(stderr, "	-d datasets\n");
	(void) fprintf(stderr, "        -C cached pool configuration\n");
	(void) fprintf(stderr, "	-i intent logs\n");
	(void) fprintf(stderr, "	-b block statistics\n");
	(void) fprintf(stderr, "	-c checksum all data blocks\n");
	(void) fprintf(stderr, "	-s report stats on zdb's I/O\n");
	(void) fprintf(stderr, "	-v verbose (applies to all others)\n");
	(void) fprintf(stderr, "        -l dump label contents\n");
	(void) fprintf(stderr, "	-L live pool (allows some errors)\n");
	(void) fprintf(stderr, "	-O [!]<pre|post|prune|data|holes> "
	    "visitation order\n");
	(void) fprintf(stderr, "	-U use zpool.cache in /tmp\n");
	(void) fprintf(stderr, "	-B objset:object:level:blkid -- "
	    "simulate bad block\n");
	(void) fprintf(stderr, "Specify an option more than once (e.g. -bb) "
	    "to make only that option verbose\n");
	(void) fprintf(stderr, "Default is to dump everything non-verbosely\n");
	exit(1);
}

static void
fatal(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	(void) fprintf(stderr, "%s: ", cmdname);
	(void) vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void) fprintf(stderr, "\n");

	exit(1);
}

static void
dump_nvlist(nvlist_t *list, int indent)
{
	nvpair_t *elem = NULL;

	while ((elem = nvlist_next_nvpair(list, elem)) != NULL) {
		switch (nvpair_type(elem)) {
		case DATA_TYPE_STRING:
			{
				char *value;

				VERIFY(nvpair_value_string(elem, &value) == 0);
				(void) printf("%*s%s='%s'\n", indent, "",
				    nvpair_name(elem), value);
			}
			break;

		case DATA_TYPE_UINT64:
			{
				uint64_t value;

				VERIFY(nvpair_value_uint64(elem, &value) == 0);
				(void) printf("%*s%s=%llu\n", indent, "",
				    nvpair_name(elem), (u_longlong_t)value);
			}
			break;

		case DATA_TYPE_NVLIST:
			{
				nvlist_t *value;

				VERIFY(nvpair_value_nvlist(elem, &value) == 0);
				(void) printf("%*s%s\n", indent, "",
				    nvpair_name(elem));
				dump_nvlist(value, indent + 4);
			}
			break;

		case DATA_TYPE_NVLIST_ARRAY:
			{
				nvlist_t **value;
				uint_t c, count;

				VERIFY(nvpair_value_nvlist_array(elem, &value,
				    &count) == 0);

				for (c = 0; c < count; c++) {
					(void) printf("%*s%s[%u]\n", indent, "",
					    nvpair_name(elem), c);
					dump_nvlist(value[c], indent + 8);
				}
			}
			break;

		default:

			(void) printf("bad config type %d for %s\n",
			    nvpair_type(elem), nvpair_name(elem));
		}
	}
}

/* ARGSUSED */
static void
dump_packed_nvlist(objset_t *os, uint64_t object, void *data, size_t size)
{
	nvlist_t *nv;
	size_t nvsize = *(uint64_t *)data;
	char *packed = umem_alloc(nvsize, UMEM_NOFAIL);

	dmu_read(os, object, 0, nvsize, packed);

	VERIFY(nvlist_unpack(packed, nvsize, &nv, 0) == 0);

	umem_free(packed, nvsize);

	dump_nvlist(nv, 8);

	nvlist_free(nv);
}

const char dump_zap_stars[] = "****************************************";
const int dump_zap_width = sizeof (dump_zap_stars) - 1;

static void
dump_zap_histogram(uint64_t histo[ZAP_HISTOGRAM_SIZE])
{
	int i;
	int minidx = ZAP_HISTOGRAM_SIZE - 1;
	int maxidx = 0;
	uint64_t max = 0;

	for (i = 0; i < ZAP_HISTOGRAM_SIZE; i++) {
		if (histo[i] > max)
			max = histo[i];
		if (histo[i] > 0 && i > maxidx)
			maxidx = i;
		if (histo[i] > 0 && i < minidx)
			minidx = i;
	}

	if (max < dump_zap_width)
		max = dump_zap_width;

	for (i = minidx; i <= maxidx; i++)
		(void) printf("\t\t\t%u: %6llu %s\n", i, (u_longlong_t)histo[i],
		    &dump_zap_stars[(max - histo[i]) * dump_zap_width / max]);
}

static void
dump_zap_stats(objset_t *os, uint64_t object)
{
	int error;
	zap_stats_t zs;

	error = zap_get_stats(os, object, &zs);
	if (error)
		return;

	if (zs.zs_ptrtbl_len == 0) {
		ASSERT(zs.zs_num_blocks == 1);
		(void) printf("\tmicrozap: %llu bytes, %llu entries\n",
		    (u_longlong_t)zs.zs_blocksize,
		    (u_longlong_t)zs.zs_num_entries);
		return;
	}

	(void) printf("\tFat ZAP stats:\n");
	(void) printf("\t\tPointer table: %llu elements\n",
	    (u_longlong_t)zs.zs_ptrtbl_len);
	(void) printf("\t\tZAP entries: %llu\n",
	    (u_longlong_t)zs.zs_num_entries);
	(void) printf("\t\tLeaf blocks: %llu\n",
	    (u_longlong_t)zs.zs_num_leafs);
	(void) printf("\t\tTotal blocks: %llu\n",
	    (u_longlong_t)zs.zs_num_blocks);
	(void) printf("\t\tOversize blocks: %llu\n",
	    (u_longlong_t)zs.zs_num_blocks_large);

	(void) printf("\t\tLeafs with 2^n pointers:\n");
	dump_zap_histogram(zs.zs_leafs_with_2n_pointers);

	(void) printf("\t\tLeafs with n chained:\n");
	dump_zap_histogram(zs.zs_leafs_with_n_chained);

	(void) printf("\t\tBlocks with n*5 entries:\n");
	dump_zap_histogram(zs.zs_blocks_with_n5_entries);

	(void) printf("\t\tBlocks n/10 full:\n");
	dump_zap_histogram(zs.zs_blocks_n_tenths_full);

	(void) printf("\t\tEntries with n chunks:\n");
	dump_zap_histogram(zs.zs_entries_using_n_chunks);

	(void) printf("\t\tBuckets with n entries:\n");
	dump_zap_histogram(zs.zs_buckets_with_n_entries);
}

/*ARGSUSED*/
static void
dump_none(objset_t *os, uint64_t object, void *data, size_t size)
{
}

/*ARGSUSED*/
void
dump_uint8(objset_t *os, uint64_t object, void *data, size_t size)
{
}

/*ARGSUSED*/
static void
dump_uint64(objset_t *os, uint64_t object, void *data, size_t size)
{
}

/*ARGSUSED*/
static void
dump_zap(objset_t *os, uint64_t object, void *data, size_t size)
{
	zap_cursor_t zc;
	zap_attribute_t attr;
	void *prop;
	int i;

	dump_zap_stats(os, object);
	(void) printf("\n");

	for (zap_cursor_init(&zc, os, object);
	    zap_cursor_retrieve(&zc, &attr) == 0;
	    zap_cursor_advance(&zc)) {
		(void) printf("\t\t%s = ", attr.za_name);
		if (attr.za_num_integers == 0) {
			(void) printf("\n");
			continue;
		}
		prop = umem_zalloc(attr.za_num_integers *
		    attr.za_integer_length, UMEM_NOFAIL);
		(void) zap_lookup(os, object, attr.za_name,
		    attr.za_integer_length, attr.za_num_integers, prop);
		if (attr.za_integer_length == 1) {
			(void) printf("%s", (char *)prop);
		} else {
			for (i = 0; i < attr.za_num_integers; i++) {
				switch (attr.za_integer_length) {
				case 2:
					(void) printf("%u ",
					    ((uint16_t *)prop)[i]);
					break;
				case 4:
					(void) printf("%u ",
					    ((uint32_t *)prop)[i]);
					break;
				case 8:
					(void) printf("%lld ",
					    (u_longlong_t)((int64_t *)prop)[i]);
					break;
				}
			}
		}
		(void) printf("\n");
		umem_free(prop, attr.za_num_integers * attr.za_integer_length);
	}
}

static void
dump_spacemap(objset_t *os, space_map_obj_t *smo, space_map_t *sm)
{
	uint64_t alloc, offset, entry;
	int mapshift = sm->sm_shift;
	uint64_t mapstart = sm->sm_start;
	char *ddata[] = { "ALLOC", "FREE", "CONDENSE", "INVALID" };

	if (smo->smo_object == 0)
		return;

	/*
	 * Print out the freelist entries in both encoded and decoded form.
	 */
	alloc = 0;
	for (offset = 0; offset < smo->smo_objsize; offset += sizeof (entry)) {
		dmu_read(os, smo->smo_object, offset, sizeof (entry), &entry);
		if (SM_DEBUG_DECODE(entry)) {
			(void) printf("\t\t[%4llu] %s: txg %llu, pass %llu\n",
			    (u_longlong_t)(offset / sizeof (entry)),
			    ddata[SM_DEBUG_ACTION_DECODE(entry)],
			    SM_DEBUG_TXG_DECODE(entry),
			    SM_DEBUG_SYNCPASS_DECODE(entry));
		} else {
			(void) printf("\t\t[%4llu]    %c  range:"
			    " %08llx-%08llx  size: %06llx\n",
			    (u_longlong_t)(offset / sizeof (entry)),
			    SM_TYPE_DECODE(entry) == SM_ALLOC ? 'A' : 'F',
			    (SM_OFFSET_DECODE(entry) << mapshift) + mapstart,
			    (SM_OFFSET_DECODE(entry) << mapshift) + mapstart +
			    (SM_RUN_DECODE(entry) << mapshift),
			    (SM_RUN_DECODE(entry) << mapshift));
			if (SM_TYPE_DECODE(entry) == SM_ALLOC)
				alloc += SM_RUN_DECODE(entry) << mapshift;
			else
				alloc -= SM_RUN_DECODE(entry) << mapshift;
		}
	}
	if (alloc != smo->smo_alloc) {
		(void) printf("space_map_object alloc (%llu) INCONSISTENT "
		    "with space map summary (%llu)\n",
		    (u_longlong_t)smo->smo_alloc, (u_longlong_t)alloc);
	}
}

static void
dump_metaslab(metaslab_t *msp)
{
	char freebuf[5];
	space_map_obj_t *smo = msp->ms_smo;
	vdev_t *vd = msp->ms_group->mg_vd;
	spa_t *spa = vd->vdev_spa;

	nicenum(msp->ms_map.sm_size - smo->smo_alloc, freebuf);

	if (dump_opt['d'] <= 5) {
		(void) printf("\t%10llx   %10llu   %5s\n",
		    (u_longlong_t)msp->ms_map.sm_start,
		    (u_longlong_t)smo->smo_object,
		    freebuf);
		return;
	}

	(void) printf(
	    "\tvdev %llu   offset %08llx   spacemap %4llu   free %5s\n",
	    (u_longlong_t)vd->vdev_id, (u_longlong_t)msp->ms_map.sm_start,
	    (u_longlong_t)smo->smo_object, freebuf);

	ASSERT(msp->ms_map.sm_size == (1ULL << vd->vdev_ms_shift));

	dump_spacemap(spa->spa_meta_objset, smo, &msp->ms_map);
}

static void
dump_metaslabs(spa_t *spa)
{
	vdev_t *rvd = spa->spa_root_vdev;
	vdev_t *vd;
	int c, m;

	(void) printf("\nMetaslabs:\n");

	for (c = 0; c < rvd->vdev_children; c++) {
		vd = rvd->vdev_child[c];

		spa_config_enter(spa, RW_READER);
		(void) printf("\n    vdev %llu = %s\n\n",
		    (u_longlong_t)vd->vdev_id, vdev_description(vd));
		spa_config_exit(spa);

		if (dump_opt['d'] <= 5) {
			(void) printf("\t%10s   %10s   %5s\n",
			    "offset", "spacemap", "free");
			(void) printf("\t%10s   %10s   %5s\n",
			    "------", "--------", "----");
		}
		for (m = 0; m < vd->vdev_ms_count; m++)
			dump_metaslab(vd->vdev_ms[m]);
		(void) printf("\n");
	}
}

static void
dump_dtl(vdev_t *vd, int indent)
{
	avl_tree_t *t = &vd->vdev_dtl_map.sm_root;
	spa_t *spa = vd->vdev_spa;
	space_seg_t *ss;
	vdev_t *pvd;
	int c;

	if (indent == 0)
		(void) printf("\nDirty time logs:\n\n");

	spa_config_enter(spa, RW_READER);
	(void) printf("\t%*s%s\n", indent, "", vdev_description(vd));
	spa_config_exit(spa);

	for (ss = avl_first(t); ss; ss = AVL_NEXT(t, ss)) {
		/*
		 * Everything in this DTL must appear in all parent DTL unions.
		 */
		for (pvd = vd; pvd; pvd = pvd->vdev_parent)
			ASSERT(vdev_dtl_contains(&pvd->vdev_dtl_map,
			    ss->ss_start, ss->ss_end - ss->ss_start));
		(void) printf("\t%*soutage [%llu,%llu] length %llu\n",
		    indent, "",
		    (u_longlong_t)ss->ss_start,
		    (u_longlong_t)ss->ss_end - 1,
		    (u_longlong_t)ss->ss_end - ss->ss_start);
	}

	(void) printf("\n");

	if (dump_opt['d'] > 5 && vd->vdev_children == 0) {
		dump_spacemap(vd->vdev_spa->spa_meta_objset, &vd->vdev_dtl,
		    &vd->vdev_dtl_map);
		(void) printf("\n");
	}

	for (c = 0; c < vd->vdev_children; c++)
		dump_dtl(vd->vdev_child[c], indent + 4);
}

/*ARGSUSED*/
static void
dump_dnode(objset_t *os, uint64_t object, void *data, size_t size)
{
}

static uint64_t
blkid2offset(dnode_phys_t *dnp, int level, uint64_t blkid)
{
	if (level < 0)
		return (blkid);

	return ((blkid << (level * (dnp->dn_indblkshift - SPA_BLKPTRSHIFT))) *
	    dnp->dn_datablkszsec << SPA_MINBLOCKSHIFT);
}

/* ARGSUSED */
static int
zdb_indirect_cb(traverse_blk_cache_t *bc, spa_t *spa, void *a)
{
	zbookmark_t *zb = &bc->bc_bookmark;
	blkptr_t *bp = &bc->bc_blkptr;
	dva_t *dva = &bp->blk_dva[0];
	void *data = bc->bc_data;
	dnode_phys_t *dnp = bc->bc_dnode;
	char buffer[300];
	int l;

	if (bc->bc_errno) {
		(void) sprintf(buffer,
		    "Error %d reading <%llu, %llu, %d, %llu>: ",
		    bc->bc_errno,
		    (u_longlong_t)zb->zb_objset,
		    (u_longlong_t)zb->zb_object,
		    zb->zb_level,
		    (u_longlong_t)zb->zb_blkid);
		goto out;
	}

	if (zb->zb_level == -1) {
		ASSERT3U(BP_GET_TYPE(bp), ==, DMU_OT_OBJSET);
		ASSERT3U(BP_GET_LEVEL(bp), ==, 0);
	} else {
		ASSERT3U(BP_GET_TYPE(bp), ==, dnp->dn_type);
		ASSERT3U(BP_GET_LEVEL(bp), ==, zb->zb_level);
	}

	if (zb->zb_level > 0) {
		uint64_t fill = 0;
		blkptr_t *bpx, *bpend;

		for (bpx = data, bpend = bpx + BP_GET_LSIZE(bp) / sizeof (*bpx);
		    bpx < bpend; bpx++) {
			if (bpx->blk_birth != 0) {
				ASSERT(bpx->blk_fill > 0);
				fill += bpx->blk_fill;
			} else {
				ASSERT(bpx->blk_fill == 0);
			}
		}
		ASSERT3U(fill, ==, bp->blk_fill);
	}

	if (zb->zb_level == 0 && dnp->dn_type == DMU_OT_DNODE) {
		uint64_t fill = 0;
		dnode_phys_t *dnx, *dnend;

		for (dnx = data, dnend = dnx + (BP_GET_LSIZE(bp)>>DNODE_SHIFT);
		    dnx < dnend; dnx++) {
			if (dnx->dn_type != DMU_OT_NONE)
				fill++;
		}
		ASSERT3U(fill, ==, bp->blk_fill);
	}

	(void) sprintf(buffer, "%16llx ",
	    (u_longlong_t)blkid2offset(dnp, zb->zb_level, zb->zb_blkid));

	ASSERT(zb->zb_level >= 0);

	for (l = dnp->dn_nlevels - 1; l >= -1; l--) {
		if (l == zb->zb_level) {
			(void) sprintf(buffer + strlen(buffer), "L%x",
			    zb->zb_level);
		} else {
			(void) sprintf(buffer + strlen(buffer), " ");
		}
	}

out:
	if (bp->blk_birth == 0) {
		(void) sprintf(buffer + strlen(buffer), "<hole>");
		(void) printf("%s\n", buffer);
	} else {
		// XXBP - Need to print number of active BPs here
		(void) sprintf(buffer + strlen(buffer),
		    "vdev=%llu off=%llx %llxL/%llxP/%llxA F=%llu B=%llu",
		    (u_longlong_t)DVA_GET_VDEV(dva),
		    (u_longlong_t)DVA_GET_OFFSET(dva),
		    (u_longlong_t)BP_GET_LSIZE(bp),
		    (u_longlong_t)BP_GET_PSIZE(bp),
		    (u_longlong_t)DVA_GET_ASIZE(dva),
		    (u_longlong_t)bp->blk_fill,
		    (u_longlong_t)bp->blk_birth);

		(void) printf("%s\n", buffer);
	}

	return (bc->bc_errno ? ERESTART : 0);
}

/*ARGSUSED*/
static void
dump_indirect(objset_t *os, uint64_t object, void *data, size_t size)
{
	traverse_handle_t *th;
	uint64_t objset = dmu_objset_id(os);
	int advance = zdb_advance;

	(void) printf("Indirect blocks:\n");

	if (object == 0)
		advance |= ADVANCE_DATA;

	th = traverse_init(dmu_objset_spa(os), zdb_indirect_cb, NULL, advance,
	    ZIO_FLAG_CANFAIL);
	th->th_noread = zdb_noread;

	traverse_add_dnode(th, 0, -1ULL, objset, object);

	while (traverse_more(th) == EAGAIN)
		continue;

	(void) printf("\n");

	traverse_fini(th);
}

/*ARGSUSED*/
static void
dump_dsl_dir(objset_t *os, uint64_t object, void *data, size_t size)
{
	dsl_dir_phys_t *dd = data;
	time_t crtime;
	char used[6], compressed[6], uncompressed[6], quota[6], resv[6];

	if (dd == NULL)
		return;

	ASSERT(size == sizeof (*dd));

	crtime = dd->dd_creation_time;
	nicenum(dd->dd_used_bytes, used);
	nicenum(dd->dd_compressed_bytes, compressed);
	nicenum(dd->dd_uncompressed_bytes, uncompressed);
	nicenum(dd->dd_quota, quota);
	nicenum(dd->dd_reserved, resv);

	(void) printf("\t\tcreation_time = %s", ctime(&crtime));
	(void) printf("\t\thead_dataset_obj = %llu\n",
	    (u_longlong_t)dd->dd_head_dataset_obj);
	(void) printf("\t\tparent_dir_obj = %llu\n",
	    (u_longlong_t)dd->dd_parent_obj);
	(void) printf("\t\tclone_parent_obj = %llu\n",
	    (u_longlong_t)dd->dd_clone_parent_obj);
	(void) printf("\t\tchild_dir_zapobj = %llu\n",
	    (u_longlong_t)dd->dd_child_dir_zapobj);
	(void) printf("\t\tused_bytes = %s\n", used);
	(void) printf("\t\tcompressed_bytes = %s\n", compressed);
	(void) printf("\t\tuncompressed_bytes = %s\n", uncompressed);
	(void) printf("\t\tquota = %s\n", quota);
	(void) printf("\t\treserved = %s\n", resv);
	(void) printf("\t\tprops_zapobj = %llu\n",
	    (u_longlong_t)dd->dd_props_zapobj);
}

/*ARGSUSED*/
static void
dump_dsl_dataset(objset_t *os, uint64_t object, void *data, size_t size)
{
	dsl_dataset_phys_t *ds = data;
	time_t crtime;
	char used[6], compressed[6], uncompressed[6], unique[6], blkbuf[300];

	if (ds == NULL)
		return;

	ASSERT(size == sizeof (*ds));
	crtime = ds->ds_creation_time;
	nicenum(ds->ds_used_bytes, used);
	nicenum(ds->ds_compressed_bytes, compressed);
	nicenum(ds->ds_uncompressed_bytes, uncompressed);
	nicenum(ds->ds_unique_bytes, unique);
	sprintf_blkptr(blkbuf, &ds->ds_bp);

	(void) printf("\t\tdataset_obj = %llu\n",
	    (u_longlong_t)ds->ds_dir_obj);
	(void) printf("\t\tprev_snap_obj = %llu\n",
	    (u_longlong_t)ds->ds_prev_snap_obj);
	(void) printf("\t\tprev_snap_txg = %llu\n",
	    (u_longlong_t)ds->ds_prev_snap_txg);
	(void) printf("\t\tnext_snap_obj = %llu\n",
	    (u_longlong_t)ds->ds_next_snap_obj);
	(void) printf("\t\tsnapnames_zapobj = %llu\n",
	    (u_longlong_t)ds->ds_snapnames_zapobj);
	(void) printf("\t\tnum_children = %llu\n",
	    (u_longlong_t)ds->ds_num_children);
	(void) printf("\t\tcreation_time = %s", ctime(&crtime));
	(void) printf("\t\tcreation_txg = %llu\n",
	    (u_longlong_t)ds->ds_creation_txg);
	(void) printf("\t\tdeadlist_obj = %llu\n",
	    (u_longlong_t)ds->ds_deadlist_obj);
	(void) printf("\t\tused_bytes = %s\n", used);
	(void) printf("\t\tcompressed_bytes = %s\n", compressed);
	(void) printf("\t\tuncompressed_bytes = %s\n", uncompressed);
	(void) printf("\t\tunique = %s\n", unique);
	(void) printf("\t\tfsid_guid = %llu\n",
	    (u_longlong_t)ds->ds_fsid_guid);
	(void) printf("\t\tguid = %llu\n",
	    (u_longlong_t)ds->ds_guid);
	(void) printf("\t\trestoring = %llu\n",
	    (u_longlong_t)ds->ds_restoring);
	(void) printf("\t\tbp = %s\n", blkbuf);
}

static void
dump_bplist(objset_t *mos, uint64_t object, char *name)
{
	bplist_t bpl = { 0 };
	blkptr_t blk, *bp = &blk;
	uint64_t itor = 0;
	char numbuf[6];

	if (dump_opt['d'] < 3)
		return;

	bplist_open(&bpl, mos, object);
	if (bplist_empty(&bpl)) {
		bplist_close(&bpl);
		return;
	}

	nicenum(bpl.bpl_phys->bpl_bytes, numbuf);

	(void) printf("\n    %s: %llu entries, %s\n",
	    name, (u_longlong_t)bpl.bpl_phys->bpl_entries, numbuf);

	if (dump_opt['d'] < 5) {
		bplist_close(&bpl);
		return;
	}

	(void) printf("\n");

	while (bplist_iterate(&bpl, &itor, bp) == 0) {
		ASSERT(bp->blk_birth != 0);
		// XXBP - Do we want to see all DVAs, or just one?
		(void) printf("\tItem %3llu: vdev=%llu off=%llx "
		    "%llxL/%llxP/%llxA F=%llu B=%llu\n",
		    (u_longlong_t)itor - 1,
		    (u_longlong_t)DVA_GET_VDEV(&bp->blk_dva[0]),
		    (u_longlong_t)DVA_GET_OFFSET(&bp->blk_dva[0]),
		    (u_longlong_t)BP_GET_LSIZE(bp),
		    (u_longlong_t)BP_GET_PSIZE(bp),
		    (u_longlong_t)DVA_GET_ASIZE(&bp->blk_dva[0]),
		    (u_longlong_t)bp->blk_fill,
		    (u_longlong_t)bp->blk_birth);
	}

	bplist_close(&bpl);
}

static char *
znode_path(objset_t *os, uint64_t object, char *pathbuf, size_t size)
{
	dmu_buf_t *db;
	dmu_object_info_t doi;
	znode_phys_t *zp;
	uint64_t parent = 0;
	size_t complen;
	char component[MAXNAMELEN + 1];
	char *path;

	path = pathbuf + size;
	*--path = '\0';

	for (;;) {
		db = dmu_bonus_hold(os, object);
		if (db == NULL)
			break;

		dmu_buf_read(db);
		dmu_object_info_from_db(db, &doi);
		zp = db->db_data;
		parent = zp->zp_parent;
		dmu_buf_rele(db);

		if (doi.doi_bonus_type != DMU_OT_ZNODE)
			break;

		if (parent == object) {
			if (path[0] != '/')
				*--path = '/';
			return (path);
		}

		if (zap_value_search(os, parent, object, component) != 0)
			break;

		complen = strlen(component);
		path -= complen;
		bcopy(component, path, complen);
		*--path = '/';

		object = parent;
	}

	(void) sprintf(component, "???<object#%llu>", (u_longlong_t)object);

	complen = strlen(component);
	path -= complen;
	bcopy(component, path, complen);

	return (path);
}

/*ARGSUSED*/
static void
dump_znode(objset_t *os, uint64_t object, void *data, size_t size)
{
	znode_phys_t *zp = data;
	time_t z_crtime, z_atime, z_mtime, z_ctime;
	char path[MAXPATHLEN * 2];	/* allow for xattr and failure prefix */

	ASSERT(size >= sizeof (znode_phys_t));

	if (dump_opt['d'] < 3) {
		(void) printf("\t%s\n",
		    znode_path(os, object, path, sizeof (path)));
		return;
	}

	z_crtime = (time_t)zp->zp_crtime[0];
	z_atime = (time_t)zp->zp_atime[0];
	z_mtime = (time_t)zp->zp_mtime[0];
	z_ctime = (time_t)zp->zp_ctime[0];

	(void) printf("\tpath	%s\n",
	    znode_path(os, object, path, sizeof (path)));
	(void) printf("\tatime	%s", ctime(&z_atime));
	(void) printf("\tmtime	%s", ctime(&z_mtime));
	(void) printf("\tctime	%s", ctime(&z_ctime));
	(void) printf("\tcrtime	%s", ctime(&z_crtime));
	(void) printf("\tgen	%llu\n", (u_longlong_t)zp->zp_gen);
	(void) printf("\tmode	%llo\n", (u_longlong_t)zp->zp_mode);
	(void) printf("\tsize	%llu\n", (u_longlong_t)zp->zp_size);
	(void) printf("\tparent	%llu\n", (u_longlong_t)zp->zp_parent);
	(void) printf("\tlinks	%llu\n", (u_longlong_t)zp->zp_links);
	(void) printf("\txattr	%llu\n", (u_longlong_t)zp->zp_xattr);
	(void) printf("\trdev	0x%016llx\n", (u_longlong_t)zp->zp_rdev);
}

/*ARGSUSED*/
static void
dump_acl(objset_t *os, uint64_t object, void *data, size_t size)
{
}

/*ARGSUSED*/
static void
dump_dmu_objset(objset_t *os, uint64_t object, void *data, size_t size)
{
}

static object_viewer_t *object_viewer[DMU_OT_NUMTYPES] = {
	dump_none,		/* unallocated			*/
	dump_zap,		/* object directory		*/
	dump_uint64,		/* object array			*/
	dump_none,		/* packed nvlist		*/
	dump_packed_nvlist,	/* packed nvlist size		*/
	dump_none,		/* bplist			*/
	dump_none,		/* bplist header		*/
	dump_none,		/* SPA space map header		*/
	dump_none,		/* SPA space map		*/
	dump_none,		/* ZIL intent log		*/
	dump_dnode,		/* DMU dnode			*/
	dump_dmu_objset,	/* DMU objset			*/
	dump_dsl_dir,	/* DSL directory			*/
	dump_zap,		/* DSL directory child map	*/
	dump_zap,		/* DSL dataset snap map		*/
	dump_zap,		/* DSL props			*/
	dump_dsl_dataset,	/* DSL dataset			*/
	dump_znode,		/* ZFS znode			*/
	dump_acl,		/* ZFS ACL			*/
	dump_uint8,		/* ZFS plain file		*/
	dump_zap,		/* ZFS directory		*/
	dump_zap,		/* ZFS master node		*/
	dump_zap,		/* ZFS delete queue		*/
	dump_uint8,		/* zvol object			*/
	dump_zap,		/* zvol prop			*/
	dump_uint8,		/* other uint8[]		*/
	dump_uint64,		/* other uint64[]		*/
	dump_zap,		/* other ZAP			*/
};

static void
dump_object(objset_t *os, uint64_t object, int verbosity, int *print_header)
{
	dmu_buf_t *db = NULL;
	dmu_object_info_t doi;
	dnode_t *dn;
	void *bonus = NULL;
	size_t bsize = 0;
	char iblk[6], dblk[6], lsize[6], psize[6], bonus_size[6], segsize[6];
	char aux[50];
	int error;

	if (*print_header) {
		(void) printf("\n    Object  lvl   iblk   dblk  lsize"
		    "  psize  type\n");
		*print_header = 0;
	}

	if (object == 0) {
		dn = os->os->os_meta_dnode;
	} else {
		db = dmu_bonus_hold(os, object);
		if (db == NULL)
			fatal("dmu_bonus_hold(%llu) failed", object);
		dmu_buf_read(db);
		bonus = db->db_data;
		bsize = db->db_size;
		dn = ((dmu_buf_impl_t *)db)->db_dnode;
	}
	dmu_object_info_from_dnode(dn, &doi);

	nicenum(doi.doi_metadata_block_size, iblk);
	nicenum(doi.doi_data_block_size, dblk);
	nicenum(doi.doi_data_block_size * (doi.doi_max_block_offset + 1),
	    lsize);
	nicenum(doi.doi_physical_blks << 9, psize);
	nicenum(doi.doi_bonus_size, bonus_size);

	aux[0] = '\0';

	if (doi.doi_checksum != ZIO_CHECKSUM_INHERIT || verbosity >= 6)
		(void) snprintf(aux + strlen(aux), sizeof (aux), " (K=%s)",
		zio_checksum_table[doi.doi_checksum].ci_name);

	if (doi.doi_compress != ZIO_COMPRESS_INHERIT || verbosity >= 6)
		(void) snprintf(aux + strlen(aux), sizeof (aux), " (Z=%s)",
		zio_compress_table[doi.doi_compress].ci_name);

	(void) printf("%10lld  %3u  %5s  %5s  %5s  %5s  %s%s\n",
	    (u_longlong_t)object, doi.doi_indirection, iblk, dblk, lsize,
	    psize, dmu_ot[doi.doi_type].ot_name, aux);

	if (doi.doi_bonus_type != DMU_OT_NONE && verbosity > 3) {
		(void) printf("%10s  %3s  %5s  %5s  %5s  %5s  %s\n",
		    "", "", "", "", bonus_size, "bonus",
		    dmu_ot[doi.doi_bonus_type].ot_name);
	}

	if (verbosity >= 4) {
		object_viewer[doi.doi_bonus_type](os, object, bonus, bsize);
		object_viewer[doi.doi_type](os, object, NULL, 0);
		*print_header = 1;
	}

	if (verbosity >= 5)
		dump_indirect(os, object, NULL, 0);

	if (verbosity >= 5) {
		/*
		 * Report the list of segments that comprise the object.
		 */
		uint64_t start = 0;
		uint64_t end;
		uint64_t blkfill = 1;
		int minlvl = 1;

		if (dn->dn_type == DMU_OT_DNODE) {
			minlvl = 0;
			blkfill = DNODES_PER_BLOCK;
		}

		for (;;) {
			error = dnode_next_offset(dn, B_FALSE, &start, minlvl,
			    blkfill);
			if (error)
				break;
			end = start;
			error = dnode_next_offset(dn, B_TRUE, &end, minlvl,
			    blkfill);
			nicenum(end - start, segsize);
			(void) printf("\t\tsegment [%016llx, %016llx)"
			    " size %5s\n", (u_longlong_t)start,
			    (u_longlong_t)end, segsize);
			if (error)
				break;
			start = end;
		}
	}

	if (db != NULL)
		dmu_buf_rele(db);
}

static char *objset_types[DMU_OST_NUMTYPES] = {
	"NONE", "META", "ZPL", "ZVOL", "OTHER", "ANY" };

/*ARGSUSED*/
static void
dump_dir(objset_t *os)
{
	dmu_objset_stats_t dds;
	uint64_t object, object_count;
	char numbuf[8];
	char blkbuf[300];
	char osname[MAXNAMELEN];
	char *type = "UNKNOWN";
	int verbosity = dump_opt['d'];
	int print_header = 1;
	int i, error;

	dmu_objset_stats(os, &dds);

	if (dds.dds_type < DMU_OST_NUMTYPES)
		type = objset_types[dds.dds_type];

	if (dds.dds_type == DMU_OST_META) {
		dds.dds_creation_txg = TXG_INITIAL;
		dds.dds_last_txg = os->os->os_rootbp.blk_birth;
		dds.dds_objects_used = os->os->os_rootbp.blk_fill;
		dds.dds_space_refd =
		    os->os->os_spa->spa_dsl_pool->dp_mos_dir->dd_used_bytes;
	}

	ASSERT3U(dds.dds_objects_used, ==, os->os->os_rootbp.blk_fill);

	nicenum(dds.dds_space_refd, numbuf);

	if (verbosity >= 4) {
		(void) strcpy(blkbuf, ", rootbp ");
		sprintf_blkptr(blkbuf + strlen(blkbuf), &os->os->os_rootbp);
	} else {
		blkbuf[0] = '\0';
	}

	dmu_objset_name(os, osname);

	(void) printf("Dataset %s [%s], ID %llu, cr_txg %llu, last_txg %llu, "
	    "%s, %llu objects%s\n",
	    osname, type, (u_longlong_t)dmu_objset_id(os),
	    (u_longlong_t)dds.dds_creation_txg,
	    (u_longlong_t)dds.dds_last_txg,
	    numbuf,
	    (u_longlong_t)dds.dds_objects_used,
	    blkbuf);

	dump_intent_log(dmu_objset_zil(os));

	if (dmu_objset_ds(os) != NULL)
		dump_bplist(dmu_objset_pool(os)->dp_meta_objset,
		    dmu_objset_ds(os)->ds_phys->ds_deadlist_obj, "Deadlist");

	if (verbosity < 2)
		return;

	if (zopt_objects != 0) {
		for (i = 0; i < zopt_objects; i++)
			dump_object(os, zopt_object[i], verbosity,
			    &print_header);
		(void) printf("\n");
		return;
	}

	dump_object(os, 0, verbosity, &print_header);
	object_count = 1;

	object = 0;
	while ((error = dmu_object_next(os, &object, B_FALSE)) == 0) {
		dump_object(os, object, verbosity, &print_header);
		object_count++;
	}

	ASSERT3U(object_count, ==, dds.dds_objects_used);

	(void) printf("\n");

	if (error != ESRCH)
		fatal("dmu_object_next() = %d", error);
}

static void
dump_uberblock(uberblock_t *ub)
{
	time_t timestamp = ub->ub_timestamp;

	(void) printf("Uberblock\n\n");
	(void) printf("\tmagic = %016llx\n", (u_longlong_t)ub->ub_magic);
	(void) printf("\tversion = %llu\n", (u_longlong_t)ub->ub_version);
	(void) printf("\ttxg = %llu\n", (u_longlong_t)ub->ub_txg);
	(void) printf("\tguid_sum = %llu\n", (u_longlong_t)ub->ub_guid_sum);
	(void) printf("\ttimestamp = %llu UTC = %s",
	    (u_longlong_t)ub->ub_timestamp, asctime(localtime(&timestamp)));
	if (dump_opt['u'] >= 3) {
		char blkbuf[300];
		sprintf_blkptr(blkbuf, &ub->ub_rootbp);
		(void) printf("\trootbp = %s\n", blkbuf);
	}
	(void) printf("\n");
}

static void
dump_config(const char *pool)
{
	spa_t *spa = NULL;

	mutex_enter(&spa_namespace_lock);
	while ((spa = spa_next(spa)) != NULL) {
		if (pool == NULL)
			(void) printf("%s\n", spa_name(spa));
		if (pool == NULL || strcmp(pool, spa_name(spa)) == 0)
			dump_nvlist(spa->spa_config, 4);
	}
	mutex_exit(&spa_namespace_lock);
}

static void
dump_label(const char *dev)
{
	int fd;
	vdev_label_t label;
	char *buf = label.vl_vdev_phys.vp_nvlist;
	size_t buflen = sizeof (label.vl_vdev_phys.vp_nvlist);
	struct stat64 statbuf;
	uint64_t psize;
	int l;

	if ((fd = open(dev, O_RDONLY)) < 0) {
		(void) printf("cannot open '%s': %s\n", dev, strerror(errno));
		exit(1);
	}

	if (fstat64(fd, &statbuf) != 0) {
		(void) printf("failed to stat '%s': %s\n", dev,
		    strerror(errno));
		exit(1);
	}

	psize = statbuf.st_size;
	psize = P2ALIGN(psize, (uint64_t)sizeof (vdev_label_t));

	for (l = 0; l < VDEV_LABELS; l++) {

		nvlist_t *config = NULL;

		(void) printf("--------------------------------------------\n");
		(void) printf("LABEL %d\n", l);
		(void) printf("--------------------------------------------\n");

		if (pread(fd, &label, sizeof (label),
		    vdev_label_offset(psize, l, 0)) != sizeof (label)) {
			(void) printf("failed to read label %d\n", l);
			continue;
		}

		if (nvlist_unpack(buf, buflen, &config, 0) != 0) {
			(void) printf("failed to unpack label %d\n", l);
			continue;
		}
		dump_nvlist(config, 4);
		nvlist_free(config);
	}
}

/*ARGSUSED*/
static void
dump_one_dir(char *dsname, void *arg)
{
	int error;
	objset_t *os;

	error = dmu_objset_open(dsname, DMU_OST_ANY,
	    DS_MODE_STANDARD | DS_MODE_READONLY, &os);
	if (error) {
		(void) printf("Could not open %s\n", dsname);
		return;
	}
	dump_dir(os);
	dmu_objset_close(os);
}

static void
zdb_space_map_load(spa_t *spa)
{
	vdev_t *rvd = spa->spa_root_vdev;
	vdev_t *vd;
	int c, m, error;

	for (c = 0; c < rvd->vdev_children; c++) {
		vd = rvd->vdev_child[c];
		for (m = 0; m < vd->vdev_ms_count; m++) {
			metaslab_t *msp = vd->vdev_ms[m];
			space_map_t *sm = &msp->ms_allocmap[0];
			mutex_enter(&msp->ms_lock);
			error = space_map_load(sm, msp->ms_smo, SM_ALLOC,
			    spa->spa_meta_objset, msp->ms_usable_end,
			    sm->sm_size - msp->ms_usable_space);
			mutex_exit(&msp->ms_lock);
			if (error)
				fatal("%s bad space map #%d, error %d",
				    spa->spa_name, c, error);
		}
	}
}

static int
zdb_space_map_claim(spa_t *spa, blkptr_t *bp)
{
	dva_t *dva = &bp->blk_dva[0];
	uint64_t vdev = DVA_GET_VDEV(dva);
	uint64_t offset = DVA_GET_OFFSET(dva);
	uint64_t size = DVA_GET_ASIZE(dva);
	vdev_t *vd;
	metaslab_t *msp;
	space_map_t *allocmap, *freemap;
	int error;

	if ((vd = vdev_lookup_top(spa, vdev)) == NULL)
		return (ENXIO);

	if ((offset >> vd->vdev_ms_shift) >= vd->vdev_ms_count)
		return (ENXIO);

	if (DVA_GET_GANG(dva)) {
		zio_gbh_phys_t gbh;
		blkptr_t blk = *bp;
		int g;

		/* LINTED - compile time assert */
		ASSERT(sizeof (zio_gbh_phys_t) == SPA_GANGBLOCKSIZE);
		size = vdev_psize_to_asize(vd, SPA_GANGBLOCKSIZE);
		DVA_SET_GANG(&blk.blk_dva[0], 0);
		DVA_SET_ASIZE(&blk.blk_dva[0], size);
		BP_SET_CHECKSUM(&blk, ZIO_CHECKSUM_GANG_HEADER);
		BP_SET_PSIZE(&blk, SPA_GANGBLOCKSIZE);
		BP_SET_LSIZE(&blk, SPA_GANGBLOCKSIZE);
		BP_SET_COMPRESS(&blk, ZIO_COMPRESS_OFF);
		error = zio_wait(zio_read(NULL, spa, &blk,
		    &gbh, SPA_GANGBLOCKSIZE, NULL, NULL,
		    ZIO_PRIORITY_SYNC_READ,
		    ZIO_FLAG_CANFAIL | ZIO_FLAG_CONFIG_HELD));
		if (error)
			return (error);
		if (BP_SHOULD_BYTESWAP(&blk))
			byteswap_uint64_array(&gbh, SPA_GANGBLOCKSIZE);
		for (g = 0; g < SPA_GBH_NBLKPTRS; g++) {
			if (gbh.zg_blkptr[g].blk_birth == 0)
				break;
			error = zdb_space_map_claim(spa, &gbh.zg_blkptr[g]);
			if (error)
				return (error);
		}
	}

	msp = vd->vdev_ms[offset >> vd->vdev_ms_shift];
	allocmap = &msp->ms_allocmap[0];
	freemap = &msp->ms_freemap[0];

	mutex_enter(&msp->ms_lock);
	if (space_map_contains(freemap, offset, size)) {
		mutex_exit(&msp->ms_lock);
		return (EAGAIN);	/* allocated more than once */
	}

	if (!space_map_contains(allocmap, offset, size)) {
		mutex_exit(&msp->ms_lock);
		return (ESTALE);	/* not allocated at all */
	}

	space_map_remove(allocmap, offset, size);
	space_map_add(freemap, offset, size);

	mutex_exit(&msp->ms_lock);

	return (0);
}

static void
zdb_leak(space_map_t *sm, uint64_t start, uint64_t size)
{
	metaslab_t *msp;

	/* LINTED */
	msp = (metaslab_t *)((char *)sm - offsetof(metaslab_t, ms_allocmap[0]));

	(void) printf("leaked space: vdev %llu, offset 0x%llx, size %llu\n",
	    (u_longlong_t)msp->ms_group->mg_vd->vdev_id,
	    (u_longlong_t)start,
	    (u_longlong_t)size);
}

static void
zdb_space_map_vacate(spa_t *spa)
{
	vdev_t *rvd = spa->spa_root_vdev;
	vdev_t *vd;
	int c, m;

	for (c = 0; c < rvd->vdev_children; c++) {
		vd = rvd->vdev_child[c];
		for (m = 0; m < vd->vdev_ms_count; m++) {
			metaslab_t *msp = vd->vdev_ms[m];
			mutex_enter(&msp->ms_lock);
			space_map_vacate(&msp->ms_allocmap[0], zdb_leak,
			    &msp->ms_allocmap[0]);
			space_map_vacate(&msp->ms_freemap[0], NULL, NULL);
			mutex_exit(&msp->ms_lock);
		}
	}
}

static void
zdb_refresh_ubsync(spa_t *spa)
{
	uberblock_t ub = { 0 };
	vdev_t *rvd = spa->spa_root_vdev;
	zio_t *zio;

	/*
	 * Reopen all devices to purge zdb's vdev caches.
	 */
	vdev_reopen(rvd, NULL);

	/*
	 * Reload the uberblock.
	 */
	zio = zio_root(spa, NULL, NULL,
	    ZIO_FLAG_CANFAIL | ZIO_FLAG_SPECULATIVE);
	vdev_uberblock_load(zio, rvd, &ub);
	(void) zio_wait(zio);

	if (ub.ub_txg != 0)
		spa->spa_ubsync = ub;
}

/*
 * Verify that the sum of the sizes of all blocks in the pool adds up
 * to the SPA's sa_alloc total.
 */
typedef struct zdb_blkstats {
	uint64_t	zb_asize;
	uint64_t	zb_lsize;
	uint64_t	zb_psize;
	uint64_t	zb_count;
} zdb_blkstats_t;

#define	DMU_OT_DEFERRED	DMU_OT_NONE
#define	DMU_OT_TOTAL	DMU_OT_NUMTYPES

#define	ZB_TOTAL	ZB_MAXLEVEL

typedef struct zdb_cb {
	zdb_blkstats_t	zcb_type[ZB_TOTAL + 1][DMU_OT_TOTAL + 1];
	uint64_t	zcb_errors[256];
	traverse_blk_cache_t *zcb_cache;
	int		zcb_readfails;
	int		zcb_haderrors;
} zdb_cb_t;

static blkptr_cb_t zdb_blkptr_cb;

static void
zdb_count_block(spa_t *spa, zdb_cb_t *zcb, blkptr_t *bp, int type)
{
	int i, error;

	for (i = 0; i < 4; i++) {
		int l = (i < 2) ? BP_GET_LEVEL(bp) : ZB_TOTAL;
		int t = (i & 1) ? type : DMU_OT_TOTAL;
		zdb_blkstats_t *zb = &zcb->zcb_type[l][t];

		zb->zb_asize += BP_GET_ASIZE(bp);
		zb->zb_lsize += BP_GET_LSIZE(bp);
		zb->zb_psize += BP_GET_PSIZE(bp);
		zb->zb_count++;
	}

	if (dump_opt['L'])
		return;

	error = zdb_space_map_claim(spa, bp);

	if (error == 0)
		return;

	if (error == EAGAIN)
		(void) fatal("double-allocation, bp=%p", bp);

	if (error == ESTALE)
		(void) fatal("reference to freed block, bp=%p", bp);

	(void) fatal("fatal error %d in bp %p", error, bp);
}

static void
zdb_log_block_cb(zilog_t *zilog, blkptr_t *bp, void *arg, uint64_t first_txg)
{
	if (bp->blk_birth < first_txg) {
		zdb_cb_t *zcb = arg;
		traverse_blk_cache_t bc = *zcb->zcb_cache;
		zbookmark_t *zb = &bc.bc_bookmark;

		zb->zb_objset = bp->blk_cksum.zc_word[2];
		zb->zb_blkid = bp->blk_cksum.zc_word[3];
		bc.bc_blkptr = *bp;

		(void) zdb_blkptr_cb(&bc, zilog->zl_spa, arg);
	}
}

static int
zdb_blkptr_cb(traverse_blk_cache_t *bc, spa_t *spa, void *arg)
{
	zbookmark_t *zb = &bc->bc_bookmark;
	zdb_cb_t *zcb = arg;
	blkptr_t *bp = &bc->bc_blkptr;
	dmu_object_type_t type = BP_GET_TYPE(bp);
	char blkbuf[300];
	int error = 0;

	if (bc->bc_errno) {
		if (zcb->zcb_readfails++ < 10 && dump_opt['L']) {
			zdb_refresh_ubsync(spa);
			error = EAGAIN;
		} else {
			zcb->zcb_haderrors = 1;
			zcb->zcb_errors[bc->bc_errno]++;
			error = ERESTART;
		}

		if (dump_opt['b'] >= 3 || (dump_opt['b'] >= 2 && bc->bc_errno))
			sprintf_blkptr(blkbuf, bp);
		else
			blkbuf[0] = '\0';

		(void) printf("zdb_blkptr_cb: Got error %d reading "
		    "<%llu, %llu, %d, %llx> %s -- %s\n",
		    bc->bc_errno,
		    (u_longlong_t)zb->zb_objset,
		    (u_longlong_t)zb->zb_object,
		    zb->zb_level,
		    (u_longlong_t)zb->zb_blkid,
		    blkbuf,
		    error == EAGAIN ? "retrying" : "skipping");

		return (error);
	}

	zcb->zcb_readfails = 0;

	ASSERT(bp->blk_birth != 0);

	zdb_count_block(spa, zcb, bp, type);

	if (dump_opt['b'] >= 4) {
		sprintf_blkptr(blkbuf, bp);
		(void) printf("objset %llu object %llu offset 0x%llx %s\n",
		    (u_longlong_t)zb->zb_objset,
		    (u_longlong_t)zb->zb_object,
		    (u_longlong_t)blkid2offset(bc->bc_dnode,
			zb->zb_level, zb->zb_blkid),
		    blkbuf);
	}

	if (type == DMU_OT_OBJSET) {
		objset_phys_t *osphys = bc->bc_data;
		zilog_t zilog = { 0 };
		zilog.zl_header = &osphys->os_zil_header;
		zilog.zl_spa = spa;

		zcb->zcb_cache = bc;

		zil_parse(&zilog, zdb_log_block_cb, NULL, zcb,
		    spa_first_txg(spa));
	}

	return (0);
}

static int
dump_block_stats(spa_t *spa)
{
	traverse_handle_t *th;
	zdb_cb_t zcb = { 0 };
	zdb_blkstats_t *zb, *tzb;
	uint64_t alloc, space;
	int leaks = 0;
	int advance = zdb_advance;
	int flags;
	int e;

	if (dump_opt['c'])
		advance |= ADVANCE_DATA;

	advance |= ADVANCE_PRUNE;

	(void) printf("\nTraversing all blocks to %sverify"
	    " nothing leaked ...\n",
	    dump_opt['c'] ? "verify checksums and " : "");

	/*
	 * Load all space maps.  As we traverse the pool, if we find a block
	 * that's not in its space map, that indicates a double-allocation,
	 * reference to a freed block, or an unclaimed block.  Otherwise we
	 * remove the block from the space map.  If the space maps are not
	 * empty when we're done, that indicates leaked blocks.
	 */
	if (!dump_opt['L'])
		zdb_space_map_load(spa);

	/*
	 * If there's a deferred-free bplist, process that first.
	 */
	if (spa->spa_sync_bplist_obj != 0) {
		bplist_t *bpl = &spa->spa_sync_bplist;
		blkptr_t blk;
		uint64_t itor = 0;

		bplist_open(bpl, spa->spa_meta_objset,
		    spa->spa_sync_bplist_obj);

		while (bplist_iterate(bpl, &itor, &blk) == 0) {
			zdb_count_block(spa, &zcb, &blk, DMU_OT_DEFERRED);
			if (dump_opt['b'] >= 4) {
				char blkbuf[300];
				sprintf_blkptr(blkbuf, &blk);
				(void) printf("[%s] %s\n",
				    "deferred free", blkbuf);
			}
		}

		bplist_close(bpl);
	}

	/*
	 * Now traverse the pool.  If we're read all data to verify checksums,
	 * do a scrubbing read so that we validate all copies.
	 */
	flags = ZIO_FLAG_CANFAIL;
	if (advance & ADVANCE_DATA)
		flags |= ZIO_FLAG_SCRUB;
	th = traverse_init(spa, zdb_blkptr_cb, &zcb, advance, flags);
	th->th_noread = zdb_noread;

	traverse_add_pool(th, 0, -1ULL);

	while (traverse_more(th) == EAGAIN)
		continue;

	traverse_fini(th);

	if (zcb.zcb_haderrors) {
		(void) printf("\nError counts:\n\n");
		(void) printf("\t%5s  %s\n", "errno", "count");
		for (e = 0; e < 256; e++) {
			if (zcb.zcb_errors[e] != 0) {
				(void) printf("\t%5d  %llu\n",
				    e, (u_longlong_t)zcb.zcb_errors[e]);
			}
		}
	}

	/*
	 * Report any leaked segments.
	 */
	if (!dump_opt['L'])
		zdb_space_map_vacate(spa);

	if (dump_opt['L'])
		(void) printf("\n\n *** Live pool traversal; "
		    "block counts are only approximate ***\n\n");

	alloc = spa_get_alloc(spa);
	space = spa_get_space(spa);

	tzb = &zcb.zcb_type[ZB_TOTAL][DMU_OT_TOTAL];

	if (tzb->zb_asize == alloc) {
		(void) printf("\n\tNo leaks (block sum matches space"
		    " maps exactly)\n");
	} else {
		(void) printf("block traversal size %llu != alloc %llu "
		    "(leaked %lld)\n",
		    (u_longlong_t)tzb->zb_asize,
		    (u_longlong_t)alloc,
		    (u_longlong_t)(alloc - tzb->zb_asize));
		leaks = 1;
	}

	if (tzb->zb_count == 0)
		return (2);

	(void) printf("\n");
	(void) printf("\tbp count:      %10llu\n",
	    (u_longlong_t)tzb->zb_count);
	(void) printf("\tbp logical:    %10llu\t avg: %6llu\n",
	    (u_longlong_t)tzb->zb_lsize,
	    (u_longlong_t)(tzb->zb_lsize / tzb->zb_count));
	(void) printf("\tbp physical:   %10llu\t avg:"
	    " %6llu\tcompression: %6.2f\n",
	    (u_longlong_t)tzb->zb_psize,
	    (u_longlong_t)(tzb->zb_psize / tzb->zb_count),
	    (double)tzb->zb_lsize / tzb->zb_psize);
	(void) printf("\tbp allocated:  %10llu\t avg:"
	    " %6llu\tcompression: %6.2f\n",
	    (u_longlong_t)tzb->zb_asize,
	    (u_longlong_t)(tzb->zb_asize / tzb->zb_count),
	    (double)tzb->zb_lsize / tzb->zb_asize);
	(void) printf("\tSPA allocated: %10llu\tused: %5.2f%%\n",
	    (u_longlong_t)alloc, 100.0 * alloc / space);

	if (dump_opt['b'] >= 2) {
		int l, t, level;
		(void) printf("\nBlocks\tLSIZE\tPSIZE\tASIZE"
		    "\t  avg\t comp\t%%Total\tType\n");

		for (t = 0; t <= DMU_OT_NUMTYPES; t++) {
			char csize[6], lsize[6], psize[6], asize[6], avg[6];
			char *typename;

			typename = t == DMU_OT_DEFERRED ? "deferred free" :
			    t == DMU_OT_TOTAL ? "Total" : dmu_ot[t].ot_name;

			if (zcb.zcb_type[ZB_TOTAL][t].zb_asize == 0) {
				(void) printf("%6s\t%5s\t%5s\t%5s"
				    "\t%5s\t%5s\t%6s\t%s\n",
				    "-",
				    "-",
				    "-",
				    "-",
				    "-",
				    "-",
				    "-",
				    typename);
				continue;
			}

			for (l = ZB_TOTAL - 1; l >= -1; l--) {
				level = (l == -1 ? ZB_TOTAL : l);
				zb = &zcb.zcb_type[level][t];

				if (zb->zb_asize == 0)
					continue;

				if (dump_opt['b'] < 3 && level != ZB_TOTAL)
					continue;

				if (level == 0 && zb->zb_asize ==
				    zcb.zcb_type[ZB_TOTAL][t].zb_asize)
					continue;

				nicenum(zb->zb_count, csize);
				nicenum(zb->zb_lsize, lsize);
				nicenum(zb->zb_psize, psize);
				nicenum(zb->zb_asize, asize);
				nicenum(zb->zb_asize / zb->zb_count, avg);

				(void) printf("%6s\t%5s\t%5s\t%5s\t%5s"
				    "\t%5.2f\t%6.2f\t",
				    csize, lsize, psize, asize, avg,
				    (double)zb->zb_lsize / zb->zb_psize,
				    100.0 * zb->zb_asize / tzb->zb_asize);

				if (level == ZB_TOTAL)
					(void) printf("%s\n", typename);
				else
					(void) printf("    L%d %s\n",
					    level, typename);
			}
		}
	}

	(void) printf("\n");

	if (leaks)
		return (2);

	if (zcb.zcb_haderrors)
		return (3);

	return (0);
}

static void
dump_zpool(spa_t *spa)
{
	dsl_pool_t *dp = spa_get_dsl(spa);
	int rc = 0;

	if (dump_opt['u'])
		dump_uberblock(&spa->spa_uberblock);

	if (dump_opt['d'] || dump_opt['i']) {
		dump_dir(dp->dp_meta_objset);
		if (dump_opt['d'] >= 3) {
			dump_bplist(dp->dp_meta_objset,
			    spa->spa_sync_bplist_obj, "Deferred frees");
			dump_dtl(spa->spa_root_vdev, 0);
			dump_metaslabs(spa);
		}
		dmu_objset_find(spa->spa_name, dump_one_dir, NULL,
		    DS_FIND_SNAPSHOTS);
	}

	if (dump_opt['b'] || dump_opt['c'])
		rc = dump_block_stats(spa);

	if (dump_opt['s'])
		show_pool_stats(spa);

	if (rc != 0)
		exit(rc);
}

int
main(int argc, char **argv)
{
	int i, c;
	struct rlimit rl = { 1024, 1024 };
	spa_t *spa;
	objset_t *os = NULL;
	char *endstr;
	int dump_all = 1;
	int verbose = 0;
	int error;
	int flag, set;

	(void) setrlimit(RLIMIT_NOFILE, &rl);

	dprintf_setup(&argc, argv);

	while ((c = getopt(argc, argv, "udibcsvCLO:B:Ul")) != -1) {
		switch (c) {
		case 'u':
		case 'd':
		case 'i':
		case 'b':
		case 'c':
		case 's':
		case 'C':
		case 'l':
			dump_opt[c]++;
			dump_all = 0;
			break;
		case 'L':
			dump_opt[c]++;
			break;
		case 'O':
			endstr = optarg;
			if (endstr[0] == '!') {
				endstr++;
				set = 0;
			} else {
				set = 1;
			}
			if (strcmp(endstr, "post") == 0) {
				flag = ADVANCE_PRE;
				set = !set;
			} else if (strcmp(endstr, "pre") == 0) {
				flag = ADVANCE_PRE;
			} else if (strcmp(endstr, "prune") == 0) {
				flag = ADVANCE_PRUNE;
			} else if (strcmp(endstr, "data") == 0) {
				flag = ADVANCE_DATA;
			} else if (strcmp(endstr, "holes") == 0) {
				flag = ADVANCE_HOLES;
			} else {
				usage();
			}
			if (set)
				zdb_advance |= flag;
			else
				zdb_advance &= ~flag;
			break;
		case 'B':
			endstr = optarg - 1;
			zdb_noread.zb_objset = strtoull(endstr + 1, &endstr, 0);
			zdb_noread.zb_object = strtoull(endstr + 1, &endstr, 0);
			zdb_noread.zb_level = strtol(endstr + 1, &endstr, 0);
			zdb_noread.zb_blkid = strtoull(endstr + 1, &endstr, 16);
			(void) printf("simulating bad block "
			    "<%llu, %llu, %d, %llx>\n",
			    (u_longlong_t)zdb_noread.zb_objset,
			    (u_longlong_t)zdb_noread.zb_object,
			    zdb_noread.zb_level,
			    (u_longlong_t)zdb_noread.zb_blkid);
			break;
		case 'v':
			verbose++;
			break;
		case 'U':
			spa_config_dir = "/tmp";
			break;
		default:
			usage();
			break;
		}
	}

	kernel_init(FREAD);

	for (c = 0; c < 256; c++) {
		if (dump_all && c != 'L' && c != 'l')
			dump_opt[c] = 1;
		if (dump_opt[c])
			dump_opt[c] += verbose;
	}

	argc -= optind;
	argv += optind;

	if (argc < 1) {
		if (dump_opt['C']) {
			dump_config(NULL);
			return (0);
		}
		usage();
	}

	if (dump_opt['l']) {
		dump_label(argv[0]);
		return (0);
	}

	if (dump_opt['C'])
		dump_config(argv[0]);

	if (strchr(argv[0], '/') != NULL) {
		error = dmu_objset_open(argv[0], DMU_OST_ANY,
		    DS_MODE_STANDARD | DS_MODE_READONLY, &os);
	} else {
		error = spa_open(argv[0], &spa, FTAG);
	}

	if (error)
		fatal("can't open %s: error %d", argv[0], error);

	argv++;
	if (--argc > 0) {
		zopt_objects = argc;
		zopt_object = calloc(zopt_objects, sizeof (uint64_t));
		for (i = 0; i < zopt_objects; i++) {
			errno = 0;
			zopt_object[i] = strtoull(argv[i], NULL, 0);
			if (zopt_object[i] == 0 && errno != 0)
				fatal("bad object number %s: %s",
				    argv[i], strerror(errno));
		}
	}

	if (os != NULL) {
		dump_dir(os);
		dmu_objset_close(os);
	} else {
		dump_zpool(spa);
		spa_close(spa, FTAG);
	}

	kernel_fini();

	return (0);
}