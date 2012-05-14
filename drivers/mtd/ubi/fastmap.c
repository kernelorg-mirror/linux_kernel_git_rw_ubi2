/*
 * Copyright (c) 2012 Linutronix GmbH
 * Author: Richard Weinberger <richard@nod.at>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See
 * the GNU General Public License for more details.
 *
 */

#include <linux/crc32.h>
#include "ubi.h"

/**
 * new_fm_vhdr - allocate a new volume header for fastmap usage.
 * @ubi: UBI device description object
 * @vol_id: the VID of the new header
 */
static struct ubi_vid_hdr *new_fm_vhdr(struct ubi_device *ubi, int vol_id)
{
	struct ubi_vid_hdr *new;

	new = ubi_zalloc_vid_hdr(ubi, GFP_KERNEL);
	if (!new)
		goto out;

	new->vol_type = UBI_VID_DYNAMIC;
	new->vol_id = cpu_to_be32(vol_id);

	/* the fastmap has be deleted on older kernels */
	new->compat = UBI_COMPAT_DELETE;

out:
	return new;
}

/**
 * add_seb - create and add a scan erase block to a given list.
 * @si: UBI scan info object
 * @list: the target list
 * @pnum: PEB number of the new scan erase block
 * @ec: erease counter of the new SEB
 */
static int add_seb(struct ubi_scan_info *si, struct list_head *list,
		   int pnum, int ec)
{
	struct ubi_scan_leb *seb;

	seb = kmem_cache_alloc(si->scan_leb_slab, GFP_KERNEL);
	if (!seb)
		return -ENOMEM;

	seb->pnum = pnum;
	seb->ec = ec;
	seb->lnum = -1;
	seb->scrub = seb->copy_flag = seb->sqnum = 0;

	si->ec_sum += seb->ec;
	si->ec_count++;

	if (si->max_ec < seb->ec)
		si->max_ec = seb->ec;

	if (si->min_ec > seb->ec)
		si->min_ec = seb->ec;

	list_add_tail(&seb->u.list, list);

	return 0;
}

/**
 * add_vol - create and add a new scan volume to ubi_scan_info.
 * @si: ubi_scan_info object
 * @vol_id: VID of the new volume
 * @used_ebs: number of used EBS
 * @data_pad: data padding value of the new volume
 * @vol_type: volume type
 * @last_eb_bytes: number of bytes in the last LEB
 */
static struct ubi_scan_volume *add_vol(struct ubi_scan_info *si, int vol_id,
				       int used_ebs, int data_pad, u8 vol_type,
				       int last_eb_bytes)
{
	struct ubi_scan_volume *sv;
	struct rb_node **p = &si->volumes.rb_node, *parent = NULL;

	while (*p) {
		parent = *p;
		sv = rb_entry(parent, struct ubi_scan_volume, rb);

		if (vol_id > sv->vol_id)
			p = &(*p)->rb_left;
		else if (vol_id > sv->vol_id)
			p = &(*p)->rb_right;
	}

	sv = kmalloc(sizeof(struct ubi_scan_volume), GFP_KERNEL);
	if (!sv)
		goto out;

	sv->highest_lnum = sv->leb_count = 0;
	sv->vol_id = vol_id;
	sv->used_ebs = used_ebs;
	sv->data_pad = data_pad;
	sv->last_data_size = last_eb_bytes;
	sv->compat = 0;
	sv->vol_type = vol_type;
	sv->root = RB_ROOT;

	rb_link_node(&sv->rb, parent, p);
	rb_insert_color(&sv->rb, &si->volumes);

out:
	return sv;
}

/**
 * assign_seb_to_sv - assigns a SEB to a given scan_volume and removes it
 * from it's original list.
 * @si: ubi_scan_info object
 * @seb: the to be assigned SEB
 * @sv: target scan volume
 */
static void assign_seb_to_sv(struct ubi_scan_info *si,
			     struct ubi_scan_leb *seb,
			     struct ubi_scan_volume *sv)
{
	struct ubi_scan_leb *tmp_seb;
	struct rb_node **p = &si->volumes.rb_node, *parent = NULL;

	p = &sv->root.rb_node;
	while (*p) {
		parent = *p;

		tmp_seb = rb_entry(parent, struct ubi_scan_leb, u.rb);
		if (seb->lnum != tmp_seb->lnum) {
			if (seb->lnum < tmp_seb->lnum)
				p = &(*p)->rb_left;
			else
				p = &(*p)->rb_right;

			continue;
		} else
			break;
	}

	list_del(&seb->u.list);
	sv->leb_count++;

	rb_link_node(&seb->u.rb, parent, p);
	rb_insert_color(&seb->u.rb, &sv->root);
}

/**
 * update_vol - inserts or updates a LEB which was found a pool.
 * @ubi: the UBI device object
 * @si: scan info object
 * @sv: the scan volume where this LEB belongs to
 * @new_vh: the volume header derived from new_seb
 * @new_seb: the SEB to be examined
 */
static int update_vol(struct ubi_device *ubi, struct ubi_scan_info *si,
		      struct ubi_scan_volume *sv, struct ubi_vid_hdr *new_vh,
		      struct ubi_scan_leb *new_seb)
{
	struct rb_node **p = &sv->root.rb_node, *parent = NULL;
	struct ubi_scan_leb *seb, *victim;
	int cmp_res;

	while (*p) {
		parent = *p;
		seb = rb_entry(parent, struct ubi_scan_leb, u.rb);

		if (be32_to_cpu(new_vh->lnum) != seb->lnum) {
			if (be32_to_cpu(new_vh->lnum) < seb->lnum)
				p = &(*p)->rb_left;
			else
				p = &(*p)->rb_right;

			continue;
		}

		/* This case can happen if the fastmap gets written
		 * because of a volume change (creation, deletion, ..).
		 * Then a PEB can be within the persistent EBA and the pool.
		 */
		if (seb->pnum == new_seb->pnum) {
			kmem_cache_free(si->scan_leb_slab, new_seb);

			return 0;
		}

		cmp_res = ubi_compare_lebs(ubi, seb, new_seb->pnum, new_vh);
		if (cmp_res < 0)
			return cmp_res;

		/* new_seb is newer */
		if (cmp_res & 1) {
			victim = kmem_cache_alloc(si->scan_leb_slab,
				GFP_KERNEL);
			if (!victim)
				return -ENOMEM;

			victim->ec = seb->ec;
			victim->pnum = seb->pnum;
			list_add_tail(&victim->u.list, &si->erase);

			if (sv->highest_lnum == be32_to_cpu(new_vh->lnum))
				sv->last_data_size = \
					be32_to_cpu(new_vh->data_size);

			seb->ec = new_seb->ec;
			seb->pnum = new_seb->pnum;
			seb->copy_flag = new_vh->copy_flag;
			kmem_cache_free(si->scan_leb_slab, new_seb);

		/* new_seb is older */
		} else {
			ubi_msg("Vol %i: LEB %i's PEB %i is old, dropping it\n",
				sv->vol_id, seb->lnum, new_seb->pnum);
			list_add_tail(&new_seb->u.list, &si->erase);
		}

		return 0;
	}
	/* This LEB is new, let's add it to the volume */

	if (sv->highest_lnum <= be32_to_cpu(new_vh->lnum)) {
		sv->highest_lnum = be32_to_cpu(new_vh->lnum);
		sv->last_data_size = be32_to_cpu(new_vh->data_size);
	}

	if (sv->vol_type == UBI_STATIC_VOLUME)
		sv->used_ebs = be32_to_cpu(new_vh->used_ebs);

	sv->leb_count++;

	rb_link_node(&new_seb->u.rb, parent, p);
	rb_insert_color(&new_seb->u.rb, &sv->root);

	return 0;
}

/**
 * process_pool_seb - we found a non-empty PEB in a pool
 * @ubi: UBI device object
 * @si: scan info object
 * @new_vh: the volume header derived from new_seb
 * @new_seb: the SEB to be examined
 */
static int process_pool_seb(struct ubi_device *ubi, struct ubi_scan_info *si,
			    struct ubi_vid_hdr *new_vh,
			    struct ubi_scan_leb *new_seb)
{
	struct ubi_scan_volume *sv, *tmp_sv = NULL;
	struct rb_node **p = &si->volumes.rb_node, *parent = NULL;
	int found = 0;

	if (be32_to_cpu(new_vh->vol_id) == UBI_FM_SB_VOLUME_ID ||
		be32_to_cpu(new_vh->vol_id) == UBI_FM_DATA_VOLUME_ID) {
		kmem_cache_free(si->scan_leb_slab, new_seb);

		return 0;
	}

	/* Find the volume this SEB belongs to */
	while (*p) {
		parent = *p;
		tmp_sv = rb_entry(parent, struct ubi_scan_volume, rb);

		if (be32_to_cpu(new_vh->vol_id) > tmp_sv->vol_id)
			p = &(*p)->rb_left;
		else if (be32_to_cpu(new_vh->vol_id) < tmp_sv->vol_id)
			p = &(*p)->rb_right;
		else {
			found = 1;
			break;
		}
	}

	if (found)
		sv = tmp_sv;
	else {
		ubi_err("Orphaned volume in fastmap pool!");

		return -EINVAL;
	}

	ubi_assert(be32_to_cpu(new_vh->vol_id) == sv->vol_id);

	return update_vol(ubi, si, sv, new_vh, new_seb);
}

/**
 * scan_pool - scans a pool for changed (no longer empty PEBs)
 * @ubi: UBI device object
 * @si: scan info object
 * @pebs: an array of all PEB numbers in the to be scanned pool
 * @pool_size: size of the pool (number of entries in @pebs)
 * @max_sqnum2: pointer to the maximal sequence number
 */
static int scan_pool(struct ubi_device *ubi, struct ubi_scan_info *si,
	int *pebs, int pool_size, unsigned long long *max_sqnum2)
{
	struct ubi_vid_hdr *vh;
	struct ubi_ec_hdr *ech;
	struct ubi_scan_leb *new_seb;
	int i;
	int pnum;
	int err;
	int ret = 0;

	ech = kzalloc(ubi->ec_hdr_alsize, GFP_KERNEL);
	if (!ech)
		return -ENOMEM;

	vh = ubi_zalloc_vid_hdr(ubi, GFP_KERNEL);
	if (!vh) {
		kfree(ech);
		return -ENOMEM;
	}

	/*
	 * Now scan all PEBs in the pool to find changes which have been made
	 * after the creation of the fastmap
	 */
	for (i = 0; i < pool_size; i++) {
		pnum = be32_to_cpu(pebs[i]);

		err = ubi_io_read_vid_hdr(ubi, pnum, vh, 0);

		if (err == UBI_IO_FF)
			continue;
		else if (err == 0) {
			err = ubi_io_read_ec_hdr(ubi, pnum, ech, 0);
			if (err) {
				ret = err;

				goto out;
			}

			new_seb = kmem_cache_alloc(si->scan_leb_slab,
				GFP_KERNEL);
			if (!new_seb) {
				ret = -ENOMEM;

				goto out;
			}

			new_seb->ec = be64_to_cpu(ech->ec);
			new_seb->pnum = pnum;
			new_seb->lnum = be32_to_cpu(vh->lnum);
			new_seb->sqnum = be64_to_cpu(vh->sqnum);
			new_seb->copy_flag = vh->copy_flag;
			new_seb->scrub = 0;

			err = process_pool_seb(ubi, si, vh, new_seb);
			if (err) {
				ret = err;

				goto out;
			}

			if (*max_sqnum2 < new_seb->sqnum)
				*max_sqnum2 = new_seb->sqnum;
		} else {
			/* We are paranoid and fall back to scanning mode */
			ubi_err("Checkpoint pool PEBs contains damaged PEBs!");
			ret = err;

			goto out;
		}

	}

out:
	ubi_free_vid_hdr(ubi, vh);
	kfree(ech);

	return ret;
}

/**
 * ubi_scan_fastmap - creates ubi_scan_info from a fastmap.
 * @ubi: UBI device object
 * @fm_raw: the fastmap it self as byte array
 * @fm_size: size of the fastmap in bytes
 */
struct ubi_scan_info *ubi_scan_fastmap(struct ubi_device *ubi, char *fm_raw,
				       size_t fm_size)
{
	struct list_head used;
	struct ubi_scan_volume *sv;
	struct ubi_scan_leb *seb, *tmp_seb, *_tmp_seb;
	struct ubi_scan_info *si;
	int i, j;

	size_t fm_pos = 0;
	struct ubi_fm_sb *fmsb;
	struct ubi_fm_hdr *fmhdr;
	struct ubi_fm_scan_pool *fmpl;
	struct ubi_fm_ec *fmec;
	struct ubi_fm_volhdr *fmvhdr;
	struct ubi_fm_eba *fm_eba;

	unsigned long long max_sqnum2 = 0;

	si = kzalloc(sizeof(struct ubi_scan_info), GFP_KERNEL);
	if (!si)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&used);
	INIT_LIST_HEAD(&si->corr);
	INIT_LIST_HEAD(&si->free);
	INIT_LIST_HEAD(&si->erase);
	INIT_LIST_HEAD(&si->alien);
	si->volumes = RB_ROOT;
	si->min_ec = UBI_MAX_ERASECOUNTER;

	si->scan_leb_slab = kmem_cache_create("ubi_scan_leb_slab",
					      sizeof(struct ubi_scan_leb),
					      0, 0, NULL);
	if (!si->scan_leb_slab)
		goto fail;

	fmsb = (struct ubi_fm_sb *)(fm_raw);
	si->max_sqnum = fmsb->sqnum;
	fm_pos += sizeof(struct ubi_fm_sb);
	if (fm_pos >= fm_size)
		goto fail;

	fmhdr = (struct ubi_fm_hdr *)(fm_raw + fm_pos);
	fm_pos += sizeof(*fmhdr);
	if (fm_pos >= fm_size)
		goto fail;

	if (fmhdr->magic != UBI_FM_HDR_MAGIC)
		goto fail;

	fmpl = (struct ubi_fm_scan_pool *)(fm_raw + fm_pos);
	fm_pos += sizeof(*fmpl);
	if (fm_pos >= fm_size)
		goto fail;
	if (fmpl->magic != UBI_FM_POOL_MAGIC)
		goto fail;

	/* read EC values from free list */
	for (i = 0; i < be32_to_cpu(fmhdr->nfree); i++) {
		fmec = (struct ubi_fm_ec *)(fm_raw + fm_pos);
		fm_pos += sizeof(*fmec);
		if (fm_pos >= fm_size)
			goto fail;

		add_seb(si, &si->free, be32_to_cpu(fmec->pnum),
			be32_to_cpu(fmec->ec));
	}

	/* read EC values from used list */
	for (i = 0; i < be32_to_cpu(fmhdr->nused); i++) {
		fmec = (struct ubi_fm_ec *)(fm_raw + fm_pos);
		fm_pos += sizeof(*fmec);
		if (fm_pos >= fm_size)
			goto fail;

		add_seb(si, &used, be32_to_cpu(fmec->pnum),
			be32_to_cpu(fmec->ec));
	}

	si->mean_ec = div_u64(si->ec_sum, si->ec_count);

	/* Iterate over all volumes and read their EBA table */
	for (i = 0; i < be32_to_cpu(fmhdr->nvol); i++) {
		fmvhdr = (struct ubi_fm_volhdr *)(fm_raw + fm_pos);
		fm_pos += sizeof(*fmvhdr);
		if (fm_pos >= fm_size)
			goto fail;

		if (fmvhdr->magic != UBI_FM_VHDR_MAGIC)
			goto fail;

		sv = add_vol(si, be32_to_cpu(fmvhdr->vol_id),
			be32_to_cpu(fmvhdr->used_ebs),
			be32_to_cpu(fmvhdr->data_pad),
			fmvhdr->vol_type, be32_to_cpu(fmvhdr->last_eb_bytes));

		if (!sv)
			goto fail;

		si->vols_found++;
		if (si->highest_vol_id < be32_to_cpu(fmvhdr->vol_id))
			si->highest_vol_id = be32_to_cpu(fmvhdr->vol_id);

		fm_eba = (struct ubi_fm_eba *)(fm_raw + fm_pos);
		fm_pos += sizeof(*fm_eba) + (sizeof(__be32) * be32_to_cpu(fm_eba->nused));
		if (fm_pos >= fm_size)
			goto fail;

		if (fm_eba->magic != UBI_FM_EBA_MAGIC)
			goto fail;

		for (j = 0; j < be32_to_cpu(fm_eba->nused); j++) {

			if ((int)be32_to_cpu(fm_eba->pnum[j]) < 0)
				continue;

			seb = NULL;
			list_for_each_entry(tmp_seb, &used, u.list) {
				if (tmp_seb->pnum == be32_to_cpu(fm_eba->pnum[j]))
					seb = tmp_seb;
			}

			/* Corner case, this PEB must be in the pool */
			if (!seb)
				continue;

			seb->lnum = j;

			if (sv->highest_lnum <= seb->lnum)
				sv->highest_lnum = seb->lnum;

			assign_seb_to_sv(si, seb, sv);

			dbg_bld("Inserting pnum %i (leb %i) to vol %i",
				seb->pnum, seb->lnum, sv->vol_id);
		}
	}

	/*
	 * The remainning PEB in the used list are not used.
	 * They lived in the fastmap pool but got never used.
	 */
	list_for_each_entry_safe(tmp_seb, _tmp_seb, &used, u.list) {
		list_del(&tmp_seb->u.list);
		list_add_tail(&tmp_seb->u.list, &si->free);
	}

	if (scan_pool(ubi, si, fmpl->pebs, be32_to_cpu(fmpl->size),
			&max_sqnum2) < 0)
		goto fail;

	if (max_sqnum2 > si->max_sqnum)
		si->max_sqnum = max_sqnum2;

	return si;

fail:
	ubi_scan_destroy_si(si);
	return NULL;
}

/**
 * ubi_read_fastmap - read the fastmap
 * @ubi: UBI device object
 * @cb_sb_pnum: PEB number of the fastmap super block
 */
struct ubi_scan_info *ubi_read_fastmap(struct ubi_device *ubi,
					  int cb_sb_pnum)
{
	struct ubi_fm_sb *fmsb;
	struct ubi_vid_hdr *vh;
	int ret, i, nblocks;
	char *fm_raw;
	size_t fm_size;
	__be32 data_crc;
	unsigned long long sqnum = 0;
	struct ubi_scan_info *si = NULL;

	fmsb = kmalloc(sizeof(*fmsb), GFP_KERNEL);
	if (!fmsb) {
		si = ERR_PTR(-ENOMEM);

		goto out;
	}

	ret = ubi_io_read(ubi, fmsb, cb_sb_pnum, ubi->leb_start, sizeof(*fmsb));
	if (ret) {
		ubi_err("Unable to read fastmap super block");
		si = ERR_PTR(ret);
		kfree(fmsb);

		goto out;
	}

	if (fmsb->magic != UBI_FM_SB_MAGIC) {
		ubi_err("Super block magic does not match");
		si = ERR_PTR(-EINVAL);
		kfree(fmsb);

		goto out;
	}

	if (fmsb->version != UBI_FM_FMT_VERSION) {
		ubi_err("Unknown fastmap format version!");
		si = ERR_PTR(-EINVAL);
		kfree(fmsb);

		goto out;
	}

	nblocks = be32_to_cpu(fmsb->nblocks);

	if (nblocks > UBI_FM_MAX_BLOCKS || nblocks < 1) {
		ubi_err("Number of fastmap blocks is invalid");
		si = ERR_PTR(-EINVAL);
		kfree(fmsb);

		goto out;
	}

	fm_size = ubi->leb_size * nblocks;
	/* fm_raw will contain the whole fastmap */
	fm_raw = vzalloc(fm_size);
	if (!fm_raw) {
		si = ERR_PTR(-ENOMEM);
		kfree(fmsb);
	}

	vh = ubi_zalloc_vid_hdr(ubi, GFP_KERNEL);
	if (!vh) {
		si = ERR_PTR(-ENOMEM);
		kfree(fmsb);

		goto free_raw;
	}

	for (i = 0; i < nblocks; i++) {
		ret = ubi_io_read_vid_hdr(ubi, be32_to_cpu(fmsb->block_loc[i]),
			vh, 0);
		if (ret) {
			ubi_err("Unable to read fastmap block# %i (PEB: %i)",
				i, be32_to_cpu(fmsb->block_loc[i]));
			si = ERR_PTR(ret);

			goto free_vhdr;
		}

		if (i == 0) {
			if (be32_to_cpu(vh->vol_id) != UBI_FM_SB_VOLUME_ID) {
				si = ERR_PTR(-EINVAL);

				goto free_vhdr;
			}
		} else {
			if (be32_to_cpu(vh->vol_id) != UBI_FM_DATA_VOLUME_ID) {
				goto free_vhdr;

				si = ERR_PTR(-EINVAL);
			}
		}

		if (sqnum < be64_to_cpu(vh->sqnum))
			sqnum = be64_to_cpu(vh->sqnum);

		ret = ubi_io_read(ubi, fm_raw + (ubi->leb_size * i),
			be32_to_cpu(fmsb->block_loc[i]),
			ubi->leb_start, ubi->leb_size);

		if (ret) {
			ubi_err("Unable to read fastmap block# %i (PEB: %i)",
				i, be32_to_cpu(fmsb->block_loc[i]));
			si = ERR_PTR(ret);

			goto free_vhdr;
		}
	}

	kfree(fmsb);

	fmsb = (struct ubi_fm_sb *)fm_raw;
	data_crc = crc32_be(UBI_CRC32_INIT, fm_raw + sizeof(*fmsb),
		fm_size - sizeof(*fmsb));
	if (data_crc != fmsb->data_crc) {
		ubi_err("Checkpoint data CRC is invalid");
		si = ERR_PTR(-EINVAL);

		goto free_vhdr;
	}

	fmsb->sqnum = sqnum;

	si = ubi_scan_fastmap(ubi, fm_raw, fm_size);
	if (!si) {
		si = ERR_PTR(-EINVAL);

		goto free_vhdr;
	}

	/* Store the fastmap position into the ubi_device struct */
	ubi->fm = kmalloc(sizeof(struct ubi_fastmap), GFP_KERNEL);
	if (!ubi->fm) {
		si = ERR_PTR(-ENOMEM);
		ubi_scan_destroy_si(si);

		goto free_vhdr;
	}

	ubi->fm->size = fm_size;
	ubi->fm->used_blocks = nblocks;

	for (i = 0; i < UBI_FM_MAX_BLOCKS; i++) {
		if (i < nblocks) {
			ubi->fm->peb[i] = be32_to_cpu(fmsb->block_loc[i]);
			ubi->fm->ec[i] = be32_to_cpu(fmsb->block_ec[i]);
		} else {
			ubi->fm->peb[i] = -1;
			ubi->fm->ec[i] = 0;
		}
	}

free_vhdr:
	ubi_free_vid_hdr(ubi, vh);
free_raw:
	vfree(fm_raw);
out:
	return si;
}

/**
 * ubi_find_fastmap - searches the first UBI_FM_MAX_START PEBs for the
 * fastmap super block.
 * @ubi: UBI device object
 */
int ubi_find_fastmap(struct ubi_device *ubi)
{
	int i, ret;
	int fm_sb = -ENOENT;
	struct ubi_vid_hdr *vhdr;

	vhdr = ubi_zalloc_vid_hdr(ubi, GFP_KERNEL);
	if (!vhdr)
		return -ENOMEM;

	for (i = 0; i < UBI_FM_MAX_START; i++) {
		ret = ubi_io_read_vid_hdr(ubi, i, vhdr, 0);
		/* ignore read errors */
		if (ret)
			continue;

		if (be32_to_cpu(vhdr->vol_id) == UBI_FM_SB_VOLUME_ID) {
			fm_sb = i;
			break;
		}
	}

	ubi_free_vid_hdr(ubi, vhdr);
	return fm_sb;
}

/**
 * ubi_write_fastmap - writes a fastmap
 * @ubi: UBI device object
 * @new_fm: the to be written checkppoint
 */
static int ubi_write_fastmap(struct ubi_device *ubi,
				struct ubi_fastmap *new_fm)
{
	int ret;
	size_t fm_pos = 0;
	char *fm_raw;
	int i, j;

	struct ubi_fm_sb *fmsb;
	struct ubi_fm_hdr *cph;
	struct ubi_fm_scan_pool *cppl;
	struct ubi_fm_ec *cec;
	struct ubi_fm_volhdr *cvh;
	struct ubi_fm_eba *ceba;

	struct rb_node *node;
	struct ubi_wl_entry *wl_e;
	struct ubi_volume *vol;

	struct ubi_vid_hdr *svhdr, *dvhdr;

	int nfree, nused, nvol;

	fm_raw = vzalloc(new_fm->size);
	if (!fm_raw) {
		ret = -ENOMEM;

		goto out;
	}

	svhdr = new_fm_vhdr(ubi, UBI_FM_SB_VOLUME_ID);
	if (!svhdr) {
		ret = -ENOMEM;

		goto out_vfree;
	}

	dvhdr = new_fm_vhdr(ubi, UBI_FM_DATA_VOLUME_ID);
	if (!dvhdr) {
		ret = -ENOMEM;

		goto out_kfree;
	}

	spin_lock(&ubi->volumes_lock);
	spin_lock(&ubi->wl_lock);

	fmsb = (struct ubi_fm_sb *)fm_raw;
	fm_pos += sizeof(*fmsb);
	ubi_assert(fm_pos <= new_fm->size);

	cph = (struct ubi_fm_hdr *)(fm_raw + fm_pos);
	fm_pos += sizeof(*cph);
	ubi_assert(fm_pos <= new_fm->size);

	fmsb->magic = UBI_FM_SB_MAGIC;
	fmsb->version = UBI_FM_FMT_VERSION;
	fmsb->nblocks = cpu_to_be32(new_fm->used_blocks);
	/* the max sqnum will be filled in while *reading* the fastmap */
	fmsb->sqnum = 0;

	cph->magic = UBI_FM_HDR_MAGIC;
	nfree = 0;
	nused = 0;
	nvol = 0;

	cppl = (struct ubi_fm_scan_pool *)(fm_raw + fm_pos);
	fm_pos += sizeof(*cppl);
	cppl->magic = UBI_FM_POOL_MAGIC;
	cppl->size = cpu_to_be32(ubi->fm_pool.size);

	for (i = 0; i < ubi->fm_pool.size; i++)
		cppl->pebs[i] = cpu_to_be32(ubi->fm_pool.pebs[i]);

	for (node = rb_first(&ubi->free); node; node = rb_next(node)) {
		wl_e = rb_entry(node, struct ubi_wl_entry, u.rb);
		cec = (struct ubi_fm_ec *)(fm_raw + fm_pos);

		cec->pnum = cpu_to_be32(wl_e->pnum);
		cec->ec = cpu_to_be32(wl_e->ec);

		nfree++;
		fm_pos += sizeof(*cec);
		ubi_assert(fm_pos <= new_fm->size);
	}
	cph->nfree = cpu_to_be32(nfree);

	for (node = rb_first(&ubi->used); node; node = rb_next(node)) {
		wl_e = rb_entry(node, struct ubi_wl_entry, u.rb);
		cec = (struct ubi_fm_ec *)(fm_raw + fm_pos);

		cec->pnum = cpu_to_be32(wl_e->pnum);
		cec->ec = cpu_to_be32(wl_e->ec);

		nused++;
		fm_pos += sizeof(*cec);
		ubi_assert(fm_pos <= new_fm->size);
	}
	cph->nused = cpu_to_be32(nused);

	for (i = 0; i < UBI_MAX_VOLUMES + UBI_INT_VOL_COUNT; i++) {
		vol = ubi->volumes[i];

		if (!vol)
			continue;

		nvol++;

		cvh = (struct ubi_fm_volhdr *)(fm_raw + fm_pos);
		fm_pos += sizeof(*cvh);
		ubi_assert(fm_pos <= new_fm->size);

		cvh->magic = UBI_FM_VHDR_MAGIC;
		cvh->vol_id = cpu_to_be32(vol->vol_id);
		cvh->vol_type = vol->vol_type;
		cvh->used_ebs = cpu_to_be32(vol->used_ebs);
		cvh->data_pad = cpu_to_be32(vol->data_pad);
		cvh->last_eb_bytes = cpu_to_be32(vol->last_eb_bytes);

		ubi_assert(vol->vol_type == UBI_DYNAMIC_VOLUME ||
			vol->vol_type == UBI_STATIC_VOLUME);

		ceba = (struct ubi_fm_eba *)(fm_raw + fm_pos);
		fm_pos += sizeof(*ceba) + (sizeof(__be32) * vol->used_ebs);
		ubi_assert(fm_pos <= new_fm->size);

		for (j = 0; j < vol->used_ebs; j++)
			ceba->pnum[j] = cpu_to_be32(vol->eba_tbl[j]);

		ceba->nused = cpu_to_be32(j);
		ceba->magic = UBI_FM_EBA_MAGIC;
	}
	cph->nvol = cpu_to_be32(nvol);

	svhdr->sqnum = cpu_to_be64(ubi_next_sqnum(ubi));
	svhdr->lnum = 0;

	spin_unlock(&ubi->wl_lock);
	spin_unlock(&ubi->volumes_lock);

	dbg_bld("Writing fastmap SB to PEB %i\n", new_fm->peb[0]);
	ret = ubi_io_write_vid_hdr(ubi, new_fm->peb[0], svhdr);
	if (ret) {
		ubi_err("Unable to write vid_hdr to fastmap SB!\n");

		goto out_kfree;
	}

	for (i = 0; i < UBI_FM_MAX_BLOCKS; i++) {
		fmsb->block_loc[i] = cpu_to_be32(new_fm->peb[i]);
		fmsb->block_ec[i] = cpu_to_be32(new_fm->ec[i]);
	}

	fmsb->data_crc = 0;
	fmsb->data_crc = crc32_be(UBI_CRC32_INIT, fm_raw + sizeof(*fmsb),
		new_fm->size - sizeof(*fmsb));

	for (i = 1; i < new_fm->used_blocks; i++) {
		dvhdr->sqnum = cpu_to_be64(ubi_next_sqnum(ubi));
		dvhdr->lnum = cpu_to_be32(i);
		dbg_bld("Writing fastmap data to PEB %i sqnum %llu\n",
			new_fm->peb[i], be64_to_cpu(dvhdr->sqnum));
		ret = ubi_io_write_vid_hdr(ubi, new_fm->peb[i], dvhdr);
		if (ret) {
			ubi_err("Unable to write vid_hdr to PEB %i!\n",
				new_fm->peb[i]);

			goto out_kfree;
		}
	}

	for (i = 0; i < new_fm->used_blocks; i++) {
		ret = ubi_io_write(ubi, fm_raw + (i * ubi->leb_size),
			new_fm->peb[i], ubi->leb_start, ubi->leb_size);
		if (ret) {
			ubi_err("Unable to write fastmap to PEB %i!\n",
				new_fm->peb[i]);

			goto out_kfree;
		}
	}

	ubi_assert(new_fm);
	ubi->fm = new_fm;

	dbg_bld("Checkpoint written!");

out_kfree:
	kfree(svhdr);
out_vfree:
	vfree(fm_raw);
out:
	return ret;
}

/**
 * get_ec - returns the erase counter of a given PEB
 * @ubi: UBI device object
 * @pnum: PEB number
 */
static int get_ec(struct ubi_device *ubi, int pnum)
{
	struct ubi_wl_entry *e;

	e = ubi->lookuptbl[pnum];

	/* can this really happen? */
	if (!e)
		return ubi->mean_ec ?: 1;
	else
		return e->ec;
}

/**
 * ubi_update_fastmap - will be called by UBI if a volume changes or
 * a fastmap pool becomes full.
 * @ubi: UBI device object
 */
int ubi_update_fastmap(struct ubi_device *ubi)
{
	int ret, i;
	struct ubi_fastmap *new_fm;

	if (ubi->ro_mode)
		return 0;

	new_fm = kmalloc(sizeof(*new_fm), GFP_KERNEL);
	if (!new_fm)
		return -ENOMEM;

	ubi->old_fm = ubi->fm;
	ubi->fm = NULL;

	if (ubi->old_fm) {
		spin_lock(&ubi->wl_lock);
		new_fm->peb[0] = ubi_wl_get_fm_peb(ubi, UBI_FM_MAX_START);
		spin_unlock(&ubi->wl_lock);
		/* no fresh early PEB was found, reuse the old one */
		if (new_fm->peb[0] < 0) {
			struct ubi_ec_hdr *ec_hdr;

			ec_hdr = kzalloc(ubi->ec_hdr_alsize, GFP_KERNEL);
			if (!ec_hdr) {
				kfree(new_fm);
				return -ENOMEM;
			}

			/* we have to erase the block by hand */

			ret = ubi_io_read_ec_hdr(ubi, ubi->old_fm->peb[0],
				ec_hdr, 0);
			if (ret) {
				ubi_err("Unable to read EC header");

				kfree(new_fm);
				kfree(ec_hdr);
				return -EINVAL;
			}

			ret = ubi_io_sync_erase(ubi, ubi->old_fm->peb[0], 0);
			if (ret < 0) {
				ubi_err("Unable to erase old SB");

				kfree(new_fm);
				kfree(ec_hdr);
				return -EINVAL;
			}

			ec_hdr->ec += ret;
			if (ret > UBI_MAX_ERASECOUNTER) {
				ubi_err("Erase counter overflow!");
				kfree(new_fm);
				kfree(ec_hdr);
				return -EINVAL;
			}

			ret = ubi_io_write_ec_hdr(ubi, ubi->old_fm->peb[0],
				ec_hdr);
			kfree(ec_hdr);
			if (ret) {
				ubi_err("Unable to write new EC header");
				kfree(new_fm);
				return -EINVAL;
			}

			new_fm->peb[0] = ubi->old_fm->peb[0];
			new_fm->ec[0] = ubi->old_fm->ec[0];
		} else {
			/* we've got a new early PEB, return the old one */
			ubi_wl_put_fm_peb(ubi, ubi->old_fm->peb[0], 0);
			new_fm->ec[0] = get_ec(ubi, new_fm->peb[0]);
		}

		/* return all other fastmap block to the wl system */
		for (i = 1; i < UBI_FM_MAX_BLOCKS; i++) {
			if (ubi->old_fm->peb[i] >= 0)
				ubi_wl_put_fm_peb(ubi, ubi->old_fm->peb[i], 0);
			else
				break;
		}
	} else {
		spin_lock(&ubi->wl_lock);
		new_fm->peb[0] = ubi_wl_get_fm_peb(ubi, UBI_FM_MAX_START);
		spin_unlock(&ubi->wl_lock);
		if (new_fm->peb[0] < 0) {
			ubi_err("Could not find an early PEB");
			kfree(new_fm);
			return -ENOSPC;
		}
		new_fm->ec[0] = get_ec(ubi, new_fm->peb[0]);
	}

	new_fm->size = sizeof(struct ubi_fm_hdr) + \
			sizeof(struct ubi_fm_scan_pool) + \
			(ubi->peb_count * sizeof(struct ubi_fm_ec)) + \
			(sizeof(struct ubi_fm_eba) + \
			(ubi->peb_count * sizeof(__be32))) + \
			sizeof(struct ubi_fm_volhdr) * UBI_MAX_VOLUMES;
	new_fm->size = roundup(new_fm->size, ubi->leb_size);

	new_fm->used_blocks = new_fm->size / ubi->leb_size;

	if (new_fm->used_blocks > UBI_FM_MAX_BLOCKS) {
		ubi_err("Checkpoint too large");
		kfree(new_fm);

		return -ENOSPC;
	}

	/* give the wl subsystem a chance to produce some free blocks */
	cond_resched();

	for (i = 1; i < UBI_FM_MAX_BLOCKS; i++) {
		if (i < new_fm->used_blocks) {
			spin_lock(&ubi->wl_lock);
			new_fm->peb[i] = ubi_wl_get_fm_peb(ubi, INT_MAX);
			spin_unlock(&ubi->wl_lock);
			if (new_fm->peb[i] < 0) {
				ubi_err("Could not get any free erase block");

				while (i--)
					ubi_wl_put_fm_peb(ubi, new_fm->peb[i],
						0);

				kfree(new_fm);

				return -ENOSPC;
			}

			new_fm->ec[i] = get_ec(ubi, new_fm->peb[i]);
		} else {
			new_fm->peb[i] = -1;
			new_fm->ec[i] = 0;
		}
	}

	kfree(ubi->old_fm);
	ubi->old_fm = NULL;

	return ubi_write_fastmap(ubi, new_fm);
}
