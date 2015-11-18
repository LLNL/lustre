/*
 * Copyright (C) 2009-2012 Cray, Inc.
 *
 *   Author: Nic Henke <nic@cray.com>
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* this code liberated and modified from lnet/lnet/router_proc.c */

#define DEBUG_SUBSYSTEM S_LND
#include "gnilnd.h"
#include <linux/seq_file.h>

#define GNILND_PROC_STATS       "stats"
#define GNILND_PROC_MDD         "mdd"
#define GNILND_PROC_SMSG        "smsg"
#define GNILND_PROC_CONN        "conn"
#define GNILND_PROC_PEER_CONNS  "peer_conns"
#define GNILND_PROC_PEER        "peer"
#define GNILND_PROC_CKSUM_TEST  "cksum_test"

static int
_kgnilnd_proc_run_cksum_test(int caseno, int nloops, int nob)
{
	lnet_kiov_t              *src, *dest;
	struct timespec          begin, end, diff;
	int                      niov;
	int                      i = 0, j = 0, n;
	__u16                    cksum, cksum2;
	__u64                    mbytes;

	LIBCFS_ALLOC(src, LNET_MAX_IOV * sizeof(lnet_kiov_t));
	LIBCFS_ALLOC(dest, LNET_MAX_IOV * sizeof(lnet_kiov_t));

	if (src == NULL || dest == NULL) {
		CERROR("couldn't allocate iovs\n");
		GOTO(unwind, -ENOMEM);
	}

	for (i = 0; i < LNET_MAX_IOV; i++) {
		src[i].kiov_offset = 0;
		src[i].kiov_len = PAGE_SIZE;
		src[i].kiov_page = alloc_page(GFP_KERNEL | __GFP_ZERO);

		if (src[i].kiov_page == NULL) {
			CERROR("couldn't allocate page %d\n", i);
			GOTO(unwind, -ENOMEM);
		}

		dest[i].kiov_offset = 0;
		dest[i].kiov_len = PAGE_SIZE;
		dest[i].kiov_page = alloc_page(GFP_KERNEL | __GFP_ZERO);

		if (dest[i].kiov_page == NULL) {
			CERROR("couldn't allocate page %d\n", i);
			GOTO(unwind, -ENOMEM);
		}
	}

	/* add extra 2 pages - one for offset of src, 2nd to allow dest offset */
	niov = (nob / PAGE_SIZE) + 2;
	if (niov > LNET_MAX_IOV) {
		CERROR("bytes %d too large, requires niov %d > %d\n",
			nob, niov, LNET_MAX_IOV);
		GOTO(unwind, -E2BIG);
	}

	/* setup real data */
	src[0].kiov_offset = 317;
	dest[0].kiov_offset = 592;
	switch (caseno) {
	default:
		/* odd -> even */
		break;
	case 1:
		/* odd -> odd */
		dest[0].kiov_offset -= 1;
		break;
	case 2:
		/* even -> even */
		src[0].kiov_offset += 1;
		break;
	case 3:
		/* even -> odd */
		src[0].kiov_offset += 1;
		dest[0].kiov_offset -= 1;
	}
	src[0].kiov_len = PAGE_SIZE - src[0].kiov_offset;
	dest[0].kiov_len = PAGE_SIZE - dest[0].kiov_offset;

	for (i = 0; i < niov; i++) {
		memset(page_address(src[i].kiov_page) + src[i].kiov_offset,
		       0xf0 + i, src[i].kiov_len);
	}

	lnet_copy_kiov2kiov(niov, dest, 0, niov, src, 0, nob);

	getnstimeofday(&begin);

	for (n = 0; n < nloops; n++) {
		CDEBUG(D_BUFFS, "case %d loop %d src %d dest %d nob %d niov %d\n",
		       caseno, n, src[0].kiov_offset, dest[0].kiov_offset, nob, niov);
		cksum = kgnilnd_cksum_kiov(niov, src, 0, nob - (n % nob), 1);
		cksum2 = kgnilnd_cksum_kiov(niov, dest, 0, nob - (n % nob), 1);

		if (cksum != cksum2) {
			CERROR("case %d loop %d different checksums %x expected %x\n",
			       j, n, cksum2, cksum);
			GOTO(unwind, -ENOKEY);
		}
	}

	getnstimeofday(&end);

	mbytes = (nloops * nob * 2) / (1024*1024);

	diff = kgnilnd_ts_sub(end, begin);

	LCONSOLE_INFO("running "LPD64"MB took %ld.%ld seconds\n",
		      mbytes, diff.tv_sec, diff.tv_nsec);

unwind:
	CDEBUG(D_NET, "freeing %d pages\n", i);
	for (i -= 1; i >= 0; i--) {
		if (src[i].kiov_page != NULL) {
			__free_page(src[i].kiov_page);
		}
		if (dest[i].kiov_page != NULL) {
			__free_page(dest[i].kiov_page);
		}
	}

	if (src != NULL)
		LIBCFS_FREE(src, LNET_MAX_IOV * sizeof(lnet_kiov_t));
	if (dest != NULL)
		LIBCFS_FREE(dest, LNET_MAX_IOV * sizeof(lnet_kiov_t));
	return 0;
}

static int
kgnilnd_proc_cksum_test_write(struct file *file, const char *ubuffer,
			      unsigned long count, void *data)
{
	char                    dummy[256 + 1] = { '\0' };
	int                     testno, nloops, nbytes;
	int                     rc;

	if (kgnilnd_data.kgn_init < GNILND_INIT_ALL) {
		CERROR("can't run cksum test, kgnilnd is not initialized yet\n");
		return -ENOSYS;
	}

	if (count >= sizeof(dummy) || count == 0)
		return -EINVAL;

	if (copy_from_user(dummy, ubuffer, count))
		return -EFAULT;

	if (sscanf(dummy, "%d:%d:%d", &testno, &nloops, &nbytes) == 3) {
		rc = _kgnilnd_proc_run_cksum_test(testno, nloops, nbytes);
		if (rc < 0) {
			RETURN(rc);
		} else {
			/* spurious, but lets us know the parse was ok */
			RETURN(count);
		}
	}
	RETURN(count);
}

static int
kgnilnd_proc_stats_read(char *page, char **start, off_t off,
			     int count, int *eof, void *data)
{
	kgn_device_t           *dev;
	struct timeval          now;
	int                     rc;

	if (kgnilnd_data.kgn_init < GNILND_INIT_ALL) {
		rc = sprintf(page,
			"kgnilnd is not initialized yet\n");
		return rc;
	}

	/* only do the first device */
	dev = &kgnilnd_data.kgn_devices[0];

	/* sampling is racy, but so is reading this file! */
	smp_rmb();
	do_gettimeofday(&now);

	rc = sprintf(page, "time: %lu.%lu\n"
			   "ntx: %d\n"
			   "npeers: %d\n"
			   "nconns: %d\n"
			   "nEPs: %d\n"
			   "ndgrams: %d\n"
			   "nfmablk: %d\n"
			   "n_mdd: %d\n"
			   "n_mdd_held: %d\n"
			   "GART map bytes: %ld\n"
			   "TX queued maps: %d\n"
			   "TX phys nmaps: %d\n"
			   "TX phys bytes: %lu\n"
			   "TX virt nmaps: %d\n"
			   "TX virt bytes: "LPU64"\n"
			   "RDMAQ bytes_auth: %ld\n"
			   "RDMAQ bytes_left: %ld\n"
			   "RDMAQ nstalls: %d\n"
			   "dev mutex delay: %ld\n"
			   "dev n_yield: %d\n"
			   "dev n_schedule: %d\n"
			   "SMSG fast_try: %d\n"
			   "SMSG fast_ok: %d\n"
			   "SMSG fast_block: %d\n"
			   "SMSG ntx: %d\n"
			   "SMSG tx_bytes: %ld\n"
			   "SMSG nrx: %d\n"
			   "SMSG rx_bytes: %ld\n"
			   "RDMA ntx: %d\n"
			   "RDMA tx_bytes: %ld\n"
			   "RDMA nrx: %d\n"
			   "RDMA rx_bytes: %ld\n"
			   "VMAP short: %d\n"
			   "VMAP cksum: %d\n"
			   "KMAP short: %d\n"
			   "RDMA REV length: %d\n"
			   "RDMA REV offset: %d\n"
			   "RDMA REV copy: %d\n",
		now.tv_sec, now.tv_usec,
		atomic_read(&kgnilnd_data.kgn_ntx),
		atomic_read(&kgnilnd_data.kgn_npeers),
		atomic_read(&kgnilnd_data.kgn_nconns),
		atomic_read(&dev->gnd_neps),
		atomic_read(&dev->gnd_ndgrams),
		atomic_read(&dev->gnd_nfmablk),
		atomic_read(&dev->gnd_n_mdd), atomic_read(&dev->gnd_n_mdd_held),
		atomic64_read(&dev->gnd_nbytes_map),
		atomic_read(&dev->gnd_nq_map),
		dev->gnd_map_nphys, dev->gnd_map_physnop * PAGE_SIZE,
		dev->gnd_map_nvirt, dev->gnd_map_virtnob,
		atomic64_read(&dev->gnd_rdmaq_bytes_out),
		atomic64_read(&dev->gnd_rdmaq_bytes_ok),
		atomic_read(&dev->gnd_rdmaq_nstalls),
		dev->gnd_mutex_delay,
		atomic_read(&dev->gnd_n_yield), atomic_read(&dev->gnd_n_schedule),
		atomic_read(&dev->gnd_fast_try), atomic_read(&dev->gnd_fast_ok),
		atomic_read(&dev->gnd_fast_block),
		atomic_read(&dev->gnd_short_ntx), atomic64_read(&dev->gnd_short_txbytes),
		atomic_read(&dev->gnd_short_nrx), atomic64_read(&dev->gnd_short_rxbytes),
		atomic_read(&dev->gnd_rdma_ntx), atomic64_read(&dev->gnd_rdma_txbytes),
		atomic_read(&dev->gnd_rdma_nrx), atomic64_read(&dev->gnd_rdma_rxbytes),
		atomic_read(&kgnilnd_data.kgn_nvmap_short),
		atomic_read(&kgnilnd_data.kgn_nvmap_cksum),
		atomic_read(&kgnilnd_data.kgn_nkmap_short),
		atomic_read(&kgnilnd_data.kgn_rev_length),
		atomic_read(&kgnilnd_data.kgn_rev_offset),
		atomic_read(&kgnilnd_data.kgn_rev_copy_buff));

	return rc;
}

static int
kgnilnd_proc_stats_write(struct file *file, const char *ubuffer,
		     unsigned long count, void *data)
{
	kgn_device_t           *dev;

	if (kgnilnd_data.kgn_init < GNILND_INIT_ALL) {
		CERROR("kgnilnd is not initialized for stats write\n");
		return -EINVAL;
	}

	/* only do the first device */
	dev = &kgnilnd_data.kgn_devices[0];

	atomic_set(&dev->gnd_short_ntx, 0);
	atomic_set(&dev->gnd_short_nrx, 0);
	atomic64_set(&dev->gnd_short_txbytes, 0);
	atomic64_set(&dev->gnd_short_rxbytes, 0);
	atomic_set(&dev->gnd_rdma_ntx, 0);
	atomic_set(&dev->gnd_rdma_nrx, 0);
	atomic_set(&dev->gnd_fast_ok, 0);
	atomic_set(&dev->gnd_fast_try, 0);
	atomic_set(&dev->gnd_fast_block, 0);
	atomic64_set(&dev->gnd_rdma_txbytes, 0);
	atomic64_set(&dev->gnd_rdma_rxbytes, 0);
	atomic_set(&dev->gnd_rdmaq_nstalls, 0);
	set_mb(dev->gnd_mutex_delay, 0);
	atomic_set(&dev->gnd_n_yield, 0);
	atomic_set(&dev->gnd_n_schedule, 0);
	atomic_set(&kgnilnd_data.kgn_nvmap_short, 0);
	atomic_set(&kgnilnd_data.kgn_nvmap_cksum, 0);
	atomic_set(&kgnilnd_data.kgn_nkmap_short, 0);
	/* sampling is racy, but so is writing this file! */
	smp_wmb();
	return count;
}

typedef struct {
	kgn_device_t           *gmdd_dev;
	kgn_tx_t               *gmdd_tx;
	loff_t                  gmdd_off;
} kgn_mdd_seq_iter_t;

int
kgnilnd_mdd_seq_seek(kgn_mdd_seq_iter_t *gseq, loff_t off)
{
	kgn_tx_t                *tx;
	struct list_head        *r;
	loff_t                  here;
	int                     rc = 0;

	if (off == 0) {
		gseq->gmdd_tx = NULL;
		gseq->gmdd_off = 0;
		return 0;
	}

	tx = gseq->gmdd_tx;

	if (tx == NULL || gseq->gmdd_off > off) {
		/* search from start */
		r = gseq->gmdd_dev->gnd_map_list.next;
		here = 1;
	} else {
		/* continue current search */
		r = &tx->tx_map_list;
		here = gseq->gmdd_off;
	}

	gseq->gmdd_off = off;

	while (r != &gseq->gmdd_dev->gnd_map_list) {
		kgn_tx_t      *t;

		t = list_entry(r, kgn_tx_t, tx_map_list);

		if (here == off) {
			gseq->gmdd_tx = t;
			rc = 0;
			goto out;
		}
		r = r->next;
		here++;
	}

	gseq->gmdd_tx = NULL;
	rc = -ENOENT;
out:
	return rc;
}

static void *
kgnilnd_mdd_seq_start(struct seq_file *s, loff_t *pos)
{

	kgn_mdd_seq_iter_t      *gseq;
	int                      rc;

	if (kgnilnd_data.kgn_init < GNILND_INIT_ALL) {
		return NULL;
	}

	LIBCFS_ALLOC(gseq, sizeof(*gseq));
	if (gseq == NULL) {
		CERROR("could not allocate mdd sequence iterator\n");
		return NULL;
	}

	/* only doing device 0 for now */
	gseq->gmdd_dev = &kgnilnd_data.kgn_devices[0];
	gseq->gmdd_tx = NULL;

	/* need to lock map while we poke - huge disturbance
	 * but without it, no way to get the data printed */
	spin_lock(&gseq->gmdd_dev->gnd_map_lock);

	/* set private to gseq for stop */
	s->private = gseq;

	rc = kgnilnd_mdd_seq_seek(gseq, *pos);
	if (rc == 0)
		return gseq;
	else
		return NULL;
}

static void
kgnilnd_mdd_seq_stop(struct seq_file *s, void *iter)
{
	kgn_mdd_seq_iter_t     *gseq = s->private;

	if (gseq != NULL) {
		spin_unlock(&gseq->gmdd_dev->gnd_map_lock);
		LIBCFS_FREE(gseq, sizeof(*gseq));
	}
}

static void *
kgnilnd_mdd_seq_next(struct seq_file *s, void *iter, loff_t *pos)
{
	kgn_mdd_seq_iter_t     *gseq = iter;
	int                     rc;
	loff_t                  next = *pos + 1;

	rc = kgnilnd_mdd_seq_seek(gseq, next);
	if (rc != 0) {
		return NULL;
	}
	*pos = next;
	return gseq;
}

static int
kgnilnd_mdd_seq_show(struct seq_file *s, void *iter)
{
	kgn_mdd_seq_iter_t     *gseq = iter;
	kgn_tx_t               *tx;
	__u64                   nob;
	__u32                   physnop;
	int                     id;
	int                     buftype;
	gni_mem_handle_t        hndl;

	if (gseq->gmdd_off == 0) {
		seq_printf(s, "%s %22s %16s %8s %8s %37s\n",
			"tx", "tx_id", "nob", "physnop",
			"buftype", "mem handle");
		return 0;
	}

	tx = gseq->gmdd_tx;
	LASSERT(tx != NULL);

	id = tx->tx_id.txe_smsg_id;
	nob = tx->tx_nob;
	physnop = tx->tx_phys_npages;
	buftype = tx->tx_buftype;
	hndl.qword1 = tx->tx_map_key.qword1;
	hndl.qword2 = tx->tx_map_key.qword2;

	seq_printf(s, "%p %x %16"LPF64"u %8d %#8x "LPX64"."LPX64"x\n",
		tx, id, nob, physnop, buftype,
		hndl.qword1, hndl.qword2);

	return 0;
}

static struct seq_operations kgn_mdd_sops = {
	.start = kgnilnd_mdd_seq_start,
	.stop  = kgnilnd_mdd_seq_stop,
	.next  = kgnilnd_mdd_seq_next,
	.show  = kgnilnd_mdd_seq_show,

};

static int
kgnilnd_mdd_seq_open(struct inode *inode, struct file *file)
{
	struct seq_file       *sf;
	int                    rc;

	rc = seq_open(file, &kgn_mdd_sops);
	if (rc == 0) {
		sf = file->private_data;

		/* NULL means we've not yet open() */
		sf->private = NULL;
	}
	return rc;
}

static struct file_operations kgn_mdd_fops = {
	.owner   = THIS_MODULE,
	.open    = kgnilnd_mdd_seq_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = seq_release,
};

typedef struct {
	__u64                   gsmsg_version;
	kgn_device_t           *gsmsg_dev;
	kgn_fma_memblock_t     *gsmsg_fmablk;
	loff_t                  gsmsg_off;
} kgn_smsg_seq_iter_t;

int
kgnilnd_smsg_seq_seek(kgn_smsg_seq_iter_t *gseq, loff_t off)
{
	kgn_fma_memblock_t             *fmablk;
	kgn_device_t                   *dev;
	struct list_head               *r;
	loff_t                          here;
	int                             rc = 0;

	/* offset 0 is the header, so we start real entries at
	 * here == off == 1 */
	if (off == 0) {
		gseq->gsmsg_fmablk = NULL;
		gseq->gsmsg_off = 0;
		return 0;
	}

	fmablk = gseq->gsmsg_fmablk;
	dev = gseq->gsmsg_dev;

	spin_lock(&dev->gnd_fmablk_lock);

	if (fmablk != NULL &&
		gseq->gsmsg_version != atomic_read(&dev->gnd_fmablk_vers)) {
		/* list changed */
		rc = -ESTALE;
		goto out;
	}

	if (fmablk == NULL || gseq->gsmsg_off > off) {
		/* search from start */
		r = dev->gnd_fma_buffs.next;
		here = 1;
	} else {
		/* continue current search */
		r = &fmablk->gnm_bufflist;
		here = gseq->gsmsg_off;
	}

	gseq->gsmsg_version = atomic_read(&dev->gnd_fmablk_vers);
	gseq->gsmsg_off = off;

	while (r != &dev->gnd_fma_buffs) {
		kgn_fma_memblock_t      *t;

		t = list_entry(r, kgn_fma_memblock_t, gnm_bufflist);

		if (here == off) {
			gseq->gsmsg_fmablk = t;
			rc = 0;
			goto out;
		}
		r = r->next;
		here++;
	}

	gseq->gsmsg_fmablk = NULL;
	rc = -ENOENT;
out:
	spin_unlock(&dev->gnd_fmablk_lock);
	return rc;
}

static void *
kgnilnd_smsg_seq_start(struct seq_file *s, loff_t *pos)
{

	kgn_smsg_seq_iter_t     *gseq;
	int                      rc;

	if (kgnilnd_data.kgn_init < GNILND_INIT_ALL) {
		return NULL;
	}

	LIBCFS_ALLOC(gseq, sizeof(*gseq));
	if (gseq == NULL) {
		CERROR("could not allocate smsg sequence iterator\n");
		return NULL;
	}

	/* only doing device 0 for now */
	gseq->gsmsg_dev = &kgnilnd_data.kgn_devices[0];
	gseq->gsmsg_fmablk = NULL;
	rc = kgnilnd_smsg_seq_seek(gseq, *pos);
	if (rc == 0)
		return gseq;

	LIBCFS_FREE(gseq, sizeof(*gseq));
	return NULL;
}

static void
kgnilnd_smsg_seq_stop(struct seq_file *s, void *iter)
{
	kgn_smsg_seq_iter_t     *gseq = iter;

	if (gseq != NULL)
		LIBCFS_FREE(gseq, sizeof(*gseq));
}

static void *
kgnilnd_smsg_seq_next(struct seq_file *s, void *iter, loff_t *pos)
{
	kgn_smsg_seq_iter_t    *gseq = iter;
	int                     rc;
	loff_t                  next = *pos + 1;

	rc = kgnilnd_smsg_seq_seek(gseq, next);
	if (rc != 0) {
		LIBCFS_FREE(gseq, sizeof(*gseq));
		return NULL;
	}
	*pos = next;
	return gseq;
}

static int
kgnilnd_smsg_seq_show(struct seq_file *s, void *iter)
{
	kgn_smsg_seq_iter_t    *gseq = iter;
	kgn_fma_memblock_t     *fmablk;
	kgn_device_t           *dev;
	int                     avail_mboxs, held_mboxs, num_mboxs;
	unsigned int            blk_size;
	int                     live;
	kgn_fmablk_state_t      state;
	gni_mem_handle_t        hndl;

	if (gseq->gsmsg_off == 0) {
		seq_printf(s, "%5s %4s %6s/%5s/%5s %9s %18s %37s\n",
			"blk#", "type", "avail", "held", "total", "size",
			"fmablk", "mem handle");
		return 0;
	}

	fmablk = gseq->gsmsg_fmablk;
	dev = gseq->gsmsg_dev;
	LASSERT(fmablk != NULL);

	spin_lock(&dev->gnd_fmablk_lock);

	if (gseq->gsmsg_version != atomic_read(&dev->gnd_fmablk_vers)) {
		/* list changed */
		spin_unlock(&dev->gnd_fmablk_lock);
		return -ESTALE;
	}

	live = fmablk->gnm_hold_timeout == 0;
	/* none are available if it isn't live... */
	avail_mboxs = live ? fmablk->gnm_avail_mboxs : 0;
	held_mboxs = fmablk->gnm_held_mboxs;
	num_mboxs = fmablk->gnm_num_mboxs;
	blk_size = fmablk->gnm_blk_size;
	state = fmablk->gnm_state;
	hndl.qword1 = fmablk->gnm_hndl.qword1;
	hndl.qword2 = fmablk->gnm_hndl.qword2;

	spin_unlock(&dev->gnd_fmablk_lock);

	if (live) {
		seq_printf(s, "%5d %4s %6d/%5d/%5d %9d %18p   "LPX64"."LPX64"\n",
			   (int) gseq->gsmsg_off, kgnilnd_fmablk_state2str(state),
			   avail_mboxs, held_mboxs, num_mboxs, blk_size,
			   fmablk, hndl.qword1, hndl.qword2);
	} else {
		seq_printf(s, "%5d %4s %6d/%5d/%5d %9d %18p %37s\n",
			   (int) gseq->gsmsg_off, kgnilnd_fmablk_state2str(state),
			   avail_mboxs, held_mboxs, num_mboxs, blk_size,
			   fmablk, "PURGATORY.HOLD");
	}

	return 0;
}

static struct seq_operations kgn_smsg_sops = {
	.start = kgnilnd_smsg_seq_start,
	.stop  = kgnilnd_smsg_seq_stop,
	.next  = kgnilnd_smsg_seq_next,
	.show  = kgnilnd_smsg_seq_show,

};

static int
kgnilnd_smsg_seq_open(struct inode *inode, struct file *file)
{
	struct seq_file       *sf;
	int                    rc;

	rc = seq_open(file, &kgn_smsg_sops);
	if (rc == 0) {
		sf = file->private_data;
		sf->private = PDE_DATA(inode);
	}

	return rc;
}

static struct file_operations kgn_smsg_fops = {
	.owner   = THIS_MODULE,
	.open    = kgnilnd_smsg_seq_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = seq_release,
};

typedef struct {
	__u64                   gconn_version;
	struct list_head       *gconn_list;
	kgn_conn_t             *gconn_conn;
	loff_t                  gconn_off;
	int                     gconn_hashidx;
} kgn_conn_seq_iter_t;

int
kgnilnd_conn_seq_seek(kgn_conn_seq_iter_t *gseq, loff_t off)
{
	struct list_head       *list, *tmp;
	loff_t                  here = 0;
	int                     rc = 0;

	if (off == 0) {
		gseq->gconn_hashidx = 0;
		gseq->gconn_list = NULL;
	}

	if (off > atomic_read(&kgnilnd_data.kgn_nconns)) {
		gseq->gconn_list = NULL;
		rc = -ENOENT;
	}

	read_lock(&kgnilnd_data.kgn_peer_conn_lock);
	if (gseq->gconn_list != NULL &&
		gseq->gconn_version != kgnilnd_data.kgn_conn_version) {
		/* list changed */
		rc = -ESTALE;
		goto out;
	}

	if ((gseq->gconn_list == NULL) ||
		(gseq->gconn_off > off) ||
		(gseq->gconn_hashidx >= *kgnilnd_tunables.kgn_peer_hash_size)) {
		/* search from start */
		gseq->gconn_hashidx = 0;
		list = &kgnilnd_data.kgn_conns[gseq->gconn_hashidx];
		here = 0;
	} else {
		/* continue current search */
		list = gseq->gconn_list;
	}

	gseq->gconn_version = kgnilnd_data.kgn_conn_version;
	gseq->gconn_off = off;

start_list:

	list_for_each(tmp, list) {
		if (here == off) {
			kgn_conn_t *conn;
			conn = list_entry(tmp, kgn_conn_t, gnc_hashlist);
			gseq->gconn_conn = conn;
			rc = 0;
			goto out;
		}
		here++;
	}
	/* if we got through this hash bucket with 'off' still to go, try next*/
	gseq->gconn_hashidx++;
	if ((here <= off) &&
		(gseq->gconn_hashidx < *kgnilnd_tunables.kgn_peer_hash_size)) {
		list = &kgnilnd_data.kgn_conns[gseq->gconn_hashidx];
		goto start_list;
	}

	gseq->gconn_list = NULL;
	rc = -ENOENT;
out:
	read_unlock(&kgnilnd_data.kgn_peer_conn_lock);
	return rc;
}

static void *
kgnilnd_conn_seq_start(struct seq_file *s, loff_t *pos)
{

	kgn_conn_seq_iter_t     *gseq;
	int                      rc;

	if (kgnilnd_data.kgn_init < GNILND_INIT_ALL) {
		return NULL;
	}

	LIBCFS_ALLOC(gseq, sizeof(*gseq));
	if (gseq == NULL) {
		CERROR("could not allocate conn sequence iterator\n");
		return NULL;
	}

	/* only doing device 0 for now */
	gseq->gconn_list = NULL;
	rc = kgnilnd_conn_seq_seek(gseq, *pos);
	if (rc == 0)
		return gseq;

	LIBCFS_FREE(gseq, sizeof(*gseq));
	return NULL;
}

static void
kgnilnd_conn_seq_stop(struct seq_file *s, void *iter)
{
	kgn_conn_seq_iter_t     *gseq = iter;

	if (gseq != NULL)
		LIBCFS_FREE(gseq, sizeof(*gseq));
}

static void *
kgnilnd_conn_seq_next(struct seq_file *s, void *iter, loff_t *pos)
{
	kgn_conn_seq_iter_t    *gseq = iter;
	int                     rc;
	loff_t                  next = *pos + 1;

	rc = kgnilnd_conn_seq_seek(gseq, next);
	if (rc != 0) {
		LIBCFS_FREE(gseq, sizeof(*gseq));
		return NULL;
	}
	*pos = next;
	return gseq;
}

static int
kgnilnd_conn_seq_show(struct seq_file *s, void *iter)
{
	kgn_conn_seq_iter_t    *gseq = iter;
	kgn_peer_t             *peer = NULL;
	kgn_conn_t             *conn;

	/* there is no header data for conns, so offset 0 is the first
	 * real entry. */

	conn = gseq->gconn_conn;
	LASSERT(conn != NULL);

	read_lock(&kgnilnd_data.kgn_peer_conn_lock);
	if (gseq->gconn_list != NULL &&
		gseq->gconn_version != kgnilnd_data.kgn_conn_version) {
		/* list changed */
		read_unlock(&kgnilnd_data.kgn_peer_conn_lock);
		return -ESTALE;
	}

	/* instead of saving off the data, just refcount */
	kgnilnd_conn_addref(conn);
	if (conn->gnc_peer) {
		/* don't use link - after unlock it could get nuked */
		peer = conn->gnc_peer;
		kgnilnd_peer_addref(peer);
	}

	read_unlock(&kgnilnd_data.kgn_peer_conn_lock);

	seq_printf(s, "%p->%s [%d] q %d/%d/%d "
		"tx sq %u %dms/%dms "
		"rx sq %u %dms/%dms "
		"noop r/s %d/%d w/s/cq %lds/%lds/%lds "
		"sched a/d %lds/%lds "
		"tx_re "LPD64" TO %ds %s\n",
		conn, peer ? libcfs_nid2str(peer->gnp_nid) : "<?>",
		atomic_read(&conn->gnc_refcount),
		kgnilnd_count_list(&conn->gnc_fmaq),
		atomic_read(&conn->gnc_nlive_fma),
		atomic_read(&conn->gnc_nlive_rdma),
		conn->gnc_tx_seq,
		jiffies_to_msecs(jiffies - conn->gnc_last_tx),
		jiffies_to_msecs(jiffies - conn->gnc_last_tx_cq),
		conn->gnc_rx_seq,
		jiffies_to_msecs(jiffies - conn->gnc_last_rx),
		jiffies_to_msecs(jiffies - conn->gnc_last_rx_cq),
		atomic_read(&conn->gnc_reaper_noop),
		atomic_read(&conn->gnc_sched_noop),
		cfs_duration_sec(jiffies - conn->gnc_last_noop_want),
		cfs_duration_sec(jiffies - conn->gnc_last_noop_sent),
		cfs_duration_sec(jiffies - conn->gnc_last_noop_cq),
		cfs_duration_sec(jiffies - conn->gnc_last_sched_ask),
		cfs_duration_sec(jiffies - conn->gnc_last_sched_do),
		conn->gnc_tx_retrans, conn->gnc_timeout,
		kgnilnd_conn_state2str(conn));

	if (peer)
		kgnilnd_peer_decref(peer);
	kgnilnd_conn_decref(conn);

	return 0;
}

static struct seq_operations kgn_conn_sops = {
	.start = kgnilnd_conn_seq_start,
	.stop  = kgnilnd_conn_seq_stop,
	.next  = kgnilnd_conn_seq_next,
	.show  = kgnilnd_conn_seq_show,

};

#define KGN_DEBUG_PEER_NID_DEFAULT -1
static int kgnilnd_debug_peer_nid = KGN_DEBUG_PEER_NID_DEFAULT;

static int
kgnilnd_proc_peer_conns_write(struct file *file, const char *ubuffer,
			      unsigned long count, void *data)
{
	char dummy[8];
	int  rc;

	if (count >= sizeof(dummy) || count == 0)
		return -EINVAL;

	if (copy_from_user(dummy, ubuffer, count))
		return -EFAULT;

	rc = sscanf(dummy, "%d", &kgnilnd_debug_peer_nid);

	if (rc != 1) {
		return -EINVAL;
	}

	RETURN(count);
}

/* debug data to print from conns associated with peer nid
  -  date/time
  -  peer nid
  -  mbox_addr (msg_buffer + mbox_offset)
  -  gnc_dgram_type
  -  gnc_in_purgatory
  -  gnc_state
  -  gnc_error
  -  gnc_peer_error
  -  gnc_tx_seq
  -  gnc_last_tx
  -  gnc_last_tx_cq
  -  gnc_rx_seq
  -  gnc_first_rx
  -  gnc_last_rx
  -  gnc_last_rx_cq
  -  gnc_tx_retrans
  -  gnc_close_sent
  -  gnc_close_recvd
*/

static int
kgnilnd_proc_peer_conns_read(char *page, char **start, off_t off,
			     int count, int *eof, void *data)
{
	kgn_peer_t      *peer;
	kgn_conn_t      *conn;
	struct tm       ctm;
	struct timespec now;
	unsigned long   jifs;
	int             len = 0;
	int             rc;

	if (kgnilnd_debug_peer_nid == KGN_DEBUG_PEER_NID_DEFAULT) {
		rc = sprintf(page, "peer_conns not initialized\n");
		return rc;
	}

	/* sample date/time stamp - print time in UTC
	 * 2012-12-11T16:06:16.966751 123@gni ...
	 */
	getnstimeofday(&now);
	time_to_tm(now.tv_sec, 0, &ctm);
	jifs = jiffies;

	write_lock(&kgnilnd_data.kgn_peer_conn_lock);
	peer = kgnilnd_find_peer_locked(kgnilnd_debug_peer_nid);

	if (peer == NULL) {
		rc = sprintf(page, "peer not found for this nid %d\n",
			     kgnilnd_debug_peer_nid);
		write_unlock(&kgnilnd_data.kgn_peer_conn_lock);
		return rc;
	}

	list_for_each_entry(conn, &peer->gnp_conns, gnc_list) {
		len += scnprintf(page, count - len,
			"%04ld-%02d-%02dT%02d:%02d:%02d.%06ld %s "
			"mbox adr %p "
			"dg type %s "
			"%s "
			"purg %d "
			"close s/r %d/%d "
			"err %d peer err %d "
			"tx sq %u %dms/%dms "
			"rx sq %u %dms/%dms/%dms "
			"tx retran %lld\n",
			ctm.tm_year+1900, ctm.tm_mon+1, ctm.tm_mday,
			ctm.tm_hour, ctm.tm_min, ctm.tm_sec, now.tv_nsec,
			libcfs_nid2str(peer->gnp_nid),
			conn->remote_mbox_addr,
			kgnilnd_conn_dgram_type2str(conn->gnc_dgram_type),
			kgnilnd_conn_state2str(conn),
			conn->gnc_in_purgatory,
			conn->gnc_close_sent,
			conn->gnc_close_recvd,
			conn->gnc_error,
			conn->gnc_peer_error,
			conn->gnc_tx_seq,
			jiffies_to_msecs(jifs - conn->gnc_last_tx),
			jiffies_to_msecs(jifs - conn->gnc_last_tx_cq),
			conn->gnc_rx_seq,
			jiffies_to_msecs(jifs - conn->gnc_first_rx),
			jiffies_to_msecs(jifs - conn->gnc_last_rx),
			jiffies_to_msecs(jifs - conn->gnc_last_rx_cq),
			conn->gnc_tx_retrans);
	}

	write_unlock(&kgnilnd_data.kgn_peer_conn_lock);
	return len;
}

static int
kgnilnd_conn_seq_open(struct inode *inode, struct file *file)
{
	struct seq_file       *sf;
	int                    rc;

	rc = seq_open(file, &kgn_conn_sops);
	if (rc == 0) {
		sf = file->private_data;
		sf->private = PDE_DATA(inode);
	}

	return rc;
}

static struct file_operations kgn_conn_fops = {
	.owner   = THIS_MODULE,
	.open    = kgnilnd_conn_seq_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = seq_release,
};

typedef struct {
	__u64                   gpeer_version;
	struct list_head       *gpeer_list;
	kgn_peer_t             *gpeer_peer;
	loff_t                  gpeer_off;
	int                     gpeer_hashidx;
} kgn_peer_seq_iter_t;

int
kgnilnd_peer_seq_seek(kgn_peer_seq_iter_t *gseq, loff_t off)
{
	struct list_head       *list, *tmp;
	loff_t                  here = 0;
	int                     rc = 0;

	if (off == 0) {
		gseq->gpeer_hashidx = 0;
		gseq->gpeer_list = NULL;
	}

	if (off > atomic_read(&kgnilnd_data.kgn_npeers)) {
		gseq->gpeer_list = NULL;
		rc = -ENOENT;
	}

	read_lock(&kgnilnd_data.kgn_peer_conn_lock);
	if (gseq->gpeer_list != NULL &&
		gseq->gpeer_version != kgnilnd_data.kgn_peer_version) {
		/* list changed */
		rc = -ESTALE;
		goto out;
	}

	if ((gseq->gpeer_list == NULL) ||
		(gseq->gpeer_off > off) ||
		(gseq->gpeer_hashidx >= *kgnilnd_tunables.kgn_peer_hash_size)) {
		/* search from start */
		gseq->gpeer_hashidx = 0;
		list = &kgnilnd_data.kgn_peers[gseq->gpeer_hashidx];
		here = 0;
	} else {
		/* continue current search */
		list = gseq->gpeer_list;
	}

	gseq->gpeer_version = kgnilnd_data.kgn_peer_version;
	gseq->gpeer_off = off;

start_list:

	list_for_each(tmp, list) {
		if (here == off) {
			kgn_peer_t *peer;
			peer = list_entry(tmp, kgn_peer_t, gnp_list);
			gseq->gpeer_peer = peer;
			rc = 0;
			goto out;
		}
		here++;
	}
	/* if we got through this hash bucket with 'off' still to go, try next*/
	gseq->gpeer_hashidx++;
	if ((here <= off) &&
		(gseq->gpeer_hashidx < *kgnilnd_tunables.kgn_peer_hash_size)) {
		list = &kgnilnd_data.kgn_peers[gseq->gpeer_hashidx];
		goto start_list;
	}

	gseq->gpeer_list = NULL;
	rc = -ENOENT;
out:
	read_unlock(&kgnilnd_data.kgn_peer_conn_lock);
	return rc;
}

static void *
kgnilnd_peer_seq_start(struct seq_file *s, loff_t *pos)
{

	kgn_peer_seq_iter_t     *gseq;
	int                      rc;

	if (kgnilnd_data.kgn_init < GNILND_INIT_ALL) {
		return NULL;
	}

	LIBCFS_ALLOC(gseq, sizeof(*gseq));
	if (gseq == NULL) {
		CERROR("could not allocate peer sequence iterator\n");
		return NULL;
	}

	/* only doing device 0 for now */
	gseq->gpeer_list = NULL;
	rc = kgnilnd_peer_seq_seek(gseq, *pos);
	if (rc == 0)
		return gseq;

	LIBCFS_FREE(gseq, sizeof(*gseq));
	return NULL;
}

static void
kgnilnd_peer_seq_stop(struct seq_file *s, void *iter)
{
	kgn_peer_seq_iter_t     *gseq = iter;

	if (gseq != NULL)
		LIBCFS_FREE(gseq, sizeof(*gseq));
}

static void *
kgnilnd_peer_seq_next(struct seq_file *s, void *iter, loff_t *pos)
{
	kgn_peer_seq_iter_t    *gseq = iter;
	int                     rc;
	loff_t                  next = *pos + 1;

	rc = kgnilnd_peer_seq_seek(gseq, next);
	if (rc != 0) {
		LIBCFS_FREE(gseq, sizeof(*gseq));
		return NULL;
	}
	*pos = next;
	return gseq;
}

static int
kgnilnd_peer_seq_show(struct seq_file *s, void *iter)
{
	kgn_peer_seq_iter_t    *gseq = iter;
	kgn_peer_t             *peer;
	kgn_conn_t             *conn;
	char                   conn_str;
	int                    purg_count = 0;
	/* there is no header data for peers, so offset 0 is the first
	 * real entry. */

	peer = gseq->gpeer_peer;
	LASSERT(peer != NULL);

	read_lock(&kgnilnd_data.kgn_peer_conn_lock);
	if (gseq->gpeer_list != NULL &&
		gseq->gpeer_version != kgnilnd_data.kgn_peer_version) {
		/* list changed */
		read_unlock(&kgnilnd_data.kgn_peer_conn_lock);
		return -ESTALE;
	}

	/* instead of saving off the data, just refcount */
	kgnilnd_peer_addref(peer);
	conn = kgnilnd_find_conn_locked(peer);

	if (peer->gnp_connecting) {
		conn_str = 'S';
	} else if (conn != NULL) {
		conn_str = 'C';
	} else {
		conn_str = 'D';
	}

	list_for_each_entry(conn, &peer->gnp_conns, gnc_list) {
		if (conn->gnc_in_purgatory) {
			purg_count++;
		}
	}

	read_unlock(&kgnilnd_data.kgn_peer_conn_lock);

	seq_printf(s, "%p->%s [%d] %s NIC 0x%x q %d conn %c purg %d "
		"last %d@%dms dgram %d@%dms "
		"reconn %dms to %lus \n",
		peer, libcfs_nid2str(peer->gnp_nid),
		atomic_read(&peer->gnp_refcount),
		(peer->gnp_down == GNILND_RCA_NODE_DOWN) ? "down" : "up",
		peer->gnp_host_id,
		kgnilnd_count_list(&peer->gnp_tx_queue),
		conn_str,
		purg_count,
		peer->gnp_last_errno,
		jiffies_to_msecs(jiffies - peer->gnp_last_alive),
		peer->gnp_last_dgram_errno,
		jiffies_to_msecs(jiffies - peer->gnp_last_dgram_time),
		peer->gnp_reconnect_interval != 0
			? jiffies_to_msecs(jiffies - peer->gnp_reconnect_time)
			: 0,
		peer->gnp_reconnect_interval);

	kgnilnd_peer_decref(peer);

	return 0;
}

static struct seq_operations kgn_peer_sops = {
	.start = kgnilnd_peer_seq_start,
	.stop  = kgnilnd_peer_seq_stop,
	.next  = kgnilnd_peer_seq_next,
	.show  = kgnilnd_peer_seq_show,
};

static int
kgnilnd_peer_seq_open(struct inode *inode, struct file *file)
{
	struct seq_file       *sf;
	int                    rc;

	rc = seq_open(file, &kgn_peer_sops);
	if (rc == 0) {
		sf = file->private_data;
		sf->private = PDE_DATA(inode);
	}

	return rc;
}

static struct file_operations kgn_peer_fops = {
	.owner   = THIS_MODULE,
	.open    = kgnilnd_peer_seq_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = seq_release,
};

static struct proc_dir_entry *kgn_proc_root;

void
kgnilnd_proc_init(void)
{
	struct proc_dir_entry *pde;
	int             rc = 0;
	ENTRY;

	/* setup dir */
	kgn_proc_root = proc_mkdir(libcfs_lnd2modname(GNILND), NULL);
	if (kgn_proc_root == NULL) {
		CERROR("couldn't create proc dir %s\n",
			libcfs_lnd2modname(GNILND));
		return;
	}

	/* Initialize CKSUM_TEST */
	pde = create_proc_entry(GNILND_PROC_CKSUM_TEST, 0200, kgn_proc_root);
	if (pde == NULL) {
		CERROR("couldn't create proc entry %s\n", GNILND_PROC_CKSUM_TEST);
		rc = -ENOENT;
		GOTO(remove_dir, rc);
	}

	pde->data = NULL;
	pde->write_proc = kgnilnd_proc_cksum_test_write;

	/* Initialize STATS */
	pde = create_proc_entry(GNILND_PROC_STATS, 0644, kgn_proc_root);
	if (pde == NULL) {
		CERROR("couldn't create proc entry %s\n", GNILND_PROC_STATS);
		rc = -ENOENT;
		GOTO(remove_test, rc);
	}

	pde->data = NULL;
	pde->read_proc = kgnilnd_proc_stats_read;
	pde->write_proc = kgnilnd_proc_stats_write;

	/* Initialize MDD */
	pde = create_proc_entry(GNILND_PROC_MDD, 0444, kgn_proc_root);
	if (pde == NULL) {
		CERROR("couldn't create proc entry %s\n", GNILND_PROC_MDD);
		rc = -ENOENT;
		GOTO(remove_stats, rc);
	}

	pde->data = NULL;
	pde->proc_fops = &kgn_mdd_fops;

	/* Initialize SMSG */
	pde = create_proc_entry(GNILND_PROC_SMSG, 0444, kgn_proc_root);
	if (pde == NULL) {
		CERROR("couldn't create proc entry %s\n", GNILND_PROC_SMSG);
		rc = -ENOENT;
		GOTO(remove_mdd, rc);
	}

	pde->data = NULL;
	pde->proc_fops = &kgn_smsg_fops;

	/* Initialize CONN */
	pde = create_proc_entry(GNILND_PROC_CONN, 0444, kgn_proc_root);
	if (pde == NULL) {
		CERROR("couldn't create proc entry %s\n", GNILND_PROC_CONN);
		rc = -ENOENT;
		GOTO(remove_smsg, rc);
	}

	pde->data = NULL;
	pde->proc_fops = &kgn_conn_fops;

	/* Initialize peer conns debug */
	pde = create_proc_entry(GNILND_PROC_PEER_CONNS, 0644, kgn_proc_root);
	if (pde == NULL) {
		CERROR("couldn't create proc entry %s\n", GNILND_PROC_PEER_CONNS);
		rc = -ENOENT;
		GOTO(remove_conn, rc);
	}

	pde->data = NULL;
	pde->read_proc = kgnilnd_proc_peer_conns_read;
	pde->write_proc = kgnilnd_proc_peer_conns_write;

	/* Initialize PEER */
	pde = create_proc_entry(GNILND_PROC_PEER, 0444, kgn_proc_root);
	if (pde == NULL) {
		CERROR("couldn't create proc entry %s\n", GNILND_PROC_PEER);
		rc = -ENOENT;
		GOTO(remove_pc, rc);
	}

	pde->data = NULL;
	pde->proc_fops = &kgn_peer_fops;
	RETURN_EXIT;

remove_pc:
	remove_proc_entry(GNILND_PROC_PEER_CONNS, kgn_proc_root);
remove_conn:
	remove_proc_entry(GNILND_PROC_CONN, kgn_proc_root);
remove_smsg:
	remove_proc_entry(GNILND_PROC_SMSG, kgn_proc_root);
remove_mdd:
	remove_proc_entry(GNILND_PROC_MDD, kgn_proc_root);
remove_stats:
	remove_proc_entry(GNILND_PROC_STATS, kgn_proc_root);
remove_test:
	remove_proc_entry(GNILND_PROC_CKSUM_TEST, kgn_proc_root);
remove_dir:
	remove_proc_entry(kgn_proc_root->name, NULL);

	RETURN_EXIT;
}

void
kgnilnd_proc_fini(void)
{
	remove_proc_entry(GNILND_PROC_PEER_CONNS, kgn_proc_root);
	remove_proc_entry(GNILND_PROC_PEER, kgn_proc_root);
	remove_proc_entry(GNILND_PROC_CONN, kgn_proc_root);
	remove_proc_entry(GNILND_PROC_MDD, kgn_proc_root);
	remove_proc_entry(GNILND_PROC_SMSG, kgn_proc_root);
	remove_proc_entry(GNILND_PROC_STATS, kgn_proc_root);
	remove_proc_entry(GNILND_PROC_CKSUM_TEST, kgn_proc_root);
	remove_proc_entry(kgn_proc_root->name, NULL);
}
