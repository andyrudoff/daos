/**
 * (C) Copyright 2016 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
 * The Government's rights to use, modify, reproduce, release, perform, display,
 * or disclose this software are subject to the terms of the Apache License as
 * provided in Contract No. B609815.
 * Any reproduction of computer software, computer software documentation, or
 * portions thereof marked with this legend must also reproduce the markings.
 */
/**
 * This file is part of daos
 *
 * vos/vos_obj.c
 */
#define DD_SUBSYS	DD_FAC(vos)

#include <daos/btree.h>
#include <daos_srv/vos.h>
#include <vos_internal.h>
#include <vos_hhash.h>
#include "vos_internal.h"

/** iterator for dkey/akey/recx */
struct vos_obj_iter {
	/* public part of the iterator */
	struct vos_iterator	 it_iter;
	/** handle of iterator */
	daos_handle_t		 it_hdl;
	/** condition of the iterator: epoch logic expression */
	vos_it_epc_expr_t	 it_epc_expr;
	/** condition of the iterator: epoch range */
	daos_epoch_range_t	 it_epr;
	/** condition of the iterator: attribute key */
	daos_key_t		 it_akey;
	/* reference on the object */
	struct vos_obj_ref	*it_oref;
};

struct vos_io_buf;

/** zero-copy I/O context */
struct vos_zc_context {
	bool			 zc_is_update;
	daos_epoch_t		 zc_epoch;
	/** number of vectors of the I/O */
	unsigned int		 zc_vec_nr;
	/** I/O buffers for all vectors */
	struct vec_io_buf	*zc_vbufs;
	/** reference on the object */
	struct vos_obj_ref	*zc_oref;
};

static void vos_zcc_destroy(struct vos_zc_context *zcc, int err);

/** I/O buffer for a vector */
struct vec_io_buf {
	/** scatter/gather list for the ZC IO on this vector */
	daos_sg_list_t		 vb_sgl;
	/** data offset of the in-use iov of vb_sgl (for non-zc) */
	daos_off_t		 vb_iov_off;
	/** in-use iov index of vb_sgl (for non-zc) */
	unsigned int		 vb_iov_at;
	/** number of pre-allocated pmem buffers (for zc update only) */
	unsigned int		 vb_mmid_nr;
	/** pre-allocated pmem buffers (for zc update only) */
	umem_id_t		*vb_mmids;
};

static bool
vbuf_empty(struct vec_io_buf *vbuf)
{
	return vbuf->vb_sgl.sg_iovs == NULL;
}

static bool
vbuf_exhausted(struct vec_io_buf *vbuf)
{
	D_ASSERT(vbuf->vb_iov_at <= vbuf->vb_sgl.sg_nr.num);
	return vbuf->vb_iov_at == vbuf->vb_sgl.sg_nr.num;
}

/**
 * This function copies @size bytes from @addr to iovs of @vbuf::vb_sgl.
 * If @addr is NULL but @len is non-zero, which represents a hole, this
 * function will skip @len bytes in @vbuf::vb_sgl (iovs are untouched).
 * NB: this function is for non-zc only.
 */
static int
vbuf_fetch(struct vec_io_buf *vbuf, void *addr, daos_size_t size)
{
	D_ASSERT(!vbuf_empty(vbuf));

	while (!vbuf_exhausted(vbuf)) {
		daos_iov_t	*iov;
		daos_size_t	 nob;

		iov = &vbuf->vb_sgl.sg_iovs[vbuf->vb_iov_at]; /* current iov */
		if (iov->iov_buf_len <= vbuf->vb_iov_off) {
			D_ERROR("Invalid iov[%d] "DF_U64"/"DF_U64"\n",
				vbuf->vb_iov_at, vbuf->vb_iov_off,
				iov->iov_buf_len);
			return -1;
		}

		nob = min(size, iov->iov_buf_len - vbuf->vb_iov_off);
		if (addr != NULL) {
			memcpy(iov->iov_buf + vbuf->vb_iov_off, addr, nob);
			addr += nob;
		} /* otherwise it's a hole */

		vbuf->vb_iov_off += nob;
		if (vbuf->vb_iov_off == nob) /* the first population */
			vbuf->vb_sgl.sg_nr.num_out++;

		iov->iov_len = vbuf->vb_iov_off;
		if (iov->iov_len == iov->iov_buf_len) {
			/* consumed an iov, move to the next */
			vbuf->vb_iov_off = 0;
			vbuf->vb_iov_at++;
		}

		size -= nob;
		if (size == 0)
			return 0;
	}
	D_DEBUG(DB_IO, "Consumed all iovs, "DF_U64" bytes left\n", size);
	return -1;
}

/**
 * This function copies @size bytes from @vbuf::vb_sgl to destination @addr.
 * NB: this function is for non-zc only.
 */
static int
vbuf_update(struct vec_io_buf *vbuf, void *addr, daos_size_t size)
{
	D_ASSERT(!vbuf_empty(vbuf));

	while (!vbuf_exhausted(vbuf)) {
		daos_iov_t	*iov;
		daos_size_t	 nob;

		iov = &vbuf->vb_sgl.sg_iovs[vbuf->vb_iov_at]; /* current iov */
		if (iov->iov_len <= vbuf->vb_iov_off) {
			D_ERROR("Invalid iov[%d] "DF_U64"/"DF_U64"\n",
				vbuf->vb_iov_at, vbuf->vb_iov_off,
				iov->iov_len);
			return -1;
		}

		nob = min(size, iov->iov_len - vbuf->vb_iov_off);
		memcpy(addr, iov->iov_buf + vbuf->vb_iov_off, nob);

		vbuf->vb_iov_off += nob;
		if (vbuf->vb_iov_off == iov->iov_len) {
			/* consumed an iov, move to the next */
			vbuf->vb_iov_off = 0;
			vbuf->vb_iov_at++;
		}

		addr += nob;
		size -= nob;
		if (size == 0)
			return 0;
	}
	D_DEBUG(DB_IO, "Consumed all iovs, "DF_U64" bytes left\n", size);
	return -1;
}

/** fill/consume vbuf for zero-copy */
static void
vbuf_zcopy(struct vec_io_buf *vbuf, daos_iov_t *iov)
{
	int	at = vbuf->vb_iov_at;

	D_ASSERT(vbuf->vb_iov_off == 0);

	if (vbuf->vb_mmids == NULL) { /* zc-fetch */
		/* return the data address for rdma in upper level stack */
		vbuf->vb_sgl.sg_iovs[at] = *iov;
		vbuf->vb_sgl.sg_nr.num_out++;
	} /* else: do nothing for zc-update */

	vbuf->vb_iov_at++;
}

static void
vos_empty_sgl(daos_sg_list_t *sgl)
{
	int	i;

	for (i = 0; i < sgl->sg_nr.num; i++)
		sgl->sg_iovs[i].iov_len = 0;
}

static void
vos_empty_viod(daos_vec_iod_t *viod)
{
	int	i;

	for (i = 0; i < viod->vd_nr; i++)
		viod->vd_recxs[i].rx_rsize = 0;
}

static struct vos_obj_iter *
vos_iter2oiter(struct vos_iterator *iter)
{
	return container_of(iter, struct vos_obj_iter, it_iter);
}

struct vos_obj_iter*
vos_hdl2oiter(daos_handle_t hdl)
{
	return vos_iter2oiter(vos_hdl2iter(hdl));
}

/**
 * @defgroup vos_tree_helper Helper functions for tree operations
 * @{
 */

/**
 * store a bundle of parameters into a iovec, which is going to be passed
 * into dbtree operations as a compound key.
 */
void
tree_key_bundle2iov(struct vos_key_bundle *kbund, daos_iov_t *iov)
{
	memset(kbund, 0, sizeof(*kbund));
	daos_iov_set(iov, kbund, sizeof(*kbund));
}

/**
 * store a bundle of parameters into a iovec, which is going to be passed
 * into dbtree operations as a compound value (data buffer address, or ZC
 * buffer mmid, checksum etc).
 */
static void
tree_rec_bundle2iov(struct vos_rec_bundle *rbund, daos_iov_t *iov)
{
	memset(rbund, 0, sizeof(*rbund));
	daos_iov_set(iov, rbund, sizeof(*rbund));
}

/**
 * Prepare the record/recx tree, both of them are btree for now, although recx
 * tree could be rtree in the future.
 *
 * vector tree	: all akeys under the same dkey
 * recx tree	: all record extents under the same akey
 */
static int
tree_prepare(struct vos_obj_ref *oref, daos_handle_t parent_toh,
	     daos_key_t *key, bool read_only, daos_handle_t *toh)
{
	daos_csum_buf_t		 csum;
	struct vos_key_bundle	 kbund;
	struct vos_rec_bundle	 rbund;
	daos_iov_t		 kiov;
	daos_iov_t		 riov;
	int			 rc;

	tree_key_bundle2iov(&kbund, &kiov);
	kbund.kb_key	= key;

	tree_rec_bundle2iov(&rbund, &riov);
	rbund.rb_csum	= &csum;
	rbund.rb_mmid	= UMMID_NULL;
	memset(&csum, 0, sizeof(csum));

	/* NB: In order to avoid complexities of passing parameters to the
	 * multi-nested tree, tree operations are not nested, instead:
	 *
	 * - In the case of fetch, we load the subtree root stored in the
	 *   parent tree leaf.
	 * - In the case of update/insert, we call dbtree_update() which can
	 *   create and return the root for the subtree.
	 */
	if (read_only) {
		daos_key_t tmp;

		daos_iov_set(&tmp, NULL, 0);
		rbund.rb_iov = &tmp;
		rc = dbtree_lookup(parent_toh, &kiov, &riov);
		if (rc != 0) {
			D_DEBUG(DF_VOS1, "Cannot find key: %d\n", rc);
			return rc;
		}
	} else {
		rbund.rb_iov = key;
		rc = dbtree_update(parent_toh, &kiov, &riov);
		if (rc != 0) {
			D_DEBUG(DF_VOS1, "Cannot add key: %d\n", rc);
			return rc;
		}
	}

	D_ASSERT(rbund.rb_btr != NULL);
	D_DEBUG(DF_VOS2, "Open subtree\n");

	rc = dbtree_open_inplace(rbund.rb_btr, vos_oref2uma(oref), toh);
	if (rc != 0) {
		D_DEBUG(DF_VOS1, "Failed to open subtree %d: %d\n",
			rbund.rb_btr->tr_class, rc);
	}
	return rc;
}

/** close the record extent tree */
static void
tree_release(daos_handle_t toh)
{
	int	rc;

	rc = dbtree_close(toh);
	D_ASSERT(rc == 0 || rc == -DER_NO_HDL);
}

/** fetch data or data address of a recx from the recx tree */
static int
tree_recx_fetch(daos_handle_t toh, daos_epoch_range_t *epr, daos_recx_t *recx,
		daos_iov_t *iov, daos_csum_buf_t *csum)
{
	struct vos_key_bundle	 kbund;
	struct vos_rec_bundle	 rbund;
	daos_iov_t		 kiov;
	daos_iov_t		 riov;

	tree_key_bundle2iov(&kbund, &kiov);
	kbund.kb_idx	= recx->rx_idx;
	kbund.kb_epr	= epr;

	tree_rec_bundle2iov(&rbund, &riov);
	rbund.rb_iov	= iov;
	rbund.rb_csum	= csum;
	rbund.rb_recx	= recx;

	return dbtree_fetch(toh, BTR_PROBE_LE, &kiov, &kiov, &riov);
}

/**
 * update data for a record extent, or install zero-copied mmid into the
 * record extent tree (if @mmid is not UMMID_NULL).
 */
static int
tree_recx_update(daos_handle_t toh, daos_epoch_range_t *epr,
		 uuid_t cookie, daos_recx_t *recx, daos_iov_t *iov,
		 daos_csum_buf_t *csum, umem_id_t *mmid)
{
	struct vos_key_bundle	kbund;
	struct vos_rec_bundle	rbund;
	daos_iov_t		kiov;
	daos_iov_t		riov;
	int			rc;

	tree_key_bundle2iov(&kbund, &kiov);
	kbund.kb_idx	= recx->rx_idx;
	kbund.kb_epr	= epr;

	tree_rec_bundle2iov(&rbund, &riov);
	rbund.rb_csum	= csum;
	rbund.rb_iov	= iov;
	rbund.rb_recx	= recx;
	rbund.rb_mmid	= mmid != NULL ? *mmid : UMMID_NULL;
	uuid_copy(rbund.rb_cookie, cookie);

	rc = dbtree_update(toh, &kiov, &riov);
	if (mmid != NULL && rc == 0)
		*mmid = rbund.rb_mmid;

	return rc;
}

static int
tree_iter_probe(struct vos_obj_iter *oiter, daos_hash_out_t *anchor)
{
	dbtree_probe_opc_t	opc;

	opc = anchor == NULL ? BTR_PROBE_FIRST : BTR_PROBE_GE;
	return dbtree_iter_probe(oiter->it_hdl, opc, NULL, anchor);
}

static int
tree_iter_fetch(struct vos_obj_iter *oiter, vos_iter_entry_t *it_entry,
		daos_hash_out_t *anchor)
{
	struct vos_key_bundle	kbund;
	struct vos_rec_bundle	rbund;
	daos_iov_t		kiov;
	daos_iov_t		riov;
	daos_csum_buf_t		csum;

	tree_key_bundle2iov(&kbund, &kiov);
	tree_rec_bundle2iov(&rbund, &riov);

	rbund.rb_iov	= &it_entry->ie_key;
	rbund.rb_csum	= &csum;

	daos_iov_set(rbund.rb_iov, NULL, 0); /* no copy */
	daos_csum_set(rbund.rb_csum, NULL, 0);

	return dbtree_iter_fetch(oiter->it_hdl, &kiov, &riov, anchor);
}

static int
tree_iter_next(struct vos_obj_iter *oiter)
{
	return dbtree_iter_next(oiter->it_hdl);
}

/**
 * @} vos_tree_helper
 */

/**
 * @defgroup vos_obj_io_func functions for object regular I/O
 * @{
 */

/**
 * Fetch a record extent.
 *
 * In non-zc mode, this function will fill data and consume iovs of @vbuf.
 * In zc mode, this function will return recx data addresses to iovs of @vbuf.
 */
static int
vos_recx_fetch(daos_handle_t toh, daos_epoch_range_t *epr, daos_recx_t *recx,
	       struct vec_io_buf *vbuf)
{
	daos_csum_buf_t	 csum;
	daos_size_t	 rsize;
	int		 holes;
	int		 i;
	int		 rc;
	bool		 is_zc;

	daos_csum_set(&csum, NULL, 0); /* no checksum for now */
	if (vbuf_empty(vbuf)) { /* fetch record size only */
		is_zc = false;
		rsize = 0;
	} else {
		/* - zc fetch is supposed to provide empty iovs (without sink
		 *   buffers), so we can return the data addresses for rdma.
		 * - non-zc fetch is supposed to provide sink buffers so we can
		 *   copy data into those buffers.
		 */
		is_zc = vbuf->vb_sgl.sg_iovs[0].iov_buf == NULL;
		rsize = recx->rx_rsize;
	}

	for (i = holes = 0; i < recx->rx_nr; i++) {
		daos_epoch_range_t	epr_tmp = *epr;
		daos_recx_t		recx_tmp;
		daos_iov_t		iov;

		recx_tmp.rx_rsize = recx->rx_rsize;
		recx_tmp.rx_idx	  = recx->rx_idx + i;
		recx_tmp.rx_nr	  = 1; /* btree has one record per index */

		if (!vbuf_empty(vbuf) && vbuf_exhausted(vbuf)) {
			D_DEBUG(DB_IO, "Invalid I/O parameters: %d/%d\n",
				vbuf->vb_iov_at, vbuf->vb_sgl.sg_nr.num);
			D_GOTO(failed, rc = -DER_IO_INVAL);
		}

		/* NB: fetch the address from the tree, either do data copy at
		 * here, or return the address to caller who will do rdma.
		 */
		daos_iov_set(&iov, NULL, 0);
		rc = tree_recx_fetch(toh, &epr_tmp, &recx_tmp, &iov, &csum);
		if (rc == -DER_NONEXIST) {
			recx_tmp.rx_idx++; /* fake a mismatch */
			rc = 0;

		} else if (rc != 0) {
			D_DEBUG(DB_IO, "Failed to fetch index "DF_U64": %d\n",
				recx->rx_idx, rc);
			D_GOTO(failed, rc);
		}

		/* If we store index and epoch in the same btree, then
		 * BTR_PROBE_LE is not enough, we also need to check if it
		 * is the same index.
		 */
		if (recx_tmp.rx_idx != recx->rx_idx + i) {
			D_DEBUG(DB_IO,
				"Mismatched idx "DF_U64"/"DF_U64", no data\n",
				recx_tmp.rx_idx, recx->rx_idx + i);
			recx_tmp.rx_rsize = 0; /* mark it as a hole */
			iov.iov_len = rsize;
			iov.iov_buf = NULL;
			iov.iov_buf_len = 0;
		}

		if (recx_tmp.rx_rsize == 0) { /* punched or no data */
			holes++;
		} else {
			if (rsize == 0) {
				/* unknown yet, save it */
				rsize = recx_tmp.rx_rsize;
				/* Check if there holes in the begining */
				if (holes > 0 && is_zc) {
					vbuf->vb_sgl.sg_iovs[0].iov_len =
								holes * rsize;
					D_DEBUG(DB_IO, "compensate holes %d "
						"rsize "DF_U64"\n", holes,
						rsize);
					holes = 0;
				}
			}
			if (rsize != recx_tmp.rx_rsize) {
				D_ERROR("Record sizes of all indices must be "
					"the same: "DF_U64"/"DF_U64"\n",
					rsize, recx_tmp.rx_rsize);
				D_GOTO(failed, rc = -DER_IO_INVAL);
			}

			if (vbuf_empty(vbuf)) /* only fetch the record size */
				D_GOTO(out, rc = 0);
		}

		if (is_zc) {
			/* ZC has one iov per record index, we just store the
			 * record data address to the iov, so caller can rdma.
			 *
			 * NB: iov::iov_buf could be NULL (hole), caller should
			 * check and handle it.
			 */
			vbuf_zcopy(vbuf, &iov);
			continue;
		}

		/* else: non zero-copy */
		if (recx_tmp.rx_rsize == 0)
			continue;

		if (holes != 0) {
			rc = vbuf_fetch(vbuf, NULL, holes * rsize);
			if (rc)
				D_GOTO(failed, rc = -DER_IO_INVAL);
			holes = 0;
		}

		/* copy data from the storage address (iov) to vbuf */
		rc = vbuf_fetch(vbuf, iov.iov_buf, iov.iov_len);
		if (rc)
			D_GOTO(failed, rc = -DER_IO_INVAL);
	}

	if (holes == recx->rx_nr) /* nothing but holes */
		rsize = 0; /* overwrite the rsize from caller */

	if (is_zc) /* done, caller should take care of holes */
		D_GOTO(out, rc = 0);

	if (holes == 0) /* no trailing holes, done */
		D_GOTO(out, rc = 0);

	if (rsize == 0) {
		vos_empty_sgl(&vbuf->vb_sgl);
		D_GOTO(out, rc = 0);
	}
	/* else: has data and some trailing holes... */

	rc = vbuf_fetch(vbuf, NULL, holes * rsize);
	if (rc)
		D_GOTO(failed, rc = -DER_IO_INVAL);
out:
	recx->rx_rsize = rsize;
	return 0;
failed:
	D_DEBUG(DB_IO, "Failed to fetch recx: %d\n", rc);
	return rc;
}

/** fetch a set of record extents from the specified vector. */
static int
vos_vec_fetch(struct vos_obj_ref *oref, daos_epoch_t epoch,
	      daos_handle_t vec_toh, daos_vec_iod_t *viod,
	      struct vec_io_buf *vbuf)
{
	daos_epoch_range_t	eprange;
	daos_handle_t		toh;
	int			i;
	int			rc;

	rc = tree_prepare(oref, vec_toh, &viod->vd_name, true, &toh);
	if (rc == -DER_NONEXIST) {
		D_DEBUG(DF_VOS2, "nonexistent record\n");
		vos_empty_sgl(&vbuf->vb_sgl);
		vos_empty_viod(viod);
		return 0;

	} else if (rc != 0) {
		D_DEBUG(DB_IO, "Failed to prepare tree: %d\n", rc);
		return rc;
	}

	eprange.epr_lo = epoch;
	eprange.epr_hi = DAOS_EPOCH_MAX;

	for (i = 0; i < viod->vd_nr; i++) {
		daos_epoch_range_t *epr;

		epr = viod->vd_eprs ?  &viod->vd_eprs[i] : &eprange;
		rc = vos_recx_fetch(toh, epr, &viod->vd_recxs[i], vbuf);
		if (rc != 0) {
			D_DEBUG(DB_IO, "Failed to fetch index %d: %d\n", i, rc);
			D_GOTO(failed, rc);
		}
	}
 failed:
	tree_release(toh);
	return rc;
}

/** fetch a set of records under the same dkey */
static int
vos_dkey_fetch(struct vos_obj_ref *oref, daos_epoch_t epoch, daos_key_t *dkey,
	       unsigned int viod_nr, daos_vec_iod_t *viods,
	       daos_sg_list_t *sgls, struct vos_zc_context *zcc)
{
	daos_handle_t	toh;
	int		i;
	int		rc;

	rc = vos_obj_tree_init(oref);
	if (rc != 0)
		return rc;

	rc = tree_prepare(oref, oref->or_toh, dkey, true, &toh);
	if (rc == -DER_NONEXIST) {
		for (i = 0; i < viod_nr; i++) {
			vos_empty_viod(&viods[i]);
			if (sgls != NULL)
				vos_empty_sgl(&sgls[i]);
		}
		D_DEBUG(DB_IO, "nonexistent dkey\n");
		return 0;

	} else if (rc != 0) {
		D_DEBUG(DB_IO, "Failed to prepare subtree: %d\n", rc);
		return rc;
	}

	for (i = 0; i < viod_nr; i++) {
		struct vec_io_buf vbuf;

		memset(&vbuf, 0, sizeof(vbuf));
		if (sgls) {
			vbuf.vb_sgl = sgls[i];
			vbuf.vb_sgl.sg_nr.num_out = 0;
		}

		rc = vos_vec_fetch(oref, epoch, toh, &viods[i],
				   (sgls || !zcc) ? &vbuf : &zcc->zc_vbufs[i]);
		if (rc != 0)
			D_GOTO(out, rc);

		if (sgls)
			sgls[i].sg_nr = vbuf.vb_sgl.sg_nr;
	}
 out:
	tree_release(toh);
	return rc;
}

/**
 * Fetch an array of vectors from the specified object.
 */
int
vos_obj_fetch(daos_handle_t coh, daos_unit_oid_t oid, daos_epoch_t epoch,
	      daos_key_t *dkey, unsigned int viod_nr, daos_vec_iod_t *viods,
	      daos_sg_list_t *sgls)
{
	struct vos_obj_ref *oref;
	int		    rc;

	D_DEBUG(DB_IO, "Fetch "DF_UOID", desc_nr %d, epoch "DF_U64"\n",
		DP_UOID(oid), viod_nr, epoch);

	rc = vos_obj_ref_hold(vos_obj_cache_current(), coh, oid, &oref);
	if (rc != 0)
		return rc;

	if (vos_obj_is_new(oref->or_obj)) {
		int	i;

		D_DEBUG(DB_IO, "New object, nothing to fetch\n");
		for (i = 0; i < viod_nr; i++) {
			vos_empty_viod(&viods[i]);
			if (sgls != NULL)
				vos_empty_sgl(&sgls[i]);
		}
		D_GOTO(out, rc = 0);
	}

	rc = vos_dkey_fetch(oref, epoch, dkey, viod_nr, viods, sgls, NULL);
 out:
	vos_obj_ref_release(vos_obj_cache_current(), oref);
	return rc;
}

/**
 * Update a record extent.
 * See comment of vos_recx_fetch for explanation of @off_p.
 */
static int
vos_recx_update(daos_handle_t toh, daos_epoch_range_t *epr, uuid_t cookie,
		daos_recx_t *recx, struct vec_io_buf *vbuf)
{
	daos_csum_buf_t	    csum;
	int		    i;
	int		    rc;

	if (epr->epr_hi != DAOS_EPOCH_MAX) {
		D_CRIT("Not ready for cache tiering...\n");
		D_GOTO(out, rc = -DER_IO_INVAL);
	}

	for (i = 0; i < recx->rx_nr; i++) {
		daos_recx_t	recx_tmp;
		daos_iov_t	iov;

		recx_tmp.rx_rsize = recx->rx_rsize;
		recx_tmp.rx_idx	  = recx->rx_idx + i;
		recx_tmp.rx_nr	  = 1; /* btree has one record per index */

		daos_csum_set(&csum, NULL, 0); /* no checksum for now */
		daos_iov_set(&iov, NULL, recx->rx_rsize);

		rc = tree_recx_update(toh, epr, cookie, &recx_tmp, &iov, &csum,
				vbuf->vb_mmids ? &vbuf->vb_mmids[i] : NULL);
		if (rc != 0) {
			D_DEBUG(DB_IO, "Failed to update subtree: %d\n", rc);
			D_GOTO(out, rc);
		}

		if (vbuf->vb_mmids != NULL) { /* zero-copy */
			/* NB: punch also has corresponding mmid and iov */
			vbuf_zcopy(vbuf, &iov);
			continue;
		}

		if (recx->rx_rsize == 0) /* punched */
			continue;

		D_ASSERT(iov.iov_buf != NULL);
		rc = vbuf_update(vbuf, iov.iov_buf, iov.iov_len);
		if (rc != 0)
			D_GOTO(out, rc = -DER_IO_INVAL);
	}
out:
	return rc;
}

/** update a set of record extents (recx) under the same akey */
static int
vos_vec_update(struct vos_obj_ref *oref, daos_epoch_t epoch,
	       uuid_t cookie, daos_handle_t vec_toh, daos_vec_iod_t *viod,
	       struct vec_io_buf *vbuf)
{
	daos_epoch_range_t	 eprange;
	daos_off_t		 off;
	daos_handle_t		 toh;
	int			 i;
	int			 nr;
	int			 rc;

	rc = tree_prepare(oref, vec_toh, &viod->vd_name, false, &toh);
	if (rc != 0)
		return rc;

	eprange.epr_lo = epoch;
	eprange.epr_hi = DAOS_EPOCH_MAX;

	for (i = nr = off = 0; i < viod->vd_nr; i++) {
		daos_epoch_range_t *epr	= &eprange;

		if (viod->vd_eprs != NULL)
			epr = &viod->vd_eprs[i];

		rc = vos_recx_update(toh, epr, cookie, &viod->vd_recxs[i],
				     vbuf);
		if (rc != 0)
			D_GOTO(failed, rc);
	}
 failed:
	tree_release(toh);
	return rc;
}

static int
vos_dkey_update(struct vos_obj_ref *oref, daos_epoch_t epoch, uuid_t cookie,
		daos_key_t *dkey, unsigned int viod_nr, daos_vec_iod_t *viods,
		daos_sg_list_t *sgls, struct vos_zc_context *zcc)
{
	daos_handle_t	toh;
	daos_handle_t	cookie_hdl;
	int		i;
	int		rc;

	rc = vos_obj_tree_init(oref);
	if (rc != 0)
		return rc;

	rc = tree_prepare(oref, oref->or_toh, dkey, false, &toh);
	if (rc != 0)
		return rc;

	for (i = 0; i < viod_nr; i++) {
		struct vec_io_buf vbuf;

		memset(&vbuf, 0, sizeof(vbuf));
		if (sgls)
			vbuf.vb_sgl = sgls[i];

		rc = vos_vec_update(oref, epoch, cookie, toh, &viods[i],
				   (sgls || !zcc) ? &vbuf : &zcc->zc_vbufs[i]);
		if (rc != 0)
			D_GOTO(out, rc);
	}

	/** If dkey update is successful update the cookie tree */
	cookie_hdl = vos_oref2cookie_hdl(oref);
	rc = vos_cookie_find_update(cookie_hdl, cookie, epoch, true, NULL);
	if (rc) {
		D_ERROR("Failed to record cookie: %d\n", rc);
		D_GOTO(out, rc);
	}
 out:
	tree_release(toh);
	return rc;
}

/**
 * Update an array of vectors for the specified object.
 */
int
vos_obj_update(daos_handle_t coh, daos_unit_oid_t oid, daos_epoch_t epoch,
	       uuid_t cookie, daos_key_t *dkey, unsigned int viod_nr,
	       daos_vec_iod_t *viods, daos_sg_list_t *sgls)
{
	struct vos_obj_ref	*oref;
	PMEMobjpool		*pop;
	int			rc;

	D_DEBUG(DF_VOS2, "Update "DF_UOID", desc_nr %d, cookie "DF_UUID" epoch "
		DF_U64"\n", DP_UOID(oid), viod_nr, DP_UUID(cookie), epoch);

	rc = vos_obj_ref_hold(vos_obj_cache_current(), coh, oid, &oref);
	if (rc != 0)
		return rc;

	pop = vos_oref2pop(oref);
	TX_BEGIN(pop) {
		rc = vos_dkey_update(oref, epoch, cookie, dkey, viod_nr,
				     viods, sgls, NULL);
	} TX_ONABORT {
		rc = umem_tx_errno(rc);
		D_DEBUG(DF_VOS1, "Failed to update object: %d\n", rc);
	} TX_END

	vos_obj_ref_release(vos_obj_cache_current(), oref);
	return rc;
}

/**
 * @} vos_obj_io_func
 */

/*
 * @defgroup vos_obj_zio_func Zero-copy I/O functions
 * @{
 */

/** convert I/O handle to ZC context */
static struct vos_zc_context *
vos_ioh2zcc(daos_handle_t ioh)
{
	return (struct vos_zc_context *)ioh.cookie;
}

/** convert ZC context to I/O handle */
static daos_handle_t
vos_zcc2ioh(struct vos_zc_context *zcc)
{
	daos_handle_t ioh;

	ioh.cookie = (uint64_t)zcc;
	return ioh;
}

/**
 * Create a zero-copy I/O context. This context includes buffers pointers
 * to return to caller which can proceed the zero-copy I/O.
 */
static int
vos_zcc_create(daos_handle_t coh, daos_unit_oid_t oid, daos_epoch_t epoch,
	       unsigned int viod_nr, daos_vec_iod_t *viods,
	       struct vos_zc_context **zcc_pp)
{
	struct vos_zc_context *zcc;
	int		       rc;

	D_ALLOC_PTR(zcc);
	if (zcc == NULL)
		return -DER_NOMEM;

	rc = vos_obj_ref_hold(vos_obj_cache_current(), coh, oid, &zcc->zc_oref);
	if (rc != 0)
		D_GOTO(failed, rc);

	zcc->zc_vec_nr = viod_nr;
	D_ALLOC(zcc->zc_vbufs, zcc->zc_vec_nr * sizeof(*zcc->zc_vbufs));
	if (zcc->zc_vbufs == NULL)
		D_GOTO(failed, rc = -DER_NOMEM);

	zcc->zc_epoch = epoch;
	*zcc_pp = zcc;
	return 0;
 failed:
	vos_zcc_destroy(zcc, rc);
	return rc;
}

/**
 * Free zero-copy buffers for @zcc, it returns false if it is called without
 * transactoin, but @zcc has pmem buffers. Otherwise it returns true.
 */
static int
vos_zcc_free_vbuf(struct vos_zc_context *zcc, bool has_tx)
{
	struct vec_io_buf *vbuf;
	int		   i;

	for (vbuf = &zcc->zc_vbufs[0];
	     vbuf < &zcc->zc_vbufs[zcc->zc_vec_nr]; vbuf++) {

		daos_sgl_fini(&vbuf->vb_sgl, false);
		if (vbuf->vb_mmids == NULL)
			continue;

		for (i = 0; i < vbuf->vb_mmid_nr; i++) {
			umem_id_t mmid = vbuf->vb_mmids[i];

			if (UMMID_IS_NULL(mmid))
				continue;

			if (!has_tx)
				return false;

			umem_free(vos_oref2umm(zcc->zc_oref), mmid);
			vbuf->vb_mmids[i] = UMMID_NULL;
		}

		D_FREE(vbuf->vb_mmids,
		       vbuf->vb_mmid_nr * sizeof(*vbuf->vb_mmids));
	}

	D_FREE(zcc->zc_vbufs, zcc->zc_vec_nr * sizeof(*zcc->zc_vbufs));
	return true;
}

/** free zero-copy I/O context */
static void
vos_zcc_destroy(struct vos_zc_context *zcc, int err)
{
	if (zcc->zc_vbufs != NULL) {
		PMEMobjpool	*pop;
		bool		 done;

		done = vos_zcc_free_vbuf(zcc, false);
		if (!done) {
			D_ASSERT(zcc->zc_oref != NULL);
			pop = vos_oref2pop(zcc->zc_oref);

			TX_BEGIN(pop) {
				done = vos_zcc_free_vbuf(zcc, true);
				D_ASSERT(done);

			} TX_ONABORT {
				err = umem_tx_errno(err);
				D_DEBUG(DF_VOS1,
					"Failed to free zcbuf: %d\n", err);
			} TX_END
		}
	}

	if (zcc->zc_oref)
		vos_obj_ref_release(vos_obj_cache_current(), zcc->zc_oref);

	D_FREE_PTR(zcc);
}

static int
vos_vec_zc_fetch_begin(struct vos_obj_ref *oref, daos_epoch_t epoch,
		       daos_key_t *dkey, unsigned int viod_nr,
		       daos_vec_iod_t *viods, struct vos_zc_context *zcc)
{
	int	i;
	int	rc;

	/* NB: no cleanup in this function, vos_obj_zc_fetch_end will release
	 * all the resources.
	 */
	rc = vos_obj_tree_init(oref);
	if (rc != 0)
		return rc;

	for (i = 0; i < viod_nr; i++) {
		struct vec_io_buf *vbuf = &zcc->zc_vbufs[i];
		int		   j;
		int		   nr;

		for (j = nr = 0; j < viods[i].vd_nr; j++)
			nr += viods[i].vd_recxs[j].rx_nr;

		rc = daos_sgl_init(&vbuf->vb_sgl, nr);
		if (rc != 0) {
			D_DEBUG(DF_VOS1,
				"Failed to create sgl for vector %d\n", i);
			return rc;
		}
	}

	rc = vos_dkey_fetch(oref, epoch, dkey, viod_nr, viods, NULL, zcc);
	if (rc != 0) {
		D_DEBUG(DF_VOS1,
			"Failed to get ZC buffer for vector %d\n", i);
		return rc;
	}

	return 0;
}

/**
 * Fetch an array of vectors from the specified object in zero-copy mode,
 * this function will create and return scatter/gather list which can address
 * vector data stored in pmem.
 */
int
vos_obj_zc_fetch_begin(daos_handle_t coh, daos_unit_oid_t oid,
		       daos_epoch_t epoch, daos_key_t *dkey,
		       unsigned int viod_nr, daos_vec_iod_t *viods,
		       daos_handle_t *ioh)
{
	struct vos_zc_context *zcc;
	int		       rc;

	rc = vos_zcc_create(coh, oid, epoch, viod_nr, viods, &zcc);
	if (rc != 0)
		return rc;

	rc = vos_vec_zc_fetch_begin(zcc->zc_oref, epoch, dkey, viod_nr,
				    viods, zcc);
	if (rc != 0)
		goto failed;

	D_DEBUG(DF_VOS2, "Prepared zcbufs for fetching %d vectors\n", viod_nr);
	*ioh = vos_zcc2ioh(zcc);
	return 0;
 failed:
	vos_obj_zc_fetch_end(vos_zcc2ioh(zcc), dkey, viod_nr, viods, rc);
	return rc;
}

/**
 * Finish the current zero-copy fetch operation and release responding
 * resources.
 */
int
vos_obj_zc_fetch_end(daos_handle_t ioh, daos_key_t *dkey, unsigned int viod_nr,
		     daos_vec_iod_t *viods, int err)
{
	struct vos_zc_context	*zcc = vos_ioh2zcc(ioh);

	D_ASSERT(!zcc->zc_is_update);
	vos_zcc_destroy(zcc, err);
	return err;
}

static daos_size_t
vos_recx2irec_size(daos_recx_t *recx, daos_csum_buf_t *csum)
{
	struct vos_rec_bundle	rbund;

	rbund.rb_csum	= csum;
	rbund.rb_recx	= recx;
	return vos_irec_size(&rbund);
}

/**
 * Prepare pmem buffers for the zero-copy update.
 *
 * NB: no cleanup in this function, vos_obj_zc_update_end will release all the
 * resources.
 */
static int
vos_rec_zc_update_begin(struct vos_obj_ref *oref, daos_vec_iod_t *viod,
			struct vec_io_buf *vbuf)
{
	int	i;
	int	nr;
	int	rc;

	for (i = nr = 0; i < viod->vd_nr; i++)
		nr += viod->vd_recxs[i].rx_nr;

	vbuf->vb_mmid_nr = nr;
	D_ALLOC(vbuf->vb_mmids, nr * sizeof(*vbuf->vb_mmids));
	if (vbuf->vb_mmids == NULL)
		return -DER_NOMEM;

	rc = daos_sgl_init(&vbuf->vb_sgl, nr);
	if (rc != 0)
		return -DER_NOMEM;

	for (i = nr = 0; i < viod->vd_nr; i++) {
		daos_recx_t	recx = viod->vd_recxs[i];
		uint64_t	irec_size;
		int		j;

		recx.rx_nr = 1;
		irec_size = vos_recx2irec_size(&recx, NULL);

		for (j = 0; j < viod->vd_recxs[i].rx_nr; j++, nr++) {
			struct vos_irec	*irec;
			umem_id_t	 mmid;

			mmid = umem_alloc(vos_oref2umm(oref), irec_size);
			if (UMMID_IS_NULL(mmid))
				return -DER_NOMEM;

			vbuf->vb_mmids[nr] = mmid;
			/* return the pmem address, so upper layer stack can do
			 * RMA update for the record.
			 */
			irec = (struct vos_irec *)
				umem_id2ptr(vos_oref2umm(oref), mmid);

			irec->ir_cs_size = 0;
			irec->ir_cs_type = 0;
			daos_iov_set(&vbuf->vb_sgl.sg_iovs[nr],
				     vos_irec2data(irec), recx.rx_rsize);
			vbuf->vb_sgl.sg_nr.num_out++;
		}
	}
	return 0;
}

static int
vos_vec_zc_update_begin(struct vos_obj_ref *oref, unsigned int viod_nr,
			daos_vec_iod_t *viods, struct vos_zc_context *zcc)
{
	int	i;
	int	rc;

	D_ASSERT(oref == zcc->zc_oref);
	for (i = 0; i < viod_nr; i++) {
		rc = vos_rec_zc_update_begin(oref, &viods[i],
					     &zcc->zc_vbufs[i]);
		if (rc != 0)
			return rc;
	}
	return 0;
}

/**
 * Create zero-copy buffers for the vectors to be updated. After storing data
 * in the returned ZC buffer, user should call vos_obj_zc_update_end() to
 * create indices for these data buffers.
 */
int
vos_obj_zc_update_begin(daos_handle_t coh, daos_unit_oid_t oid,
			daos_epoch_t epoch, daos_key_t *dkey,
			unsigned int viod_nr, daos_vec_iod_t *viods,
			daos_handle_t *ioh)
{
	struct vos_zc_context	*zcc;
	PMEMobjpool		*pop;
	int			 rc;

	rc = vos_zcc_create(coh, oid, epoch, viod_nr, viods, &zcc);
	if (rc != 0)
		return rc;

	zcc->zc_is_update = true;
	pop = vos_oref2pop(zcc->zc_oref);

	TX_BEGIN(pop) {
		rc = vos_vec_zc_update_begin(zcc->zc_oref, viod_nr, viods, zcc);
	} TX_ONABORT {
		rc = umem_tx_errno(rc);
		D_DEBUG(DF_VOS1, "Failed to update object: %d\n", rc);
	} TX_END

	if (rc != 0)
		goto failed;

	D_DEBUG(DF_VOS2, "Prepared zcbufs for updating %d vectors\n", viod_nr);
	*ioh = vos_zcc2ioh(zcc);
	return 0;
 failed:
	vos_obj_zc_update_end(vos_zcc2ioh(zcc), 0, dkey, viod_nr,
			      viods, rc);
	return rc;
}

/**
 * Submit the current zero-copy I/O operation to VOS and release responding
 * resources.
 */
int
vos_obj_zc_update_end(daos_handle_t ioh, uuid_t cookie, daos_key_t *dkey,
		      unsigned int viod_nr, daos_vec_iod_t *viods, int err)
{
	struct vos_zc_context	*zcc = vos_ioh2zcc(ioh);
	PMEMobjpool		*pop;

	D_ASSERT(zcc->zc_is_update);
	if (err != 0)
		goto out;

	D_ASSERT(zcc->zc_oref != NULL);
	pop = vos_oref2pop(zcc->zc_oref);

	TX_BEGIN(pop) {
		D_DEBUG(DF_VOS1, "Submit ZC update\n");
		err = vos_dkey_update(zcc->zc_oref, zcc->zc_epoch, cookie,
				      dkey, viod_nr, viods, NULL, zcc);
	} TX_ONABORT {
		err = umem_tx_errno(err);
		D_DEBUG(DF_VOS1, "Failed to submit ZC update: %d\n", err);
	} TX_END
 out:
	vos_zcc_destroy(zcc, err);
	return err;
}

int
vos_obj_zc_vec2sgl(daos_handle_t ioh, unsigned int vec_at,
		   daos_sg_list_t **sgl_pp)
{
	struct vos_zc_context *zcc = vos_ioh2zcc(ioh);

	D_ASSERT(zcc->zc_vbufs != NULL);
	if (vec_at >= zcc->zc_vec_nr) {
		*sgl_pp = NULL;
		D_DEBUG(DF_VOS1, "Invalid vector index %d/%d.\n",
			vec_at, zcc->zc_vec_nr);
		return -DER_NONEXIST;
	}

	*sgl_pp = &zcc->zc_vbufs[vec_at].vb_sgl;
	return 0;
}

/**
 * @} vos_obj_zio_func
 */

/**
 * @defgroup vos_obj_iters VOS object iterators
 * @{
 *
 * - iterate d-key
 * - iterate a-key (vector)
 * - iterate recx
 */

/**
 * Iterator for the d-key tree.
 */
static int
dkey_iter_prepare(struct vos_obj_iter *oiter, daos_key_t *akey)
{
	/* optional condition, d-keys with the provided attribute (a-key) */
	oiter->it_akey = *akey;

	return dbtree_iter_prepare(oiter->it_oref->or_toh, 0, &oiter->it_hdl);
}

/**
 * Check if the current item can match the provided condition (with the
 * giving a-key). If the item can't match the condition, this function
 * traverses the tree until a matched item is found.
 */
static int
dkey_iter_probe_cond(struct vos_obj_iter *oiter)
{
	struct vos_obj_ref *oref = oiter->it_oref;

	if (oiter->it_akey.iov_buf == NULL ||
	    oiter->it_akey.iov_len == 0) /* no condition */
		return 0;

	while (1) {
		vos_iter_entry_t	entry;
		daos_handle_t		toh;
		struct vos_key_bundle	kbund;
		struct vos_rec_bundle	rbund;
		daos_iov_t		kiov;
		daos_iov_t		riov;
		int			rc;

		rc = tree_iter_fetch(oiter, &entry, NULL);
		if (rc != 0)
			return rc;

		rc = tree_prepare(oref, oref->or_toh, &entry.ie_key, true,
				  &toh);
		if (rc != 0) {
			D_DEBUG(DF_VOS1,
				"Failed to load the record tree: %d\n", rc);
			return rc;
		}

		/* check if the a-key exists */
		tree_rec_bundle2iov(&rbund, &riov);
		tree_key_bundle2iov(&kbund, &kiov);
		kbund.kb_key = &oiter->it_akey;

		rc = dbtree_lookup(toh, &kiov, &riov);
		tree_release(toh);
		if (rc == 0) /* match the condition (a-key), done */
			return 0;

		if (rc != -DER_NONEXIST)
			return rc; /* a real failure */

		/* move to the next dkey */
		rc = tree_iter_next(oiter);
		if (rc != 0)
			return rc;
	}
}

static int
dkey_iter_probe(struct vos_obj_iter *oiter, daos_hash_out_t *anchor)
{
	int	rc;

	rc = tree_iter_probe(oiter, anchor);
	if (rc != 0)
		return rc;

	rc = dkey_iter_probe_cond(oiter);
	return rc;
}

static int
dkey_iter_next(struct vos_obj_iter *oiter)
{
	int	rc;

	rc = tree_iter_next(oiter);
	if (rc != 0)
		return rc;

	rc = dkey_iter_probe_cond(oiter);
	return rc;
}

static int
dkey_iter_fetch(struct vos_obj_iter *oiter, vos_iter_entry_t *it_entry,
	       daos_hash_out_t *anchor)
{
	return tree_iter_fetch(oiter, it_entry, anchor);
}

/**
 * Iterator for the vector tree.
 */
static int
vec_iter_prepare(struct vos_obj_iter *oiter, daos_key_t *dkey)
{
	struct vos_obj_ref *oref = oiter->it_oref;
	daos_handle_t	    toh;
	int		    rc;

	rc = tree_prepare(oref, oref->or_toh, dkey, true, &toh);
	if (rc != 0) {
		D_DEBUG(DF_VOS1, "Cannot load the recx tree: %d\n", rc);
		return rc;
	}

	/* see BTR_ITER_EMBEDDED for the details */
	rc = dbtree_iter_prepare(toh, BTR_ITER_EMBEDDED, &oiter->it_hdl);
	tree_release(toh);
	return rc;
}

static int
vec_iter_probe(struct vos_obj_iter *oiter, daos_hash_out_t *anchor)
{
	return tree_iter_probe(oiter, anchor);
}

static int
vec_iter_next(struct vos_obj_iter *oiter)
{
	return tree_iter_next(oiter);
}

static int
vec_iter_fetch(struct vos_obj_iter *oiter, vos_iter_entry_t *it_entry,
	       daos_hash_out_t *anchor)
{
	return tree_iter_fetch(oiter, it_entry, anchor);
}

/**
 * Record extent (recx) iterator
 */

/**
 * Record extent (recx) iterator
 */
static int recx_iter_fetch(struct vos_obj_iter *oiter,
			   vos_iter_entry_t *it_entry,
			   daos_hash_out_t *anchor);
/**
 * Prepare the iterator for the recx tree.
 */
static int
recx_iter_prepare(struct vos_obj_iter *oiter, daos_key_t *dkey,
		  daos_key_t *akey)
{
	struct vos_obj_ref *oref = oiter->it_oref;
	daos_handle_t	    dk_toh;
	daos_handle_t	    ak_toh;
	int		    rc;

	rc = tree_prepare(oref, oref->or_toh, dkey, true, &dk_toh);
	if (rc != 0) {
		D_DEBUG(DF_VOS1, "Cannot load the record tree: %d\n", rc);
		return rc;
	}

	rc = tree_prepare(oref, dk_toh, akey, true, &ak_toh);
	if (rc != 0) {
		D_DEBUG(DF_VOS1, "Cannot load the recx tree: %d\n", rc);
		D_GOTO(failed_0, rc);
	}

	/* see BTR_ITER_EMBEDDED for the details */
	rc = dbtree_iter_prepare(ak_toh, BTR_ITER_EMBEDDED, &oiter->it_hdl);
	if (rc != 0) {
		D_DEBUG(DF_VOS1, "Cannot prepare recx iterator: %d\n", rc);
		D_GOTO(failed_1, rc);
	}

 failed_1:
	tree_release(ak_toh);
 failed_0:
	tree_release(dk_toh);
	return rc;
}

/**
 * Probe the recx based on @opc and conditions in @entry (index and epoch),
 * return the matched one to @entry.
 */
static int
recx_iter_probe_fetch(struct vos_obj_iter *oiter, dbtree_probe_opc_t opc,
		      vos_iter_entry_t *entry)
{
	struct vos_key_bundle	kbund;
	daos_iov_t		kiov;
	int			rc;

	tree_key_bundle2iov(&kbund, &kiov);
	kbund.kb_idx	= entry->ie_recx.rx_idx;
	kbund.kb_epr	= &entry->ie_epr;

	rc = dbtree_iter_probe(oiter->it_hdl, opc, &kiov, NULL);
	if (rc != 0)
		return rc;

	memset(entry, 0, sizeof(*entry));
	rc = recx_iter_fetch(oiter, entry, NULL);
	return rc;
}

/**
 * Find the data that was written before/in the specified epoch of @oiter
 * for the recx in @entry. If this recx has no data for this epoch, then
 * this function will move on to the next recx and repeat this process.
 */
static int
recx_iter_probe_epr(struct vos_obj_iter *oiter, vos_iter_entry_t *entry)
{
	while (1) {
		int	rc;

		if (entry->ie_epr.epr_lo == oiter->it_epr.epr_lo)
			return 0; /* matched */

		switch (oiter->it_epc_expr) {
		default:
			return -DER_INVAL;

		case VOS_IT_EPC_RE:
			if (entry->ie_epr.epr_lo >= oiter->it_epr.epr_lo &&
			    entry->ie_epr.epr_lo <= oiter->it_epr.epr_hi)
				return 0; /** Falls in the range */
			/**
			 * This recx may have data for epoch >
			 * entry->ie_epr.epr_lo
			 */
			if (entry->ie_epr.epr_lo < oiter->it_epr.epr_lo)
				entry->ie_epr.epr_lo = oiter->it_epr.epr_lo;
			else /** epoch not in this index search next epoch */
				entry->ie_epr.epr_lo = DAOS_EPOCH_MAX;

			rc = recx_iter_probe_fetch(oiter, BTR_PROBE_GE, entry);
			break;

		case VOS_IT_EPC_RR:
			if (entry->ie_epr.epr_lo >= oiter->it_epr.epr_lo &&
			    entry->ie_epr.epr_lo <= oiter->it_epr.epr_hi)
				return 0; /** Falls in the range */

			if (entry->ie_epr.epr_lo > oiter->it_epr.epr_hi)
				entry->ie_epr.epr_lo = oiter->it_epr.epr_hi;
			else /** if ent::epr_lo < oiter::epr_lo */
				entry->ie_recx.rx_idx -= 1;

			rc = recx_iter_probe_fetch(oiter, BTR_PROBE_LE, entry);
			break;

		case VOS_IT_EPC_GE:
			if (entry->ie_epr.epr_lo > oiter->it_epr.epr_lo)
				return 0; /* matched */

			/* this recx may have data for the specified epoch, we
			 * can use BTR_PROBE_GE to find out.
			 */
			entry->ie_epr.epr_lo = oiter->it_epr.epr_lo;
			rc = recx_iter_probe_fetch(oiter, BTR_PROBE_GE, entry);
			break;

		case VOS_IT_EPC_LE:
			if (entry->ie_epr.epr_lo < oiter->it_epr.epr_lo) {
				/* this recx has data for the specified epoch,
				 * we can use BTR_PROBE_LE to find the closest
				 * epoch of this recx.
				 */
				entry->ie_epr.epr_lo = oiter->it_epr.epr_lo;
				rc = recx_iter_probe_fetch(oiter, BTR_PROBE_LE,
							   entry);
				return rc;
			}
			/* No matched epoch from in index, try the next index.
			 * NB: Nobody can use DAOS_EPOCH_MAX as an epoch of
			 * update, so using BTR_PROBE_GE & DAOS_EPOCH_MAX can
			 * effectively find the index of the next recx.
			 */
			entry->ie_epr.epr_lo = DAOS_EPOCH_MAX;
			rc = recx_iter_probe_fetch(oiter, BTR_PROBE_GE, entry);
			break;

		case VOS_IT_EPC_EQ:
			if (entry->ie_epr.epr_lo < oiter->it_epr.epr_lo) {
				/* this recx may have data for the specified
				 * epoch, we try to find it by BTR_PROBE_EQ.
				 */
				entry->ie_epr.epr_lo = oiter->it_epr.epr_lo;
				rc = recx_iter_probe_fetch(oiter, BTR_PROBE_EQ,
							   entry);
				if (rc == 0) /* found */
					return 0;

				if (rc != -DER_NONEXIST) /* real failure */
					return rc;
				/* not found, fall through for the next one */
			}
			/* No matched epoch in this index, try the next index.
			 * See the comment for VOS_IT_EPC_LE.
			 */
			entry->ie_epr.epr_lo = DAOS_EPOCH_MAX;
			rc = recx_iter_probe_fetch(oiter, BTR_PROBE_GE, entry);
			break;
		}
		if (rc != 0)
			return rc;
	}
}

static int
recx_iter_probe(struct vos_obj_iter *oiter, daos_hash_out_t *anchor)
{
	vos_iter_entry_t	entry;
	struct vos_key_bundle	kbund;
	daos_iov_t		kiov;
	daos_hash_out_t		tmp;
	int			opc;
	int			rc;

	if (oiter->it_epc_expr == VOS_IT_EPC_RR)
		opc = anchor == NULL ? BTR_PROBE_LAST : BTR_PROBE_LE;
	else
		opc = anchor == NULL ? BTR_PROBE_FIRST : BTR_PROBE_GE;

	rc = dbtree_iter_probe(oiter->it_hdl, opc, NULL, anchor);
	if (rc != 0)
		return rc;

	tree_key_bundle2iov(&kbund, &kiov);
	kbund.kb_epr = &entry.ie_epr;

	memset(&entry, 0, sizeof(entry));
	rc = recx_iter_fetch(oiter, &entry, &tmp);
	if (rc != 0)
		return rc;

	if (anchor != NULL) {
		if (memcmp(anchor, &tmp, sizeof(tmp)) == 0)
			return 0;

		D_DEBUG(DF_VOS2, "Can't find the provided anchor\n");
		/**
		 * the original recx has been merged/discarded, so we need to
		 * call recx_iter_probe_epr() and check if the current record
		 * can match the condition.
		 */
	}

	rc = recx_iter_probe_epr(oiter, &entry);
	return rc;
}

static int
recx_iter_fetch(struct vos_obj_iter *oiter, vos_iter_entry_t *it_entry,
		daos_hash_out_t *anchor)
{
	struct vos_key_bundle	kbund;
	struct vos_rec_bundle	rbund;
	daos_iov_t		kiov;
	daos_iov_t		riov;
	daos_csum_buf_t		csum;
	int			rc;

	tree_key_bundle2iov(&kbund, &kiov);
	kbund.kb_epr	= &it_entry->ie_epr;

	tree_rec_bundle2iov(&rbund, &riov);
	rbund.rb_recx	= &it_entry->ie_recx;
	rbund.rb_iov	= &it_entry->ie_iov;
	rbund.rb_csum	= &csum;

	daos_iov_set(rbund.rb_iov, NULL, 0); /* no data copy */
	daos_csum_set(rbund.rb_csum, NULL, 0);

	rc = dbtree_iter_fetch(oiter->it_hdl, &kiov, &riov, anchor);
	if (rc == 0)
		uuid_copy(it_entry->ie_cookie, rbund.rb_cookie);

	return rc;
}

static int
recx_iter_next(struct vos_obj_iter *oiter)
{
	vos_iter_entry_t entry;
	int		 rc;
	int		 opc;

	memset(&entry, 0, sizeof(entry));
	rc = recx_iter_fetch(oiter, &entry, NULL);
	if (rc != 0)
		return rc;

	if (oiter->it_epc_expr == VOS_IT_EPC_RE)
		entry.ie_epr.epr_lo +=  1;
	else if (oiter->it_epc_expr == VOS_IT_EPC_RR)
		entry.ie_epr.epr_lo -=  1;
	else
		entry.ie_epr.epr_lo = DAOS_EPOCH_MAX;

	opc = (oiter->it_epc_expr == VOS_IT_EPC_RR) ?
		BTR_PROBE_LE : BTR_PROBE_GE;

	rc = recx_iter_probe_fetch(oiter, opc, &entry);
	if (rc != 0)
		return rc;

	rc = recx_iter_probe_epr(oiter, &entry);
	return rc;
}

static int
obj_iter_delete(struct vos_obj_iter *oiter, void *args)
{
	int		rc = 0;
	PMEMobjpool	*pop;

	D_DEBUG(DF_VOS2, "BTR delete called of obj\n");
	pop = vos_oref2pop(oiter->it_oref);

	TX_BEGIN(pop) {
		rc = dbtree_iter_delete(oiter->it_hdl, args);
	} TX_ONABORT {
		rc = umem_tx_errno(rc);
		D_DEBUG(DF_VOS1, "Failed to delete iter entry: %d\n", rc);
	} TX_END

	return rc;
}

/**
 * common functions for iterator.
 */
static int vos_obj_iter_fini(struct vos_iterator *vitr);

/** prepare an object content iterator */
int
vos_obj_iter_prep(vos_iter_type_t type, vos_iter_param_t *param,
		  struct vos_iterator **iter_pp)
{
	struct vos_obj_iter *oiter;
	int		     rc;

	if (param->ip_epr.epr_lo == 0) /* the most recent one */
		param->ip_epr.epr_lo = DAOS_EPOCH_MAX;

	D_ALLOC_PTR(oiter);
	if (oiter == NULL)
		return -DER_NOMEM;

	oiter->it_epr = param->ip_epr;
	rc = vos_obj_ref_hold(vos_obj_cache_current(), param->ip_hdl,
			      param->ip_oid, &oiter->it_oref);
	if (rc != 0)
		D_GOTO(failed, rc);

	if (vos_obj_is_new(oiter->it_oref->or_obj)) {
		D_DEBUG(DF_VOS2, "New object, nothing to iterate\n");
		D_GOTO(failed, rc = -DER_NONEXIST);
	}

	rc = vos_obj_tree_init(oiter->it_oref);
	if (rc != 0)
		goto failed;

	switch (type) {
	default:
		D_ERROR("unknown iterator type %d.\n", type);
		rc = -DER_INVAL;
		break;

	case VOS_ITER_DKEY:
		rc = dkey_iter_prepare(oiter, &param->ip_akey);
		break;

	case VOS_ITER_AKEY:
		rc = vec_iter_prepare(oiter, &param->ip_dkey);
		break;

	case VOS_ITER_RECX:
		oiter->it_epc_expr = param->ip_epc_expr;
		rc = recx_iter_prepare(oiter, &param->ip_dkey, &param->ip_akey);
		break;
	}

	if (rc != 0)
		D_GOTO(failed, rc);

	*iter_pp = &oiter->it_iter;
	return 0;
 failed:
	vos_obj_iter_fini(&oiter->it_iter);
	return rc;
}

/** release the object iterator */
static int
vos_obj_iter_fini(struct vos_iterator *iter)
{
	int			rc =  0;
	struct vos_obj_iter	*oiter = vos_iter2oiter(iter);

	if (!daos_handle_is_inval(oiter->it_hdl)) {
		rc = dbtree_iter_finish(oiter->it_hdl);
		if (rc) {
			D_ERROR("obj_iter_fini failed:%d\n", rc);
			return rc;
		}
	}

	if (oiter->it_oref != NULL)
		vos_obj_ref_release(vos_obj_cache_current(), oiter->it_oref);

	D_FREE_PTR(oiter);
	return 0;
}

int
vos_obj_iter_probe(struct vos_iterator *iter, daos_hash_out_t *anchor)
{
	struct vos_obj_iter *oiter = vos_iter2oiter(iter);

	switch (iter->it_type) {
	default:
		D_ASSERT(0);
		return -DER_INVAL;

	case VOS_ITER_DKEY:
		return dkey_iter_probe(oiter, anchor);

	case VOS_ITER_AKEY:
		return vec_iter_probe(oiter, anchor);

	case VOS_ITER_RECX:
		return recx_iter_probe(oiter, anchor);
	}
}

static int
vos_obj_iter_next(struct vos_iterator *iter)
{
	struct vos_obj_iter *oiter = vos_iter2oiter(iter);

	switch (iter->it_type) {
	default:
		D_ASSERT(0);
		return -DER_INVAL;

	case VOS_ITER_DKEY:
		return dkey_iter_next(oiter);

	case VOS_ITER_AKEY:
		return vec_iter_next(oiter);

	case VOS_ITER_RECX:
		return recx_iter_next(oiter);
	}
}

static int
vos_obj_iter_fetch(struct vos_iterator *iter, vos_iter_entry_t *it_entry,
		   daos_hash_out_t *anchor)
{
	struct vos_obj_iter *oiter = vos_iter2oiter(iter);

	switch (iter->it_type) {
	default:
		D_ASSERT(0);
		return -DER_INVAL;

	case VOS_ITER_DKEY:
		return dkey_iter_fetch(oiter, it_entry, anchor);

	case VOS_ITER_AKEY:
		return vec_iter_fetch(oiter, it_entry, anchor);

	case VOS_ITER_RECX:
		return recx_iter_fetch(oiter, it_entry, anchor);
	}
}

static int
vos_obj_iter_delete(struct vos_iterator *iter, void *args)
{
	struct vos_obj_iter *oiter = vos_iter2oiter(iter);

	switch (iter->it_type) {
	default:
		D_ASSERT(0);
		return -DER_INVAL;
	case VOS_ITER_DKEY:
	case VOS_ITER_AKEY:
	case VOS_ITER_RECX:
		return obj_iter_delete(oiter, args);
	}
}

static int
vos_obj_iter_empty(struct vos_iterator *iter)
{
	struct vos_obj_iter *oiter = vos_iter2oiter(iter);

	switch (iter->it_type) {
	default:
		D_ASSERT(0);
		return -DER_INVAL;
	case VOS_ITER_DKEY:
	case VOS_ITER_AKEY:
	case VOS_ITER_RECX:
		if (daos_handle_is_inval(oiter->it_hdl))
			return -DER_NO_HDL;

		return dbtree_iter_empty(oiter->it_hdl);
	}
}

struct vos_iter_ops	vos_obj_iter_ops = {
	.iop_prepare	= vos_obj_iter_prep,
	.iop_finish	= vos_obj_iter_fini,
	.iop_probe	= vos_obj_iter_probe,
	.iop_next	= vos_obj_iter_next,
	.iop_fetch	= vos_obj_iter_fetch,
	.iop_delete	= vos_obj_iter_delete,
	.iop_empty	= vos_obj_iter_empty,
};
/**
 * @} vos_obj_iters
 */
