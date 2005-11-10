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

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/txg.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/zio_impl.h>
#include <sys/zio_compress.h>
#include <sys/zio_checksum.h>

static void zio_vdev_io_enter(zio_t *zio);
static void zio_vdev_io_exit(zio_t *zio);

/*
 * ==========================================================================
 * I/O priority table
 * ==========================================================================
 */
uint8_t zio_priority_table[ZIO_PRIORITY_TABLE_SIZE] = {
	0,	/* ZIO_PRIORITY_NOW		*/
	0,	/* ZIO_PRIORITY_SYNC_READ	*/
	0,	/* ZIO_PRIORITY_SYNC_WRITE	*/
	6,	/* ZIO_PRIORITY_ASYNC_READ	*/
	4,	/* ZIO_PRIORITY_ASYNC_WRITE	*/
	4,	/* ZIO_PRIORITY_FREE		*/
	0,	/* ZIO_PRIORITY_CACHE_FILL	*/
	0,	/* ZIO_PRIORITY_LOG_WRITE	*/
	10,	/* ZIO_PRIORITY_RESILVER	*/
	20,	/* ZIO_PRIORITY_SCRUB		*/
};

/*
 * ==========================================================================
 * I/O type descriptions
 * ==========================================================================
 */
char *zio_type_name[ZIO_TYPES] = {
	"null", "read", "write", "free", "claim", "ioctl" };

/* At or above this size, force gang blocking - for testing */
uint64_t zio_gang_bang = SPA_MAXBLOCKSIZE + 1;

typedef struct zio_sync_pass {
	int	zp_defer_free;		/* defer frees after this pass */
	int	zp_dontcompress;	/* don't compress after this pass */
	int	zp_rewrite;		/* rewrite new bps after this pass */
} zio_sync_pass_t;

zio_sync_pass_t zio_sync_pass = {
	1,	/* zp_defer_free */
	4,	/* zp_dontcompress */
	1,	/* zp_rewrite */
};

/*
 * ==========================================================================
 * I/O kmem caches
 * ==========================================================================
 */
kmem_cache_t *zio_buf_cache[SPA_MAXBLOCKSIZE >> SPA_MINBLOCKSHIFT];

void
zio_init(void)
{
	size_t c;

	/*
	 * For small buffers, we want a cache for each multiple of
	 * SPA_MINBLOCKSIZE.  For medium-size buffers, we want a cache
	 * for each quarter-power of 2.  For large buffers, we want
	 * a cache for each multiple of PAGESIZE.
	 */
	for (c = 0; c < SPA_MAXBLOCKSIZE >> SPA_MINBLOCKSHIFT; c++) {
		size_t size = (c + 1) << SPA_MINBLOCKSHIFT;
		size_t p2 = size;
		size_t align = 0;

		while (p2 & (p2 - 1))
			p2 &= p2 - 1;

		if (size <= 4 * SPA_MINBLOCKSIZE) {
			align = SPA_MINBLOCKSIZE;
		} else if (P2PHASE(size, PAGESIZE) == 0) {
			align = PAGESIZE;
		} else if (P2PHASE(size, p2 >> 2) == 0) {
			align = p2 >> 2;
		}

		if (align != 0) {
			char name[30];
			(void) sprintf(name, "zio_buf_%lu", size);
			zio_buf_cache[c] = kmem_cache_create(name, size,
			    align, NULL, NULL, NULL, NULL, NULL, KMC_NODEBUG);
			dprintf("creating cache for size %5lx align %5lx\n",
			    size, align);
		}
	}

	while (--c != 0) {
		ASSERT(zio_buf_cache[c] != NULL);
		if (zio_buf_cache[c - 1] == NULL)
			zio_buf_cache[c - 1] = zio_buf_cache[c];
	}
}

void
zio_fini(void)
{
	size_t c;
	kmem_cache_t *last_cache = NULL;

	for (c = 0; c < SPA_MAXBLOCKSIZE >> SPA_MINBLOCKSHIFT; c++) {
		if (zio_buf_cache[c] != last_cache) {
			last_cache = zio_buf_cache[c];
			kmem_cache_destroy(zio_buf_cache[c]);
		}
		zio_buf_cache[c] = NULL;
	}
}

/*
 * ==========================================================================
 * Allocate and free I/O buffers
 * ==========================================================================
 */
void *
zio_buf_alloc(size_t size)
{
	size_t c = (size - 1) >> SPA_MINBLOCKSHIFT;

	ASSERT(c < SPA_MAXBLOCKSIZE >> SPA_MINBLOCKSHIFT);

	return (kmem_cache_alloc(zio_buf_cache[c], KM_SLEEP));
}

void
zio_buf_free(void *buf, size_t size)
{
	size_t c = (size - 1) >> SPA_MINBLOCKSHIFT;

	ASSERT(c < SPA_MAXBLOCKSIZE >> SPA_MINBLOCKSHIFT);

	kmem_cache_free(zio_buf_cache[c], buf);
}

/*
 * ==========================================================================
 * Push and pop I/O transform buffers
 * ==========================================================================
 */
static void
zio_push_transform(zio_t *zio, void *data, uint64_t size, uint64_t bufsize)
{
	zio_transform_t *zt = kmem_alloc(sizeof (zio_transform_t), KM_SLEEP);

	zt->zt_data = data;
	zt->zt_size = size;
	zt->zt_bufsize = bufsize;

	zt->zt_next = zio->io_transform_stack;
	zio->io_transform_stack = zt;

	zio->io_data = data;
	zio->io_size = size;
}

static void
zio_pop_transform(zio_t *zio, void **data, uint64_t *size, uint64_t *bufsize)
{
	zio_transform_t *zt = zio->io_transform_stack;

	*data = zt->zt_data;
	*size = zt->zt_size;
	*bufsize = zt->zt_bufsize;

	zio->io_transform_stack = zt->zt_next;
	kmem_free(zt, sizeof (zio_transform_t));

	if ((zt = zio->io_transform_stack) != NULL) {
		zio->io_data = zt->zt_data;
		zio->io_size = zt->zt_size;
	}
}

static void
zio_clear_transform_stack(zio_t *zio)
{
	void *data;
	uint64_t size, bufsize;

	ASSERT(zio->io_transform_stack != NULL);

	zio_pop_transform(zio, &data, &size, &bufsize);
	while (zio->io_transform_stack != NULL) {
		zio_buf_free(data, bufsize);
		zio_pop_transform(zio, &data, &size, &bufsize);
	}
}

/*
 * ==========================================================================
 * Create the various types of I/O (read, write, free)
 * ==========================================================================
 */
static zio_t *
zio_create(zio_t *pio, spa_t *spa, uint64_t txg, blkptr_t *bp,
    void *data, uint64_t size, zio_done_func_t *done, void *private,
    zio_type_t type, int priority, int flags, uint8_t stage, uint32_t pipeline)
{
	zio_t *zio;

	ASSERT3U(size, <=, SPA_MAXBLOCKSIZE);
	ASSERT(P2PHASE(size, SPA_MINBLOCKSIZE) == 0);

	zio = kmem_zalloc(sizeof (zio_t), KM_SLEEP);
	zio->io_parent = pio;
	zio->io_spa = spa;
	zio->io_txg = txg;
	if (bp != NULL) {
		zio->io_bp = bp;
		zio->io_bp_copy = *bp;
		zio->io_bp_orig = *bp;
		/* XXBP - Need to inherit this when it matters */
		zio->io_dva_index = 0;
	}
	zio->io_done = done;
	zio->io_private = private;
	zio->io_type = type;
	zio->io_priority = priority;
	zio->io_stage = stage;
	zio->io_pipeline = pipeline;
	zio->io_async_stages = ZIO_ASYNC_PIPELINE_STAGES;
	zio->io_timestamp = lbolt64;
	zio->io_flags = flags;
	zio_push_transform(zio, data, size, size);

	if (pio == NULL) {
		if (!(flags & ZIO_FLAG_CONFIG_HELD))
			spa_config_enter(zio->io_spa, RW_READER);
		zio->io_root = zio;
	} else {
		zio->io_root = pio->io_root;

		mutex_enter(&pio->io_lock);
		if (stage < ZIO_STAGE_READY)
			pio->io_children_notready++;
		pio->io_children_notdone++;
		zio->io_sibling_next = pio->io_child;
		zio->io_sibling_prev = NULL;
		if (pio->io_child != NULL)
			pio->io_child->io_sibling_prev = zio;
		pio->io_child = zio;
		mutex_exit(&pio->io_lock);
	}

	return (zio);
}

zio_t *
zio_null(zio_t *pio, spa_t *spa, zio_done_func_t *done, void *private,
	int flags)
{
	zio_t *zio;

	zio = zio_create(pio, spa, 0, NULL, NULL, 0, done, private,
	    ZIO_TYPE_NULL, ZIO_PRIORITY_NOW, flags, ZIO_STAGE_OPEN,
	    ZIO_WAIT_FOR_CHILDREN_PIPELINE);

	return (zio);
}

zio_t *
zio_root(spa_t *spa, zio_done_func_t *done, void *private, int flags)
{
	return (zio_null(NULL, spa, done, private, flags));
}

zio_t *
zio_read(zio_t *pio, spa_t *spa, blkptr_t *bp, void *data,
    uint64_t size, zio_done_func_t *done, void *private,
    int priority, int flags)
{
	zio_t *zio;
	dva_t *dva;

	ASSERT3U(size, ==, BP_GET_LSIZE(bp));

	zio = zio_create(pio, spa, bp->blk_birth, bp, data, size, done, private,
	    ZIO_TYPE_READ, priority, flags, ZIO_STAGE_OPEN, ZIO_READ_PIPELINE);

	/*
	 * Work off our copy of the bp so the caller can free it.
	 */
	zio->io_bp = &zio->io_bp_copy;

	bp = zio->io_bp;
	dva = ZIO_GET_DVA(zio);

	if (BP_GET_COMPRESS(bp) != ZIO_COMPRESS_OFF) {
		uint64_t csize = BP_GET_PSIZE(bp);
		void *cbuf = zio_buf_alloc(csize);

		zio_push_transform(zio, cbuf, csize, csize);
		zio->io_pipeline |= 1U << ZIO_STAGE_READ_DECOMPRESS;
	}

	if (DVA_GET_GANG(dva)) {
		uint64_t gsize = SPA_GANGBLOCKSIZE;
		void *gbuf = zio_buf_alloc(gsize);

		zio_push_transform(zio, gbuf, gsize, gsize);
		zio->io_pipeline |= 1U << ZIO_STAGE_READ_GANG_MEMBERS;
	}

	return (zio);
}

zio_t *
zio_write(zio_t *pio, spa_t *spa, int checksum, int compress,
    uint64_t txg, blkptr_t *bp, void *data, uint64_t size,
    zio_done_func_t *done, void *private, int priority, int flags)
{
	zio_t *zio;

	ASSERT(checksum >= ZIO_CHECKSUM_OFF &&
	    checksum < ZIO_CHECKSUM_FUNCTIONS);

	ASSERT(compress >= ZIO_COMPRESS_OFF &&
	    compress < ZIO_COMPRESS_FUNCTIONS);

	zio = zio_create(pio, spa, txg, bp, data, size, done, private,
	    ZIO_TYPE_WRITE, priority, flags,
	    ZIO_STAGE_OPEN, ZIO_WRITE_PIPELINE);

	zio->io_checksum = checksum;
	zio->io_compress = compress;

	if (compress != ZIO_COMPRESS_OFF)
		zio->io_async_stages |= 1U << ZIO_STAGE_WRITE_COMPRESS;

	if (bp->blk_birth != txg) {
		/* XXX the bp usually (always?) gets re-zeroed later */
		BP_ZERO(bp);
		BP_SET_LSIZE(bp, size);
		BP_SET_PSIZE(bp, size);
	}

	return (zio);
}

zio_t *
zio_rewrite(zio_t *pio, spa_t *spa, int checksum,
    uint64_t txg, blkptr_t *bp, void *data, uint64_t size,
    zio_done_func_t *done, void *private, int priority, int flags)
{
	zio_t *zio;

	/* XXBP - We need to re-evaluate when to insert pipeline stages */
	zio = zio_create(pio, spa, txg, bp, data, size, done, private,
	    ZIO_TYPE_WRITE, priority, flags,
	    ZIO_STAGE_OPEN, ZIO_REWRITE_PIPELINE);

	zio->io_checksum = checksum;
	zio->io_compress = ZIO_COMPRESS_OFF;

	return (zio);
}

static zio_t *
zio_write_allocate(zio_t *pio, spa_t *spa, int checksum,
    uint64_t txg, blkptr_t *bp, void *data, uint64_t size,
    zio_done_func_t *done, void *private, int priority, int flags)
{
	zio_t *zio;

	BP_ZERO(bp);
	BP_SET_LSIZE(bp, size);
	BP_SET_PSIZE(bp, size);
	BP_SET_COMPRESS(bp, ZIO_COMPRESS_OFF);

	zio = zio_create(pio, spa, txg, bp, data, size, done, private,
	    ZIO_TYPE_WRITE, priority, flags,
	    ZIO_STAGE_OPEN, ZIO_WRITE_ALLOCATE_PIPELINE);

	zio->io_checksum = checksum;
	zio->io_compress = ZIO_COMPRESS_OFF;

	return (zio);
}

zio_t *
zio_free(zio_t *pio, spa_t *spa, uint64_t txg, blkptr_t *bp,
    zio_done_func_t *done, void *private)
{
	zio_t *zio;

	ASSERT(!BP_IS_HOLE(bp));

	if (txg == spa->spa_syncing_txg &&
	    spa->spa_sync_pass > zio_sync_pass.zp_defer_free) {
		bplist_enqueue_deferred(&spa->spa_sync_bplist, bp);
		return (zio_null(pio, spa, NULL, NULL, 0));
	}

	/* XXBP - We need to re-evaluate when to insert pipeline stages */
	zio = zio_create(pio, spa, txg, bp, NULL, 0, done, private,
	    ZIO_TYPE_FREE, ZIO_PRIORITY_FREE, 0,
	    ZIO_STAGE_OPEN, ZIO_FREE_PIPELINE);

	zio->io_bp = &zio->io_bp_copy;

	return (zio);
}

zio_t *
zio_claim(zio_t *pio, spa_t *spa, uint64_t txg, blkptr_t *bp,
    zio_done_func_t *done, void *private)
{
	zio_t *zio;

	/*
	 * A claim is an allocation of a specific block.  Claims are needed
	 * to support immediate writes in the intent log.  The issue is that
	 * immediate writes contain committed data, but in a txg that was
	 * *not* committed.  Upon opening the pool after an unclean shutdown,
	 * the intent log claims all blocks that contain immediate write data
	 * so that the SPA knows they're in use.
	 *
	 * All claims *must* be resolved in the first txg -- before the SPA
	 * starts allocating blocks -- so that nothing is allocated twice.
	 */
	ASSERT3U(spa->spa_uberblock.ub_rootbp.blk_birth, <, spa_first_txg(spa));
	ASSERT3U(spa_first_txg(spa), <=, txg);

	/* XXBP - We need to re-evaluate when to insert pipeline stages */
	zio = zio_create(pio, spa, txg, bp, NULL, 0, done, private,
	    ZIO_TYPE_CLAIM, ZIO_PRIORITY_NOW, 0,
	    ZIO_STAGE_OPEN, ZIO_CLAIM_PIPELINE);

	zio->io_bp = &zio->io_bp_copy;

	return (zio);
}

zio_t *
zio_ioctl(zio_t *pio, spa_t *spa, vdev_t *vd, int cmd,
    zio_done_func_t *done, void *private, int priority, int flags)
{
	zio_t *zio;
	int c;

	if (vd->vdev_children == 0) {
		zio = zio_create(pio, spa, 0, NULL, NULL, 0, done, private,
		    ZIO_TYPE_IOCTL, priority, flags,
		    ZIO_STAGE_OPEN, ZIO_IOCTL_PIPELINE);

		zio->io_vd = vd;
		zio->io_cmd = cmd;
	} else {
		zio = zio_null(pio, spa, NULL, NULL, flags);

		for (c = 0; c < vd->vdev_children; c++)
			zio_nowait(zio_ioctl(zio, spa, vd->vdev_child[c], cmd,
			    done, private, priority, flags));
	}

	return (zio);
}

static void
zio_phys_bp_init(vdev_t *vd, blkptr_t *bp, uint64_t offset, uint64_t size,
    int checksum)
{
	ASSERT(vd->vdev_children == 0);

	ASSERT(size <= SPA_MAXBLOCKSIZE);
	ASSERT(P2PHASE(size, SPA_MINBLOCKSIZE) == 0);
	ASSERT(P2PHASE(offset, SPA_MINBLOCKSIZE) == 0);

	ASSERT(offset + size <= VDEV_LABEL_START_SIZE ||
	    offset >= vd->vdev_psize - VDEV_LABEL_END_SIZE);
	ASSERT3U(offset + size, <=, vd->vdev_psize);

	BP_ZERO(bp);

	BP_SET_LSIZE(bp, size);
	BP_SET_PSIZE(bp, size);

	BP_SET_CHECKSUM(bp, checksum);
	BP_SET_COMPRESS(bp, ZIO_COMPRESS_OFF);
	BP_SET_BYTEORDER(bp, ZFS_HOST_BYTEORDER);

	if (checksum != ZIO_CHECKSUM_OFF)
		ZIO_SET_CHECKSUM(&bp->blk_cksum, offset, 0, 0, 0);
}

zio_t *
zio_read_phys(zio_t *pio, vdev_t *vd, uint64_t offset, uint64_t size,
    void *data, int checksum, zio_done_func_t *done, void *private,
    int priority, int flags)
{
	zio_t *zio;
	blkptr_t blk;

	zio_phys_bp_init(vd, &blk, offset, size, checksum);

	zio = zio_create(pio, vd->vdev_spa, 0, &blk, data, size, done, private,
	    ZIO_TYPE_READ, priority, flags | ZIO_FLAG_PHYSICAL,
	    ZIO_STAGE_OPEN, ZIO_READ_PHYS_PIPELINE);

	zio->io_vd = vd;
	zio->io_offset = offset;

	/*
	 * Work off our copy of the bp so the caller can free it.
	 */
	zio->io_bp = &zio->io_bp_copy;

	return (zio);
}

zio_t *
zio_write_phys(zio_t *pio, vdev_t *vd, uint64_t offset, uint64_t size,
    void *data, int checksum, zio_done_func_t *done, void *private,
    int priority, int flags)
{
	zio_block_tail_t *zbt;
	void *wbuf;
	zio_t *zio;
	blkptr_t blk;

	zio_phys_bp_init(vd, &blk, offset, size, checksum);

	zio = zio_create(pio, vd->vdev_spa, 0, &blk, data, size, done, private,
	    ZIO_TYPE_WRITE, priority, flags | ZIO_FLAG_PHYSICAL,
	    ZIO_STAGE_OPEN, ZIO_WRITE_PHYS_PIPELINE);

	zio->io_vd = vd;
	zio->io_offset = offset;

	zio->io_bp = &zio->io_bp_copy;
	zio->io_checksum = checksum;

	if (zio_checksum_table[checksum].ci_zbt) {
		/*
		 * zbt checksums are necessarily destructive -- they modify
		 * one word of the write buffer to hold the verifier/checksum.
		 * Therefore, we must make a local copy in case the data is
		 * being written to multiple places.
		 */
		wbuf = zio_buf_alloc(size);
		bcopy(data, wbuf, size);
		zio_push_transform(zio, wbuf, size, size);

		zbt = (zio_block_tail_t *)((char *)wbuf + size) - 1;
		zbt->zbt_cksum = blk.blk_cksum;
	}

	return (zio);
}

/*
 * Create a child I/O to do some work for us.  It has no associated bp.
 */
zio_t *
zio_vdev_child_io(zio_t *zio, blkptr_t *bp, vdev_t *vd, uint64_t offset,
	void *data, uint64_t size, int type, int priority, int flags,
	zio_done_func_t *done, void *private)
{
	uint32_t pipeline = ZIO_VDEV_CHILD_PIPELINE;
	zio_t *cio;

	if (type == ZIO_TYPE_READ && bp != NULL) {
		/*
		 * If we have the bp, then the child should perform the
		 * checksum and the parent need not.  This pushes error
		 * detection as close to the leaves as possible and
		 * eliminates redundant checksums in the interior nodes.
		 */
		pipeline |= 1U << ZIO_STAGE_CHECKSUM_VERIFY;
		zio->io_pipeline &= ~(1U << ZIO_STAGE_CHECKSUM_VERIFY);
	}

	cio = zio_create(zio, zio->io_spa, zio->io_txg, bp, data, size,
	    done, private, type, priority,
	    (zio->io_flags & ZIO_FLAG_VDEV_INHERIT) | ZIO_FLAG_CANFAIL | flags,
	    ZIO_STAGE_VDEV_IO_SETUP - 1, pipeline);

	cio->io_vd = vd;
	cio->io_offset = offset;

	return (cio);
}

/*
 * ==========================================================================
 * Initiate I/O, either sync or async
 * ==========================================================================
 */
int
zio_wait(zio_t *zio)
{
	int error;

	ASSERT(zio->io_stage == ZIO_STAGE_OPEN);

	zio->io_waiter = curthread;

	zio_next_stage_async(zio);

	mutex_enter(&zio->io_lock);
	while (zio->io_stalled != ZIO_STAGE_DONE)
		cv_wait(&zio->io_cv, &zio->io_lock);
	mutex_exit(&zio->io_lock);

	error = zio->io_error;

	kmem_free(zio, sizeof (zio_t));

	return (error);
}

void
zio_nowait(zio_t *zio)
{
	zio_next_stage_async(zio);
}

/*
 * ==========================================================================
 * I/O pipeline interlocks: parent/child dependency scoreboarding
 * ==========================================================================
 */
static void
zio_wait_for_children(zio_t *zio, uint32_t stage, uint64_t *countp)
{
	mutex_enter(&zio->io_lock);
	if (*countp == 0) {
		ASSERT(zio->io_stalled == 0);
		mutex_exit(&zio->io_lock);
		zio_next_stage(zio);
	} else {
		if (zio->io_stage == ZIO_STAGE_VDEV_IO_START)
			zio_vdev_io_exit(zio);
		zio->io_stalled = stage;
		mutex_exit(&zio->io_lock);
	}
}

static void
zio_notify_parent(zio_t *zio, uint32_t stage, uint64_t *countp)
{
	zio_t *pio = zio->io_parent;

	mutex_enter(&pio->io_lock);
	if (pio->io_error == 0 && !(zio->io_flags & ZIO_FLAG_DONT_PROPAGATE))
		pio->io_error = zio->io_error;
	if (--*countp == 0 && pio->io_stalled == stage) {
		if (pio->io_stage == ZIO_STAGE_VDEV_IO_START)
			zio_vdev_io_enter(pio);
		pio->io_stalled = 0;
		mutex_exit(&pio->io_lock);
		zio_next_stage_async(pio);
	} else {
		mutex_exit(&pio->io_lock);
	}
}

static void
zio_wait_children_ready(zio_t *zio)
{
	zio_wait_for_children(zio, ZIO_STAGE_WAIT_CHILDREN_READY,
	    &zio->io_children_notready);
}

void
zio_wait_children_done(zio_t *zio)
{
	zio_wait_for_children(zio, ZIO_STAGE_WAIT_CHILDREN_DONE,
	    &zio->io_children_notdone);
}

static void
zio_ready(zio_t *zio)
{
	zio_t *pio = zio->io_parent;

	if (pio != NULL)
		zio_notify_parent(zio, ZIO_STAGE_WAIT_CHILDREN_READY,
		    &pio->io_children_notready);

	if (zio->io_bp)
		zio->io_bp_copy = *zio->io_bp;

	zio_next_stage(zio);
}

static void
zio_done(zio_t *zio)
{
	zio_t *pio = zio->io_parent;
	spa_t *spa = zio->io_spa;
	blkptr_t *bp = zio->io_bp;
	vdev_t *vd = zio->io_vd;
	char blkbuf[300];

	ASSERT(zio->io_children_notready == 0);
	ASSERT(zio->io_children_notdone == 0);

	if (bp != NULL) {
		ASSERT(bp->blk_pad[0] == 0);
		ASSERT(bp->blk_pad[1] == 0);
		ASSERT(bp->blk_pad[2] == 0);
		ASSERT(bcmp(bp, &zio->io_bp_copy, sizeof (blkptr_t)) == 0);
		if (zio->io_type == ZIO_TYPE_WRITE && !BP_IS_HOLE(bp) &&
		    !(zio->io_flags & ZIO_FLAG_IO_REPAIR))
			ASSERT(!BP_SHOULD_BYTESWAP(bp));
	}

	if (vd != NULL)
		vdev_stat_update(zio);

	if (zio->io_error) {
		sprintf_blkptr(blkbuf, bp ? bp : &zio->io_bp_copy);
		dprintf("ZFS: %s (%s on %s off %llx: zio %p %s): error %d\n",
		    zio->io_error == ECKSUM ? "bad checksum" : "I/O failure",
		    zio_type_name[zio->io_type],
		    vdev_description(vd),
		    (u_longlong_t)zio->io_offset,
		    zio, blkbuf, zio->io_error);
	}

	if (zio->io_numerrors != 0 && zio->io_type == ZIO_TYPE_WRITE) {
		sprintf_blkptr(blkbuf, bp ? bp : &zio->io_bp_copy);
		dprintf("ZFS: %s (%s on %s off %llx: zio %p %s): %d errors\n",
		    "partial write",
		    zio_type_name[zio->io_type],
		    vdev_description(vd),
		    (u_longlong_t)zio->io_offset,
		    zio, blkbuf, zio->io_numerrors);
	}

	if (zio->io_error && !(zio->io_flags & ZIO_FLAG_CANFAIL)) {
		sprintf_blkptr(blkbuf, bp ? bp : &zio->io_bp_copy);
		panic("ZFS: %s (%s on %s off %llx: zio %p %s): error %d",
		    zio->io_error == ECKSUM ? "bad checksum" : "I/O failure",
		    zio_type_name[zio->io_type],
		    vdev_description(vd),
		    (u_longlong_t)zio->io_offset,
		    zio, blkbuf, zio->io_error);
	}

	zio_clear_transform_stack(zio);

	if (zio->io_done)
		zio->io_done(zio);

	ASSERT(zio->io_delegate_list == NULL);
	ASSERT(zio->io_delegate_next == NULL);

	if (pio != NULL) {
		zio_t *next, *prev;

		mutex_enter(&pio->io_lock);
		next = zio->io_sibling_next;
		prev = zio->io_sibling_prev;
		if (next != NULL)
			next->io_sibling_prev = prev;
		if (prev != NULL)
			prev->io_sibling_next = next;
		if (pio->io_child == zio)
			pio->io_child = next;
		mutex_exit(&pio->io_lock);

		zio_notify_parent(zio, ZIO_STAGE_WAIT_CHILDREN_DONE,
		    &pio->io_children_notdone);
	}

	if (pio == NULL && !(zio->io_flags & ZIO_FLAG_CONFIG_HELD))
		spa_config_exit(spa);

	if (zio->io_waiter != NULL) {
		mutex_enter(&zio->io_lock);
		ASSERT(zio->io_stage == ZIO_STAGE_DONE);
		zio->io_stalled = zio->io_stage;
		cv_broadcast(&zio->io_cv);
		mutex_exit(&zio->io_lock);
	} else {
		kmem_free(zio, sizeof (zio_t));
	}
}

/*
 * ==========================================================================
 * Compression support
 * ==========================================================================
 */
static void
zio_write_compress(zio_t *zio)
{
	int compress = zio->io_compress;
	blkptr_t *bp = zio->io_bp;
	void *cbuf;
	uint64_t lsize = zio->io_size;
	uint64_t csize = lsize;
	uint64_t cbufsize = 0;
	int pass;

	if (bp->blk_birth == zio->io_txg) {
		/*
		 * We're rewriting an existing block, which means we're
		 * working on behalf of spa_sync().  For spa_sync() to
		 * converge, it must eventually be the case that we don't
		 * have to allocate new blocks.  But compression changes
		 * the blocksize, which forces a reallocate, and makes
		 * convergence take longer.  Therefore, after the first
		 * few passes, stop compressing to ensure convergence.
		 */
		pass = spa_sync_pass(zio->io_spa);
		if (pass > zio_sync_pass.zp_dontcompress)
			compress = ZIO_COMPRESS_OFF;
	} else {
		ASSERT(BP_IS_HOLE(bp));
		pass = 1;
	}

	if (compress != ZIO_COMPRESS_OFF)
		if (!zio_compress_data(compress, zio->io_data, zio->io_size,
		    &cbuf, &csize, &cbufsize))
			compress = ZIO_COMPRESS_OFF;

	if (compress != ZIO_COMPRESS_OFF && csize != 0)
		zio_push_transform(zio, cbuf, csize, cbufsize);

	/*
	 * The final pass of spa_sync() must be all rewrites, but the first
	 * few passes offer a trade-off: allocating blocks defers convergence,
	 * but newly allocated blocks are sequential, so they can be written
	 * to disk faster.  Therefore, we allow the first few passes of
	 * spa_sync() to reallocate new blocks, but force rewrites after that.
	 * There should only be a handful of blocks after pass 1 in any case.
	 */
	if (bp->blk_birth == zio->io_txg && BP_GET_PSIZE(bp) == csize &&
	    pass > zio_sync_pass.zp_rewrite) {
		ASSERT(csize != 0);
		ASSERT3U(BP_GET_COMPRESS(bp), ==, compress);
		ASSERT3U(BP_GET_LSIZE(bp), ==, lsize);

		zio->io_pipeline = ZIO_REWRITE_PIPELINE;
	} else {
		if (bp->blk_birth == zio->io_txg) {
			ASSERT3U(BP_GET_LSIZE(bp), ==, lsize);
			bzero(bp, sizeof (blkptr_t));
		}
		if (csize == 0) {
			BP_ZERO(bp);
			zio->io_pipeline = ZIO_WAIT_FOR_CHILDREN_PIPELINE;
		} else {
			BP_SET_LSIZE(bp, lsize);
			BP_SET_PSIZE(bp, csize);
			BP_SET_COMPRESS(bp, compress);
			zio->io_pipeline = ZIO_WRITE_ALLOCATE_PIPELINE;
		}
	}

	zio_next_stage(zio);
}

static void
zio_read_decompress(zio_t *zio)
{
	blkptr_t *bp = zio->io_bp;
	void *data;
	uint64_t size;
	uint64_t bufsize;
	int compress = BP_GET_COMPRESS(bp);

	ASSERT(compress != ZIO_COMPRESS_OFF);

	zio_pop_transform(zio, &data, &size, &bufsize);

	if (zio_decompress_data(compress, data, size,
	    zio->io_data, zio->io_size))
		zio->io_error = EIO;

	zio_buf_free(data, bufsize);

	zio_next_stage(zio);
}

/*
 * ==========================================================================
 * Gang block support
 * ==========================================================================
 */
static void
zio_gang_pipeline(zio_t *zio)
{
	/*
	 * By default, the pipeline assumes that we're dealing with a gang
	 * block.  If we're not, strip out any gang-specific stages.
	 */
	if (!DVA_GET_GANG(ZIO_GET_DVA(zio)))
		zio->io_pipeline &= ~ZIO_GANG_STAGES;

	zio_next_stage(zio);
}

static void
zio_gang_byteswap(zio_t *zio)
{
	ASSERT(zio->io_size == SPA_GANGBLOCKSIZE);

	if (BP_SHOULD_BYTESWAP(zio->io_bp))
		byteswap_uint64_array(zio->io_data, zio->io_size);
}

static void
zio_get_gang_header(zio_t *zio)
{
	blkptr_t *bp = zio->io_bp;
	uint64_t gsize = SPA_GANGBLOCKSIZE;
	void *gbuf = zio_buf_alloc(gsize);

	ASSERT(DVA_GET_GANG(ZIO_GET_DVA(zio)));

	zio_push_transform(zio, gbuf, gsize, gsize);

	zio_nowait(zio_create(zio, zio->io_spa, bp->blk_birth, bp, gbuf, gsize,
	    NULL, NULL, ZIO_TYPE_READ, zio->io_priority,
	    zio->io_flags & ZIO_FLAG_GANG_INHERIT,
	    ZIO_STAGE_OPEN, ZIO_READ_PIPELINE));

	zio_wait_children_done(zio);
}

static void
zio_read_gang_members(zio_t *zio)
{
	zio_gbh_phys_t *gbh;
	uint64_t gsize, gbufsize, loff, lsize;
	int i;

	ASSERT(DVA_GET_GANG(ZIO_GET_DVA(zio)));

	zio_gang_byteswap(zio);
	zio_pop_transform(zio, (void **)&gbh, &gsize, &gbufsize);

	for (loff = 0, i = 0; loff != zio->io_size; loff += lsize, i++) {
		blkptr_t *gbp = &gbh->zg_blkptr[i];
		lsize = BP_GET_PSIZE(gbp);

		ASSERT(BP_GET_COMPRESS(gbp) == ZIO_COMPRESS_OFF);
		ASSERT3U(lsize, ==, BP_GET_LSIZE(gbp));
		ASSERT3U(loff + lsize, <=, zio->io_size);
		ASSERT(i < SPA_GBH_NBLKPTRS);
		ASSERT(!BP_IS_HOLE(gbp));

		zio_nowait(zio_read(zio, zio->io_spa, gbp,
		    (char *)zio->io_data + loff, lsize, NULL, NULL,
		    zio->io_priority, zio->io_flags & ZIO_FLAG_GANG_INHERIT));
	}

	zio_buf_free(gbh, gbufsize);
	zio_wait_children_done(zio);
}

static void
zio_rewrite_gang_members(zio_t *zio)
{
	zio_gbh_phys_t *gbh;
	uint64_t gsize, gbufsize, loff, lsize;
	int i;

	ASSERT(DVA_GET_GANG(ZIO_GET_DVA(zio)));
	ASSERT3U(zio->io_size, ==, SPA_GANGBLOCKSIZE);

	zio_gang_byteswap(zio);
	zio_pop_transform(zio, (void **)&gbh, &gsize, &gbufsize);

	ASSERT(gsize == gbufsize);

	for (loff = 0, i = 0; loff != zio->io_size; loff += lsize, i++) {
		blkptr_t *gbp = &gbh->zg_blkptr[i];
		lsize = BP_GET_PSIZE(gbp);

		ASSERT(BP_GET_COMPRESS(gbp) == ZIO_COMPRESS_OFF);
		ASSERT3U(lsize, ==, BP_GET_LSIZE(gbp));
		ASSERT3U(loff + lsize, <=, zio->io_size);
		ASSERT(i < SPA_GBH_NBLKPTRS);
		ASSERT(!BP_IS_HOLE(gbp));

		zio_nowait(zio_rewrite(zio, zio->io_spa, zio->io_checksum,
		    zio->io_txg, gbp, (char *)zio->io_data + loff, lsize,
		    NULL, NULL, zio->io_priority, zio->io_flags));
	}

	zio_push_transform(zio, gbh, gsize, gbufsize);
	zio_wait_children_ready(zio);
}

static void
zio_free_gang_members(zio_t *zio)
{
	zio_gbh_phys_t *gbh;
	uint64_t gsize, gbufsize;
	int i;

	ASSERT(DVA_GET_GANG(ZIO_GET_DVA(zio)));

	zio_gang_byteswap(zio);
	zio_pop_transform(zio, (void **)&gbh, &gsize, &gbufsize);

	for (i = 0; i < SPA_GBH_NBLKPTRS; i++) {
		blkptr_t *gbp = &gbh->zg_blkptr[i];

		if (BP_IS_HOLE(gbp))
			continue;
		zio_nowait(zio_free(zio, zio->io_spa, zio->io_txg,
		    gbp, NULL, NULL));
	}

	zio_buf_free(gbh, gbufsize);
	zio_next_stage(zio);
}

static void
zio_claim_gang_members(zio_t *zio)
{
	zio_gbh_phys_t *gbh;
	uint64_t gsize, gbufsize;
	int i;

	ASSERT(DVA_GET_GANG(ZIO_GET_DVA(zio)));

	zio_gang_byteswap(zio);
	zio_pop_transform(zio, (void **)&gbh, &gsize, &gbufsize);

	for (i = 0; i < SPA_GBH_NBLKPTRS; i++) {
		blkptr_t *gbp = &gbh->zg_blkptr[i];
		if (BP_IS_HOLE(gbp))
			continue;
		zio_nowait(zio_claim(zio, zio->io_spa, zio->io_txg,
		    gbp, NULL, NULL));
	}

	zio_buf_free(gbh, gbufsize);
	zio_next_stage(zio);
}

static void
zio_write_allocate_gang_member_done(zio_t *zio)
{
	zio_t *pio = zio->io_parent;
	dva_t *cdva = ZIO_GET_DVA(zio);
	dva_t *pdva = ZIO_GET_DVA(pio);
	uint64_t asize;

	ASSERT(DVA_GET_GANG(pdva));

	/* XXBP - Need to be careful here with multiple DVAs */
	mutex_enter(&pio->io_lock);
	asize = DVA_GET_ASIZE(pdva);
	asize += DVA_GET_ASIZE(cdva);
	DVA_SET_ASIZE(pdva, asize);
	mutex_exit(&pio->io_lock);
}

static void
zio_write_allocate_gang_members(zio_t *zio)
{
	blkptr_t *bp = zio->io_bp;
	dva_t *dva = ZIO_GET_DVA(zio);
	zio_gbh_phys_t *gbh;
	uint64_t resid = zio->io_size;
	uint64_t maxalloc = P2ROUNDUP(zio->io_size >> 1, SPA_MINBLOCKSIZE);
	uint64_t gsize, loff, lsize;
	uint32_t gbps_left;
	int error;
	int i;

	gsize = SPA_GANGBLOCKSIZE;
	gbps_left = SPA_GBH_NBLKPTRS;

	error = metaslab_alloc(zio->io_spa, gsize, dva, zio->io_txg);
	if (error == ENOSPC)
		panic("can't allocate gang block header");
	ASSERT(error == 0);

	DVA_SET_GANG(dva, 1);

	bp->blk_birth = zio->io_txg;

	gbh = zio_buf_alloc(gsize);
	bzero(gbh, gsize);

	for (loff = 0, i = 0; loff != zio->io_size;
	    loff += lsize, resid -= lsize, gbps_left--, i++) {
		blkptr_t *gbp = &gbh->zg_blkptr[i];
		dva = &gbp->blk_dva[0];

		ASSERT(gbps_left != 0);
		maxalloc = MIN(maxalloc, resid);

		while (resid <= maxalloc * gbps_left) {
			error = metaslab_alloc(zio->io_spa, maxalloc, dva,
			    zio->io_txg);
			if (error == 0)
				break;
			ASSERT3U(error, ==, ENOSPC);
			if (maxalloc == SPA_MINBLOCKSIZE)
				panic("really out of space");
			maxalloc = P2ROUNDUP(maxalloc >> 1, SPA_MINBLOCKSIZE);
		}

		if (resid <= maxalloc * gbps_left) {
			lsize = maxalloc;
			BP_SET_LSIZE(gbp, lsize);
			BP_SET_PSIZE(gbp, lsize);
			BP_SET_COMPRESS(gbp, ZIO_COMPRESS_OFF);
			gbp->blk_birth = zio->io_txg;
			zio_nowait(zio_rewrite(zio, zio->io_spa,
			    zio->io_checksum, zio->io_txg, gbp,
			    (char *)zio->io_data + loff, lsize,
			    zio_write_allocate_gang_member_done, NULL,
			    zio->io_priority, zio->io_flags));
		} else {
			lsize = P2ROUNDUP(resid / gbps_left, SPA_MINBLOCKSIZE);
			ASSERT(lsize != SPA_MINBLOCKSIZE);
			zio_nowait(zio_write_allocate(zio, zio->io_spa,
			    zio->io_checksum, zio->io_txg, gbp,
			    (char *)zio->io_data + loff, lsize,
			    zio_write_allocate_gang_member_done, NULL,
			    zio->io_priority, zio->io_flags));
		}
	}

	ASSERT(resid == 0 && loff == zio->io_size);

	zio->io_pipeline |= 1U << ZIO_STAGE_GANG_CHECKSUM_GENERATE;

	zio_push_transform(zio, gbh, gsize, gsize);
	zio_wait_children_done(zio);
}

/*
 * ==========================================================================
 * Allocate and free blocks
 * ==========================================================================
 */
static void
zio_dva_allocate(zio_t *zio)
{
	blkptr_t *bp = zio->io_bp;
	dva_t *dva = ZIO_GET_DVA(zio);
	int error;

	ASSERT(BP_IS_HOLE(bp));

	/* For testing, make some blocks above a certain size be gang blocks */
	if (zio->io_size >= zio_gang_bang && (lbolt & 0x3) == 0) {
		zio_write_allocate_gang_members(zio);
		return;
	}

	ASSERT3U(zio->io_size, ==, BP_GET_PSIZE(bp));

	error = metaslab_alloc(zio->io_spa, zio->io_size, dva, zio->io_txg);

	if (error == 0) {
		bp->blk_birth = zio->io_txg;
	} else if (error == ENOSPC) {
		if (zio->io_size == SPA_MINBLOCKSIZE)
			panic("really, truly out of space");
		zio_write_allocate_gang_members(zio);
		return;
	} else {
		zio->io_error = error;
	}
	zio_next_stage(zio);
}

static void
zio_dva_free(zio_t *zio)
{
	blkptr_t *bp = zio->io_bp;
	dva_t *dva = ZIO_GET_DVA(zio);

	ASSERT(!BP_IS_HOLE(bp));

	metaslab_free(zio->io_spa, dva, zio->io_txg);

	BP_ZERO(bp);

	zio_next_stage(zio);
}

static void
zio_dva_claim(zio_t *zio)
{
	blkptr_t *bp = zio->io_bp;
	dva_t *dva = ZIO_GET_DVA(zio);

	ASSERT(!BP_IS_HOLE(bp));

	zio->io_error = metaslab_claim(zio->io_spa, dva, zio->io_txg);

	zio_next_stage(zio);
}

static void
zio_dva_translate(zio_t *zio)
{
	spa_t *spa = zio->io_spa;
	dva_t *dva = ZIO_GET_DVA(zio);
	uint64_t vdev = DVA_GET_VDEV(dva);
	uint64_t offset = DVA_GET_OFFSET(dva);

	ASSERT3U(zio->io_size, ==, ZIO_GET_IOSIZE(zio));

	zio->io_offset = offset;

	if ((zio->io_vd = vdev_lookup_top(spa, vdev)) == NULL)
		zio->io_error = ENXIO;
	else if (offset + zio->io_size > zio->io_vd->vdev_asize)
		zio->io_error = EOVERFLOW;

	zio_next_stage(zio);
}

/*
 * ==========================================================================
 * Read and write to physical devices
 * ==========================================================================
 */
static void
zio_vdev_io_enter(zio_t *zio)
{
	vdev_t *tvd = zio->io_vd->vdev_top;

	mutex_enter(&tvd->vdev_io_lock);
	ASSERT(zio->io_pending.list_next == NULL);
	list_insert_tail(&tvd->vdev_io_pending, zio);
	mutex_exit(&tvd->vdev_io_lock);
}

static void
zio_vdev_io_exit(zio_t *zio)
{
	vdev_t *tvd = zio->io_vd->vdev_top;

	mutex_enter(&tvd->vdev_io_lock);
	ASSERT(zio->io_pending.list_next != NULL);
	list_remove(&tvd->vdev_io_pending, zio);
	if (list_head(&tvd->vdev_io_pending) == NULL)
		cv_broadcast(&tvd->vdev_io_cv);
	mutex_exit(&tvd->vdev_io_lock);
}

static void
zio_vdev_io_retry(void *vdarg)
{
	vdev_t *vd = vdarg;
	zio_t *zio, *zq;

	ASSERT(vd == vd->vdev_top);

	/* XXPOLICY */
	delay(hz);

	vdev_reopen(vd, &zq);

	while ((zio = zq) != NULL) {
		zq = zio->io_retry_next;
		zio->io_retry_next = NULL;
		dprintf("async retry #%d for I/O to %s offset %llx\n",
		    zio->io_retries, vdev_description(vd), zio->io_offset);
		zio_next_stage_async(zio);
	}
}

static void
zio_vdev_io_setup(zio_t *zio)
{
	vdev_t *vd = zio->io_vd;

	/* XXPOLICY */
	if (zio->io_retries == 0 && vd == vd->vdev_top)
		zio->io_flags |= ZIO_FLAG_FAILFAST;

	if (!(zio->io_flags & ZIO_FLAG_PHYSICAL) && vd->vdev_children == 0) {
		zio->io_flags |= ZIO_FLAG_PHYSICAL;
		zio->io_offset += VDEV_LABEL_START_SIZE;
	}

	zio_vdev_io_enter(zio);

	zio_next_stage(zio);
}

static void
zio_vdev_io_start(zio_t *zio)
{
	blkptr_t *bp = zio->io_bp;

	ASSERT(P2PHASE(zio->io_offset, 1ULL << zio->io_vd->vdev_ashift) == 0);
	ASSERT(P2PHASE(zio->io_size, 1ULL << zio->io_vd->vdev_ashift) == 0);
	ASSERT(bp == NULL || ZIO_GET_IOSIZE(zio) == zio->io_size);
	ASSERT(zio->io_type != ZIO_TYPE_WRITE || (spa_mode & FWRITE));

	vdev_io_start(zio);

	/* zio_next_stage_async() gets called from io completion interrupt */
}

static void
zio_vdev_io_done(zio_t *zio)
{
	vdev_io_done(zio);
}

/* XXPOLICY */
static boolean_t
zio_should_retry(zio_t *zio)
{
	vdev_t *vd = zio->io_vd;

	if (zio->io_error == 0)
		return (B_FALSE);
	if (zio->io_delegate_list != NULL)
		return (B_FALSE);
	if (vd != vd->vdev_top)
		return (B_FALSE);
	if (zio->io_flags & ZIO_FLAG_DONT_RETRY)
		return (B_FALSE);
	if (zio->io_retries > 300 &&
	    (zio->io_flags & (ZIO_FLAG_SPECULATIVE | ZIO_FLAG_CANFAIL)))
		return (B_FALSE);
	if (zio->io_retries > 1 &&
	    (zio->io_error == ECKSUM || zio->io_error == ENXIO))
		return (B_FALSE);

	return (B_TRUE);
}

static void
zio_vdev_io_assess(zio_t *zio)
{
	vdev_t *vd = zio->io_vd;
	vdev_t *tvd = vd->vdev_top;

	zio_vdev_io_exit(zio);

	ASSERT(zio->io_vsd == NULL);

	/*
	 * If the I/O failed, determine whether we should attempt to retry it.
	 */
	/* XXPOLICY */
	if (zio_should_retry(zio)) {
		zio_t *zq;

		ASSERT(tvd == vd);
		ASSERT(!(zio->io_flags & ZIO_FLAG_DONT_PROPAGATE));

		zio->io_retries++;
		zio->io_error = 0;
		zio->io_flags &= ZIO_FLAG_VDEV_INHERIT;
		/* XXPOLICY */
		zio->io_flags &= ~ZIO_FLAG_FAILFAST;
		zio->io_flags |= ZIO_FLAG_DONT_CACHE;
		zio->io_stage = ZIO_STAGE_VDEV_IO_SETUP - 1;

		dprintf("retry #%d for %s to %s offset %llx\n",
		    zio->io_retries, zio_type_name[zio->io_type],
		    vdev_description(vd), zio->io_offset);

		/*
		 * If this is the first retry, do it immediately.
		 */
		/* XXPOLICY */
		if (zio->io_retries == 1) {
			zio_next_stage_async(zio);
			return;
		}

		/*
		 * This was not the first retry, so go through the
		 * longer enqueue/delay/vdev_reopen() process.
		 */
		mutex_enter(&tvd->vdev_io_lock);
		ASSERT(zio->io_retry_next == NULL);
		zio->io_retry_next = zq = tvd->vdev_io_retry;
		tvd->vdev_io_retry = zio;
		mutex_exit(&tvd->vdev_io_lock);
		if (zq == NULL)
			(void) taskq_dispatch(
			    tvd->vdev_spa->spa_vdev_retry_taskq,
			    zio_vdev_io_retry, tvd, TQ_SLEEP);
		return;
	}

	zio_next_stage(zio);
}

void
zio_vdev_io_reissue(zio_t *zio)
{
	ASSERT(zio->io_stage == ZIO_STAGE_VDEV_IO_START);
	ASSERT(zio->io_error == 0);

	zio->io_stage--;
}

void
zio_vdev_io_redone(zio_t *zio)
{
	ASSERT(zio->io_stage == ZIO_STAGE_VDEV_IO_DONE);

	zio->io_stage--;
}

void
zio_vdev_io_bypass(zio_t *zio)
{
	ASSERT(zio->io_stage == ZIO_STAGE_VDEV_IO_START);
	ASSERT(zio->io_error == 0);

	zio->io_flags |= ZIO_FLAG_IO_BYPASS;
	zio->io_stage = ZIO_STAGE_VDEV_IO_ASSESS - 1;
}

/*
 * ==========================================================================
 * Generate and verify checksums
 * ==========================================================================
 */
static void
zio_checksum_generate(zio_t *zio)
{
	int checksum = zio->io_checksum;
	blkptr_t *bp = zio->io_bp;

	ASSERT3U(zio->io_size, ==, BP_GET_PSIZE(bp));

	BP_SET_CHECKSUM(bp, checksum);
	BP_SET_BYTEORDER(bp, ZFS_HOST_BYTEORDER);

	zio_checksum(checksum, &bp->blk_cksum, zio->io_data, zio->io_size);

	zio_next_stage(zio);
}

static void
zio_gang_checksum_generate(zio_t *zio)
{
	zio_cksum_t zc;
	zio_gbh_phys_t *gbh = zio->io_data;

	ASSERT3U(zio->io_size, ==, SPA_GANGBLOCKSIZE);
	ASSERT(DVA_GET_GANG(ZIO_GET_DVA(zio)));

	zio_set_gang_verifier(zio, &gbh->zg_tail.zbt_cksum);

	zio_checksum(ZIO_CHECKSUM_GANG_HEADER, &zc, zio->io_data, zio->io_size);

	zio_next_stage(zio);
}

static void
zio_checksum_verify(zio_t *zio)
{
	if (zio->io_bp != NULL) {
		zio->io_error = zio_checksum_error(zio);
		if (zio->io_error) {
			dprintf("bad checksum on vdev %s\n",
			    vdev_description(zio->io_vd));
		}
	}

	zio_next_stage(zio);
}

/*
 * Called by RAID-Z to ensure we don't compute the checksum twice.
 */
void
zio_checksum_verified(zio_t *zio)
{
	zio->io_pipeline &= ~(1U << ZIO_STAGE_CHECKSUM_VERIFY);
}

/*
 * Set the external verifier for a gang block based on stuff in the bp
 */
void
zio_set_gang_verifier(zio_t *zio, zio_cksum_t *zcp)
{
	zcp->zc_word[0] = DVA_GET_VDEV(ZIO_GET_DVA(zio));
	zcp->zc_word[1] = DVA_GET_OFFSET(ZIO_GET_DVA(zio));
	zcp->zc_word[2] = zio->io_bp->blk_birth;
	zcp->zc_word[3] = 0;
}

/*
 * ==========================================================================
 * Define the pipeline
 * ==========================================================================
 */
typedef void zio_pipe_stage_t(zio_t *zio);

static void
zio_badop(zio_t *zio)
{
	panic("Invalid I/O pipeline stage %u for zio %p", zio->io_stage, zio);
}

zio_pipe_stage_t *zio_pipeline[ZIO_STAGE_DONE + 2] = {
	zio_badop,
	zio_wait_children_ready,
	zio_write_compress,
	zio_checksum_generate,
	zio_gang_pipeline,
	zio_get_gang_header,
	zio_rewrite_gang_members,
	zio_free_gang_members,
	zio_claim_gang_members,
	zio_dva_allocate,
	zio_dva_free,
	zio_dva_claim,
	zio_gang_checksum_generate,
	zio_ready,
	zio_dva_translate,
	zio_vdev_io_setup,
	zio_vdev_io_start,
	zio_vdev_io_done,
	zio_vdev_io_assess,
	zio_wait_children_done,
	zio_checksum_verify,
	zio_read_gang_members,
	zio_read_decompress,
	zio_done,
	zio_badop
};

/*
 * Move an I/O to the next stage of the pipeline and execute that stage.
 * There's no locking on io_stage because there's no legitimate way for
 * multiple threads to be attempting to process the same I/O.
 */
void
zio_next_stage(zio_t *zio)
{
	uint32_t pipeline = zio->io_pipeline;

	ASSERT(!MUTEX_HELD(&zio->io_lock));

	if (zio->io_error) {
		dprintf("zio %p vdev %s offset %llx stage %d error %d\n",
		    zio, vdev_description(zio->io_vd),
		    zio->io_offset, zio->io_stage, zio->io_error);
		if (((1U << zio->io_stage) & ZIO_VDEV_IO_PIPELINE) == 0)
			pipeline &= ZIO_ERROR_PIPELINE_MASK;
	}

	while (((1U << ++zio->io_stage) & pipeline) == 0)
		continue;

	ASSERT(zio->io_stage <= ZIO_STAGE_DONE);
	ASSERT(zio->io_stalled == 0);

	zio_pipeline[zio->io_stage](zio);
}

void
zio_next_stage_async(zio_t *zio)
{
	taskq_t *tq;
	uint32_t pipeline = zio->io_pipeline;

	ASSERT(!MUTEX_HELD(&zio->io_lock));

	if (zio->io_error) {
		dprintf("zio %p vdev %s offset %llx stage %d error %d\n",
		    zio, vdev_description(zio->io_vd),
		    zio->io_offset, zio->io_stage, zio->io_error);
		if (((1U << zio->io_stage) & ZIO_VDEV_IO_PIPELINE) == 0)
			pipeline &= ZIO_ERROR_PIPELINE_MASK;
	}

	while (((1U << ++zio->io_stage) & pipeline) == 0)
		continue;

	ASSERT(zio->io_stage <= ZIO_STAGE_DONE);
	ASSERT(zio->io_stalled == 0);

	/*
	 * For performance, we'll probably want two sets of task queues:
	 * per-CPU issue taskqs and per-CPU completion taskqs.  The per-CPU
	 * part is for read performance: since we have to make a pass over
	 * the data to checksum it anyway, we want to do this on the same CPU
	 * that issued the read, because (assuming CPU scheduling affinity)
	 * that thread is probably still there.  Getting this optimization
	 * right avoids performance-hostile cache-to-cache transfers.
	 *
	 * Note that having two sets of task queues is also necessary for
	 * correctness: if all of the issue threads get bogged down waiting
	 * for dependent reads (e.g. metaslab freelist) to complete, then
	 * there won't be any threads available to service I/O completion
	 * interrupts.
	 */
	if ((1U << zio->io_stage) & zio->io_async_stages) {
		if (zio->io_stage < ZIO_STAGE_VDEV_IO_DONE)
			tq = zio->io_spa->spa_zio_issue_taskq[zio->io_type];
		else
			tq = zio->io_spa->spa_zio_intr_taskq[zio->io_type];
		(void) taskq_dispatch(tq,
		    (task_func_t *)zio_pipeline[zio->io_stage], zio, TQ_SLEEP);
	} else {
		zio_pipeline[zio->io_stage](zio);
	}
}

/*
 * Try to allocate an intent log block.  Return 0 on success, errno on failure.
 */
int
zio_alloc_blk(spa_t *spa, int checksum, uint64_t size, blkptr_t *bp,
    uint64_t txg)
{
	int error;

	spa_config_enter(spa, RW_READER);

	BP_ZERO(bp);

	error = metaslab_alloc(spa, size, BP_IDENTITY(bp), txg);

	if (error == 0) {
		BP_SET_CHECKSUM(bp, checksum);
		BP_SET_LSIZE(bp, size);
		BP_SET_PSIZE(bp, size);
		BP_SET_COMPRESS(bp, ZIO_COMPRESS_OFF);
		BP_SET_TYPE(bp, DMU_OT_INTENT_LOG);
		BP_SET_LEVEL(bp, 0);
		BP_SET_BYTEORDER(bp, ZFS_HOST_BYTEORDER);
		bp->blk_birth = txg;
	}

	spa_config_exit(spa);

	return (error);
}

/*
 * Free an intent log block.  We know it can't be a gang block, so there's
 * nothing to do except metaslab_free() it.
 */
void
zio_free_blk(spa_t *spa, blkptr_t *bp, uint64_t txg)
{
	ASSERT(DVA_GET_GANG(BP_IDENTITY(bp)) == 0);

	dprintf_bp(bp, "txg %llu: ", txg);

	spa_config_enter(spa, RW_READER);

	metaslab_free(spa, BP_IDENTITY(bp), txg);

	spa_config_exit(spa);
}