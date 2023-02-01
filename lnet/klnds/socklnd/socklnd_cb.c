/*
 * Copyright (c) 2003, 2010, Oracle and/or its affiliates. All rights reserved.
 *
 * Copyright (c) 2011, 2017, Intel Corporation.
 *
 *   Author: Zach Brown <zab@zabbo.net>
 *   Author: Peter J. Braam <braam@clusterfs.com>
 *   Author: Phil Schwan <phil@clusterfs.com>
 *   Author: Eric Barton <eric@bartonsoftware.com>
 *
 *   This file is part of Lustre, https://wiki.whamcloud.com/
 *
 *   Portals is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Portals is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Portals; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <libcfs/linux/linux-mem.h>
#include "socklnd.h"
#include <linux/sunrpc/addr.h>

struct ksock_tx *
ksocknal_alloc_tx(int type, int size)
{
	struct ksock_tx *tx = NULL;

	if (type == KSOCK_MSG_NOOP) {
		LASSERT(size == KSOCK_NOOP_TX_SIZE);

		/* searching for a noop tx in free list */
		spin_lock(&ksocknal_data.ksnd_tx_lock);

		tx = list_first_entry_or_null(&ksocknal_data.ksnd_idle_noop_txs,
					      struct ksock_tx, tx_list);
		if (tx) {
			LASSERT(tx->tx_desc_size == size);
			list_del(&tx->tx_list);
		}

		spin_unlock(&ksocknal_data.ksnd_tx_lock);
        }

        if (tx == NULL)
                LIBCFS_ALLOC(tx, size);

        if (tx == NULL)
                return NULL;

	refcount_set(&tx->tx_refcount, 1);
	tx->tx_zc_aborted = 0;
	tx->tx_zc_capable = 0;
	tx->tx_zc_checked = 0;
	tx->tx_hstatus = LNET_MSG_STATUS_OK;
	tx->tx_desc_size  = size;

	atomic_inc(&ksocknal_data.ksnd_nactive_txs);

	return tx;
}

struct ksock_tx *
ksocknal_alloc_tx_noop(__u64 cookie, int nonblk)
{
	struct ksock_tx *tx;

	tx = ksocknal_alloc_tx(KSOCK_MSG_NOOP, KSOCK_NOOP_TX_SIZE);
	if (tx == NULL) {
		CERROR("Can't allocate noop tx desc\n");
		return NULL;
	}

	tx->tx_conn     = NULL;
	tx->tx_lnetmsg  = NULL;
	tx->tx_kiov     = NULL;
	tx->tx_nkiov    = 0;
	tx->tx_niov     = 1;
	tx->tx_nonblk   = nonblk;

	tx->tx_msg.ksm_csum = 0;
	tx->tx_msg.ksm_type = KSOCK_MSG_NOOP;
	tx->tx_msg.ksm_zc_cookies[0] = 0;
	tx->tx_msg.ksm_zc_cookies[1] = cookie;

	return tx;
}


void
ksocknal_free_tx(struct ksock_tx *tx)
{
	atomic_dec(&ksocknal_data.ksnd_nactive_txs);

	if (tx->tx_lnetmsg == NULL && tx->tx_desc_size == KSOCK_NOOP_TX_SIZE) {
		/* it's a noop tx */
		spin_lock(&ksocknal_data.ksnd_tx_lock);

		list_add(&tx->tx_list, &ksocknal_data.ksnd_idle_noop_txs);

		spin_unlock(&ksocknal_data.ksnd_tx_lock);
	} else {
		LIBCFS_FREE(tx, tx->tx_desc_size);
	}
}

static int
ksocknal_send_hdr(struct ksock_conn *conn, struct ksock_tx *tx,
		  struct kvec *scratch_iov)
{
	struct kvec *iov = &tx->tx_hdr;
	int    nob;
	int    rc;

	LASSERT(tx->tx_niov > 0);

	/* Never touch tx->tx_hdr inside ksocknal_lib_send_hdr() */
	rc = ksocknal_lib_send_hdr(conn, tx, scratch_iov);

	if (rc <= 0)                            /* sent nothing? */
		return rc;

	nob = rc;
	LASSERT(nob <= tx->tx_resid);
	tx->tx_resid -= nob;

	/* "consume" iov */
	LASSERT(tx->tx_niov == 1);

	if (nob < (int) iov->iov_len) {
		iov->iov_base += nob;
		iov->iov_len -= nob;
		return rc;
	}

	LASSERT(nob == iov->iov_len);
	tx->tx_niov--;

	return rc;
}

static int
ksocknal_send_kiov(struct ksock_conn *conn, struct ksock_tx *tx,
		   struct kvec *scratch_iov)
{
	struct bio_vec *kiov = tx->tx_kiov;
	int nob;
	int rc;

	LASSERT(tx->tx_niov == 0);
	LASSERT(tx->tx_nkiov > 0);

	/* Never touch tx->tx_kiov inside ksocknal_lib_send_kiov() */
	rc = ksocknal_lib_send_kiov(conn, tx, scratch_iov);

	if (rc <= 0)                            /* sent nothing? */
		return rc;

	nob = rc;
	LASSERT(nob <= tx->tx_resid);
	tx->tx_resid -= nob;

	/* "consume" kiov */
	do {
		LASSERT(tx->tx_nkiov > 0);

		if (nob < (int)kiov->bv_len) {
			kiov->bv_offset += nob;
			kiov->bv_len -= nob;
			return rc;
		}

		nob -= (int)kiov->bv_len;
		tx->tx_kiov = ++kiov;
		tx->tx_nkiov--;
	} while (nob != 0);

	return rc;
}

static int
ksocknal_transmit(struct ksock_conn *conn, struct ksock_tx *tx,
		  struct kvec *scratch_iov)
{
	int	rc;
	int	bufnob;

	if (ksocknal_data.ksnd_stall_tx != 0)
		schedule_timeout_uninterruptible(
			cfs_time_seconds(ksocknal_data.ksnd_stall_tx));

	LASSERT(tx->tx_resid != 0);

	rc = ksocknal_connsock_addref(conn);
	if (rc != 0) {
		LASSERT(conn->ksnc_closing);
		return -ESHUTDOWN;
	}

	do {
		if (ksocknal_data.ksnd_enomem_tx > 0) {
			/* testing... */
			ksocknal_data.ksnd_enomem_tx--;
			rc = -EAGAIN;
		} else if (tx->tx_niov != 0) {
			rc = ksocknal_send_hdr(conn, tx, scratch_iov);
		} else {
			rc = ksocknal_send_kiov(conn, tx, scratch_iov);
		}

		bufnob = conn->ksnc_sock->sk->sk_wmem_queued;
		if (rc > 0)                     /* sent something? */
			conn->ksnc_tx_bufnob += rc; /* account it */

		if (bufnob < conn->ksnc_tx_bufnob) {
			/* allocated send buffer bytes < computed; infer
			 * something got ACKed */
			conn->ksnc_tx_deadline = ktime_get_seconds() +
						 ksocknal_timeout();
			conn->ksnc_peer->ksnp_last_alive = ktime_get_seconds();
			conn->ksnc_tx_bufnob = bufnob;
			smp_mb();
		}

		if (rc <= 0) { /* Didn't write anything? */
			/* some stacks return 0 instead of -EAGAIN */
			if (rc == 0)
				rc = -EAGAIN;

			/* Check if EAGAIN is due to memory pressure */
			if (rc == -EAGAIN && ksocknal_lib_memory_pressure(conn))
				rc = -ENOMEM;

			break;
		}

		/* socket's wmem_queued now includes 'rc' bytes */
		atomic_sub (rc, &conn->ksnc_tx_nob);
		rc = 0;

	} while (tx->tx_resid != 0);

	ksocknal_connsock_decref(conn);
	return rc;
}

static int
ksocknal_recv_iov(struct ksock_conn *conn, struct kvec *scratchiov)
{
	struct kvec *iov = conn->ksnc_rx_iov;
	int     nob;
	int     rc;

	LASSERT(conn->ksnc_rx_niov > 0);

	/* Never touch conn->ksnc_rx_iov or change connection
	 * status inside ksocknal_lib_recv_iov */
	rc = ksocknal_lib_recv_iov(conn, scratchiov);

	if (rc <= 0)
		return rc;

	/* received something... */
	nob = rc;

	conn->ksnc_peer->ksnp_last_alive = ktime_get_seconds();
	conn->ksnc_rx_deadline = ktime_get_seconds() +
				 ksocknal_timeout();
	smp_mb();                       /* order with setting rx_started */
	conn->ksnc_rx_started = 1;

	conn->ksnc_rx_nob_wanted -= nob;
	conn->ksnc_rx_nob_left -= nob;

	do {
		LASSERT(conn->ksnc_rx_niov > 0);

		if (nob < (int)iov->iov_len) {
			iov->iov_len -= nob;
			iov->iov_base += nob;
			return -EAGAIN;
		}

		nob -= iov->iov_len;
		conn->ksnc_rx_iov = ++iov;
		conn->ksnc_rx_niov--;
	} while (nob != 0);

	return rc;
}

static int
ksocknal_recv_kiov(struct ksock_conn *conn, struct page **rx_scratch_pgs,
		   struct kvec *scratch_iov)
{
	struct bio_vec *kiov = conn->ksnc_rx_kiov;
	int nob;
	int rc;

	LASSERT(conn->ksnc_rx_nkiov > 0);
	/* Never touch conn->ksnc_rx_kiov or change connection
	 * status inside ksocknal_lib_recv_iov */
	rc = ksocknal_lib_recv_kiov(conn, rx_scratch_pgs, scratch_iov);

	if (rc <= 0)
		return rc;

	/* received something... */
	nob = rc;

	conn->ksnc_peer->ksnp_last_alive = ktime_get_seconds();
	conn->ksnc_rx_deadline = ktime_get_seconds() +
				 ksocknal_timeout();
	smp_mb();                       /* order with setting rx_started */
	conn->ksnc_rx_started = 1;

	conn->ksnc_rx_nob_wanted -= nob;
	conn->ksnc_rx_nob_left -= nob;

	do {
		LASSERT(conn->ksnc_rx_nkiov > 0);

		if (nob < (int) kiov->bv_len) {
			kiov->bv_offset += nob;
			kiov->bv_len -= nob;
			return -EAGAIN;
		}

		nob -= kiov->bv_len;
		conn->ksnc_rx_kiov = ++kiov;
		conn->ksnc_rx_nkiov--;
	} while (nob != 0);

	return 1;
}

static int
ksocknal_receive(struct ksock_conn *conn, struct page **rx_scratch_pgs,
		 struct kvec *scratch_iov)
{
	/* Return 1 on success, 0 on EOF, < 0 on error.
	 * Caller checks ksnc_rx_nob_wanted to determine
	 * progress/completion. */
	int     rc;
	ENTRY;

	if (ksocknal_data.ksnd_stall_rx != 0)
		schedule_timeout_uninterruptible(
			cfs_time_seconds(ksocknal_data.ksnd_stall_rx));

	rc = ksocknal_connsock_addref(conn);
	if (rc != 0) {
		LASSERT(conn->ksnc_closing);
		return -ESHUTDOWN;
	}

	for (;;) {
		if (conn->ksnc_rx_niov != 0)
			rc = ksocknal_recv_iov(conn, scratch_iov);
		else
			rc = ksocknal_recv_kiov(conn, rx_scratch_pgs,
						 scratch_iov);

		if (rc <= 0) {
			/* error/EOF or partial receive */
			if (rc == -EAGAIN) {
				rc = 1;
			} else if (rc == 0 && conn->ksnc_rx_started) {
				/* EOF in the middle of a message */
				rc = -EPROTO;
			}
			break;
		}

		/* Completed a fragment */

		if (conn->ksnc_rx_nob_wanted == 0) {
			rc = 1;
			break;
		}
	}

	ksocknal_connsock_decref(conn);
	RETURN(rc);
}

void
ksocknal_tx_done(struct lnet_ni *ni, struct ksock_tx *tx, int rc)
{
	struct lnet_msg *lnetmsg = tx->tx_lnetmsg;
	enum lnet_msg_hstatus hstatus = tx->tx_hstatus;

	LASSERT(ni != NULL || tx->tx_conn != NULL);

	if (!rc && (tx->tx_resid != 0 || tx->tx_zc_aborted)) {
		rc = -EIO;
		if (hstatus == LNET_MSG_STATUS_OK)
			hstatus = LNET_MSG_STATUS_LOCAL_ERROR;
	}

	if (tx->tx_conn != NULL)
		ksocknal_conn_decref(tx->tx_conn);

	ksocknal_free_tx(tx);
	if (lnetmsg != NULL) { /* KSOCK_MSG_NOOP go without lnetmsg */
		lnetmsg->msg_health_status = hstatus;
		lnet_finalize(lnetmsg, rc);
	}
}

void
ksocknal_txlist_done(struct lnet_ni *ni, struct list_head *txlist, int error)
{
	struct ksock_tx *tx;

	while ((tx = list_first_entry_or_null(txlist, struct ksock_tx,
					      tx_list)) != NULL) {
		if (error && tx->tx_lnetmsg) {
			CNETERR("Deleting packet type %d len %d %s->%s\n",
				tx->tx_lnetmsg->msg_type,
				tx->tx_lnetmsg->msg_len,
				libcfs_nidstr(&tx->tx_lnetmsg->msg_initiator),
				libcfs_nidstr(&tx->tx_lnetmsg->msg_target.nid));
		} else if (error) {
			CNETERR("Deleting noop packet\n");
		}

		list_del(&tx->tx_list);

		if (tx->tx_hstatus == LNET_MSG_STATUS_OK) {
			if (error == -ETIMEDOUT)
				tx->tx_hstatus =
				  LNET_MSG_STATUS_LOCAL_TIMEOUT;
			else if (error == -ENETDOWN ||
				 error == -EHOSTUNREACH ||
				 error == -ENETUNREACH ||
				 error == -ECONNREFUSED ||
				 error == -ECONNRESET)
				tx->tx_hstatus = LNET_MSG_STATUS_REMOTE_DROPPED;
			/*
			 * for all other errors we don't want to
			 * retransmit
			 */
			else if (error)
				tx->tx_hstatus = LNET_MSG_STATUS_LOCAL_ERROR;
		}

		LASSERT(refcount_read(&tx->tx_refcount) == 1);
		ksocknal_tx_done(ni, tx, error);
	}
}

static void
ksocknal_check_zc_req(struct ksock_tx *tx)
{
	struct ksock_conn *conn = tx->tx_conn;
	struct ksock_peer_ni *peer_ni = conn->ksnc_peer;

        /* Set tx_msg.ksm_zc_cookies[0] to a unique non-zero cookie and add tx
         * to ksnp_zc_req_list if some fragment of this message should be sent
         * zero-copy.  Our peer_ni will send an ACK containing this cookie when
         * she has received this message to tell us we can signal completion.
         * tx_msg.ksm_zc_cookies[0] remains non-zero while tx is on
         * ksnp_zc_req_list. */
        LASSERT (tx->tx_msg.ksm_type != KSOCK_MSG_NOOP);
        LASSERT (tx->tx_zc_capable);

        tx->tx_zc_checked = 1;

        if (conn->ksnc_proto == &ksocknal_protocol_v1x ||
            !conn->ksnc_zc_capable)
                return;

        /* assign cookie and queue tx to pending list, it will be released when
         * a matching ack is received. See ksocknal_handle_zcack() */

        ksocknal_tx_addref(tx);

	spin_lock(&peer_ni->ksnp_lock);

        /* ZC_REQ is going to be pinned to the peer_ni */
	tx->tx_deadline = ktime_get_seconds() +
			  ksocknal_timeout();

        LASSERT (tx->tx_msg.ksm_zc_cookies[0] == 0);

        tx->tx_msg.ksm_zc_cookies[0] = peer_ni->ksnp_zc_next_cookie++;

        if (peer_ni->ksnp_zc_next_cookie == 0)
                peer_ni->ksnp_zc_next_cookie = SOCKNAL_KEEPALIVE_PING + 1;

	list_add_tail(&tx->tx_zc_list, &peer_ni->ksnp_zc_req_list);

	spin_unlock(&peer_ni->ksnp_lock);
}

static void
ksocknal_uncheck_zc_req(struct ksock_tx *tx)
{
	struct ksock_peer_ni *peer_ni = tx->tx_conn->ksnc_peer;

	LASSERT(tx->tx_msg.ksm_type != KSOCK_MSG_NOOP);
	LASSERT(tx->tx_zc_capable);

	tx->tx_zc_checked = 0;

	spin_lock(&peer_ni->ksnp_lock);

	if (tx->tx_msg.ksm_zc_cookies[0] == 0) {
		/* Not waiting for an ACK */
		spin_unlock(&peer_ni->ksnp_lock);
		return;
	}

	tx->tx_msg.ksm_zc_cookies[0] = 0;
	list_del(&tx->tx_zc_list);

	spin_unlock(&peer_ni->ksnp_lock);

	ksocknal_tx_decref(tx);
}

static int
ksocknal_process_transmit(struct ksock_conn *conn, struct ksock_tx *tx,
			  struct kvec *scratch_iov)
{
	int rc;
	bool error_sim = false;

	if (lnet_send_error_simulation(tx->tx_lnetmsg, &tx->tx_hstatus)) {
		error_sim = true;
		rc = -EINVAL;
		goto simulate_error;
	}

	if (tx->tx_zc_capable && !tx->tx_zc_checked)
		ksocknal_check_zc_req(tx);

	rc = ksocknal_transmit(conn, tx, scratch_iov);

	CDEBUG(D_NET, "send(%d) %d\n", tx->tx_resid, rc);

	if (tx->tx_resid == 0) {
		/* Sent everything OK */
		LASSERT(rc == 0);

		return 0;
	}

	if (rc == -EAGAIN)
		return rc;

	if (rc == -ENOMEM) {
		static int counter;

		counter++;   /* exponential backoff warnings */
		if ((counter & (-counter)) == counter)
			CWARN("%u ENOMEM tx %p (%lld allocated)\n",
			      counter, conn, libcfs_kmem_read());

		/* Queue on ksnd_enomem_conns for retry after a timeout */
		spin_lock_bh(&ksocknal_data.ksnd_reaper_lock);

		/* enomem list takes over scheduler's ref... */
		LASSERT(conn->ksnc_tx_scheduled);
		list_add_tail(&conn->ksnc_tx_list,
				  &ksocknal_data.ksnd_enomem_conns);
		if (ktime_get_seconds() + SOCKNAL_ENOMEM_RETRY <
		    ksocknal_data.ksnd_reaper_waketime)
			wake_up(&ksocknal_data.ksnd_reaper_waitq);

		spin_unlock_bh(&ksocknal_data.ksnd_reaper_lock);

		/*
		 * set the health status of the message which determines
		 * whether we should retry the transmit
		 */
		tx->tx_hstatus = LNET_MSG_STATUS_LOCAL_ERROR;
		return (rc);
	}

simulate_error:

	/* Actual error */
	LASSERT(rc < 0);

	if (!error_sim) {
		/*
		* set the health status of the message which determines
		* whether we should retry the transmit
		*/
		if (rc == -ETIMEDOUT)
			tx->tx_hstatus = LNET_MSG_STATUS_REMOTE_TIMEOUT;
		else
			tx->tx_hstatus = LNET_MSG_STATUS_LOCAL_ERROR;
	}

	if (!conn->ksnc_closing) {
		switch (rc) {
		case -ECONNRESET:
			LCONSOLE_WARN("Host %pIS reset our connection while we were sending data; it may have rebooted.\n",
				      &conn->ksnc_peeraddr);
			break;
		default:
			LCONSOLE_WARN("There was an unexpected network error while writing to %pIS: %d.\n",
				      &conn->ksnc_peeraddr, rc);
			break;
		}
		CDEBUG(D_NET, "[%p] Error %d on write to %s ip %pISp\n",
		       conn, rc, libcfs_idstr(&conn->ksnc_peer->ksnp_id),
		       &conn->ksnc_peeraddr);
	}

	if (tx->tx_zc_checked)
		ksocknal_uncheck_zc_req(tx);

	/* it's not an error if conn is being closed */
	ksocknal_close_conn_and_siblings(conn,
					  (conn->ksnc_closing) ? 0 : rc);

	return rc;
}

static void
ksocknal_launch_connection_locked(struct ksock_conn_cb *conn_cb)
{
	/* called holding write lock on ksnd_global_lock */

	LASSERT(!conn_cb->ksnr_scheduled);
	LASSERT(!conn_cb->ksnr_connecting);
	LASSERT((ksocknal_conn_cb_mask() & ~conn_cb->ksnr_connected) != 0);

	/* scheduling conn for connd */
	conn_cb->ksnr_scheduled = 1;

	/* extra ref for connd */
	ksocknal_conn_cb_addref(conn_cb);

	spin_lock_bh(&ksocknal_data.ksnd_connd_lock);

	list_add_tail(&conn_cb->ksnr_connd_list,
		      &ksocknal_data.ksnd_connd_routes);
	wake_up(&ksocknal_data.ksnd_connd_waitq);

	spin_unlock_bh(&ksocknal_data.ksnd_connd_lock);
}

void
ksocknal_launch_all_connections_locked(struct ksock_peer_ni *peer_ni)
{
	struct ksock_conn_cb *conn_cb;

	/* called holding write lock on ksnd_global_lock */
	for (;;) {
		/* launch any/all connections that need it */
		conn_cb = ksocknal_find_connectable_conn_cb_locked(peer_ni);
		if (conn_cb == NULL)
			return;

		ksocknal_launch_connection_locked(conn_cb);
	}
}

struct ksock_conn *
ksocknal_find_conn_locked(struct ksock_peer_ni *peer_ni, struct ksock_tx *tx, int nonblk)
{
	struct ksock_conn *c;
	struct ksock_conn *conn;
	struct ksock_conn *typed = NULL;
	struct ksock_conn *fallback = NULL;
	int tnob = 0;
	int fnob = 0;

	list_for_each_entry(c, &peer_ni->ksnp_conns, ksnc_list) {
		int nob = atomic_read(&c->ksnc_tx_nob) +
			  c->ksnc_sock->sk->sk_wmem_queued;
		int rc;

                LASSERT (!c->ksnc_closing);
                LASSERT (c->ksnc_proto != NULL &&
                         c->ksnc_proto->pro_match_tx != NULL);

                rc = c->ksnc_proto->pro_match_tx(c, tx, nonblk);

                switch (rc) {
                default:
                        LBUG();
                case SOCKNAL_MATCH_NO: /* protocol rejected the tx */
                        continue;

                case SOCKNAL_MATCH_YES: /* typed connection */
                        if (typed == NULL || tnob > nob ||
                            (tnob == nob && *ksocknal_tunables.ksnd_round_robin &&
			     typed->ksnc_tx_last_post > c->ksnc_tx_last_post)) {
                                typed = c;
                                tnob  = nob;
                        }
                        break;

                case SOCKNAL_MATCH_MAY: /* fallback connection */
                        if (fallback == NULL || fnob > nob ||
                            (fnob == nob && *ksocknal_tunables.ksnd_round_robin &&
			     fallback->ksnc_tx_last_post > c->ksnc_tx_last_post)) {
                                fallback = c;
                                fnob     = nob;
                        }
                        break;
                }
        }

        /* prefer the typed selection */
        conn = (typed != NULL) ? typed : fallback;

        if (conn != NULL)
		conn->ksnc_tx_last_post = ktime_get_seconds();

        return conn;
}

void
ksocknal_tx_prep(struct ksock_conn *conn, struct ksock_tx *tx)
{
        conn->ksnc_proto->pro_pack(tx);

	atomic_add (tx->tx_nob, &conn->ksnc_tx_nob);
        ksocknal_conn_addref(conn); /* +1 ref for tx */
        tx->tx_conn = conn;
}

void
ksocknal_queue_tx_locked(struct ksock_tx *tx, struct ksock_conn *conn)
{
	struct ksock_sched *sched = conn->ksnc_scheduler;
	struct ksock_msg *msg = &tx->tx_msg;
	struct ksock_tx *ztx = NULL;
	int bufnob = 0;

        /* called holding global lock (read or irq-write) and caller may
         * not have dropped this lock between finding conn and calling me,
         * so we don't need the {get,put}connsock dance to deref
         * ksnc_sock... */
        LASSERT(!conn->ksnc_closing);

	CDEBUG(D_NET, "Sending to %s ip %pISp\n",
	       libcfs_idstr(&conn->ksnc_peer->ksnp_id),
	       &conn->ksnc_peeraddr);

        ksocknal_tx_prep(conn, tx);

	/* Ensure the frags we've been given EXACTLY match the number of
	 * bytes we want to send.  Many TCP/IP stacks disregard any total
	 * size parameters passed to them and just look at the frags.
	 *
	 * We always expect at least 1 mapped fragment containing the
	 * complete ksocknal message header.
	 */
	LASSERT(lnet_iov_nob(tx->tx_niov, &tx->tx_hdr) +
		lnet_kiov_nob(tx->tx_nkiov, tx->tx_kiov) ==
		(unsigned int)tx->tx_nob);
	LASSERT(tx->tx_niov >= 1);
	LASSERT(tx->tx_resid == tx->tx_nob);

	CDEBUG(D_NET, "Packet %p type %d, nob %d niov %d nkiov %d\n",
	       tx, tx->tx_lnetmsg ? tx->tx_lnetmsg->msg_type : KSOCK_MSG_NOOP,
	       tx->tx_nob, tx->tx_niov, tx->tx_nkiov);

	bufnob = conn->ksnc_sock->sk->sk_wmem_queued;
	spin_lock_bh(&sched->kss_lock);

	if (list_empty(&conn->ksnc_tx_queue) && bufnob == 0) {
		/* First packet starts the timeout */
		conn->ksnc_tx_deadline = ktime_get_seconds() +
					 ksocknal_timeout();
		if (conn->ksnc_tx_bufnob > 0) /* something got ACKed */
			conn->ksnc_peer->ksnp_last_alive = ktime_get_seconds();
		conn->ksnc_tx_bufnob = 0;
		smp_mb(); /* order with adding to tx_queue */
	}

	if (msg->ksm_type == KSOCK_MSG_NOOP) {
		/* The packet is noop ZC ACK, try to piggyback the ack_cookie
		 * on a normal packet so I don't need to send it */
                LASSERT (msg->ksm_zc_cookies[1] != 0);
                LASSERT (conn->ksnc_proto->pro_queue_tx_zcack != NULL);

                if (conn->ksnc_proto->pro_queue_tx_zcack(conn, tx, 0))
                        ztx = tx; /* ZC ACK piggybacked on ztx release tx later */

        } else {
                /* It's a normal packet - can it piggback a noop zc-ack that
                 * has been queued already? */
                LASSERT (msg->ksm_zc_cookies[1] == 0);
                LASSERT (conn->ksnc_proto->pro_queue_tx_msg != NULL);

                ztx = conn->ksnc_proto->pro_queue_tx_msg(conn, tx);
                /* ztx will be released later */
        }

        if (ztx != NULL) {
		atomic_sub (ztx->tx_nob, &conn->ksnc_tx_nob);
		list_add_tail(&ztx->tx_list, &sched->kss_zombie_noop_txs);
        }

	if (conn->ksnc_tx_ready &&      /* able to send */
	    !conn->ksnc_tx_scheduled) { /* not scheduled to send */
		/* +1 ref for scheduler */
		ksocknal_conn_addref(conn);
		list_add_tail(&conn->ksnc_tx_list,
				   &sched->kss_tx_conns);
		conn->ksnc_tx_scheduled = 1;
		wake_up(&sched->kss_waitq);
	}

	spin_unlock_bh(&sched->kss_lock);
}


struct ksock_conn_cb *
ksocknal_find_connectable_conn_cb_locked(struct ksock_peer_ni *peer_ni)
{
	time64_t now = ktime_get_seconds();
	struct ksock_conn_cb *conn_cb;

	conn_cb = peer_ni->ksnp_conn_cb;
	if (!conn_cb)
		return NULL;

	LASSERT(!conn_cb->ksnr_connecting || conn_cb->ksnr_scheduled);

	if (conn_cb->ksnr_scheduled)	/* connections being established */
		return NULL;

	/* all conn types connected ? */
	if ((ksocknal_conn_cb_mask() & ~conn_cb->ksnr_connected) == 0)
		return NULL;

	if (!(conn_cb->ksnr_retry_interval == 0 || /* first attempt */
	      now >= conn_cb->ksnr_timeout)) {
		CDEBUG(D_NET,
		       "Too soon to retry route %pIS (cnted %d, interval %lld, %lld secs later)\n",
		       &conn_cb->ksnr_addr,
		       conn_cb->ksnr_connected,
		       conn_cb->ksnr_retry_interval,
		       conn_cb->ksnr_timeout - now);
		return NULL;
	}

	return conn_cb;
}

struct ksock_conn_cb *
ksocknal_find_connecting_conn_cb_locked(struct ksock_peer_ni *peer_ni)
{
	struct ksock_conn_cb *conn_cb;

	conn_cb = peer_ni->ksnp_conn_cb;
	if (!conn_cb)
		return NULL;

	LASSERT(!conn_cb->ksnr_connecting || conn_cb->ksnr_scheduled);

	return conn_cb->ksnr_scheduled ? conn_cb : NULL;
}

int
ksocknal_launch_packet(struct lnet_ni *ni, struct ksock_tx *tx,
		       struct lnet_processid *id)
{
	struct ksock_peer_ni *peer_ni;
	struct ksock_conn *conn;
	struct sockaddr_in sa;
	rwlock_t *g_lock;
	int retry;
	int rc;

	LASSERT(tx->tx_conn == NULL);

	g_lock = &ksocknal_data.ksnd_global_lock;

	for (retry = 0;; retry = 1) {
		read_lock(g_lock);
		peer_ni = ksocknal_find_peer_locked(ni, id);
		if (peer_ni != NULL) {
			if (ksocknal_find_connectable_conn_cb_locked(peer_ni) == NULL) {
				conn = ksocknal_find_conn_locked(peer_ni, tx, tx->tx_nonblk);
				if (conn != NULL) {
					/* I've got nothing that need to be
					 * connecting and I do have an actual
					 * connection...
					 */
					ksocknal_queue_tx_locked(tx, conn);
					read_unlock(g_lock);
					return 0;
				}
			}
		}

		/* I'll need a write lock... */
		read_unlock(g_lock);

		write_lock_bh(g_lock);

		peer_ni = ksocknal_find_peer_locked(ni, id);
		if (peer_ni != NULL)
			break;

		write_unlock_bh(g_lock);

		if ((id->pid & LNET_PID_USERFLAG) != 0) {
			CERROR("Refusing to create a connection to userspace process %s\n",
			       libcfs_idstr(id));
			return -EHOSTUNREACH;
		}

		if (retry) {
			CERROR("Can't find peer_ni %s\n", libcfs_idstr(id));
			return -EHOSTUNREACH;
		}

		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_addr.s_addr = id->nid.nid_addr[0];
		sa.sin_port = htons(lnet_acceptor_port());
		{
			struct lnet_process_id id4 = {
				.pid = id->pid,
				.nid = lnet_nid_to_nid4(&id->nid),
			};
			rc = ksocknal_add_peer(ni, id4, (struct sockaddr *)&sa);
		}
		if (rc != 0) {
			CERROR("Can't add peer_ni %s: %d\n",
			       libcfs_idstr(id), rc);
			return rc;
		}
	}

        ksocknal_launch_all_connections_locked(peer_ni);

        conn = ksocknal_find_conn_locked(peer_ni, tx, tx->tx_nonblk);
        if (conn != NULL) {
                /* Connection exists; queue message on it */
                ksocknal_queue_tx_locked (tx, conn);
		write_unlock_bh(g_lock);
                return (0);
        }

	if (peer_ni->ksnp_accepting > 0 ||
	    ksocknal_find_connecting_conn_cb_locked(peer_ni) != NULL) {
                /* the message is going to be pinned to the peer_ni */
		tx->tx_deadline = ktime_get_seconds() +
				  ksocknal_timeout();

                /* Queue the message until a connection is established */
		list_add_tail(&tx->tx_list, &peer_ni->ksnp_tx_queue);
		write_unlock_bh(g_lock);
                return 0;
	}

	write_unlock_bh(g_lock);

        /* NB Routes may be ignored if connections to them failed recently */
	CNETERR("No usable routes to %s\n", libcfs_idstr(id));
	tx->tx_hstatus = LNET_MSG_STATUS_REMOTE_ERROR;
        return (-EHOSTUNREACH);
}

int
ksocknal_send(struct lnet_ni *ni, void *private, struct lnet_msg *lntmsg)
{
	/* '1' for consistency with code that checks !mpflag to restore */
	unsigned int mpflag = 1;
	int type = lntmsg->msg_type;
	struct lnet_processid *target = &lntmsg->msg_target;
	unsigned int payload_niov = lntmsg->msg_niov;
	struct bio_vec *payload_kiov = lntmsg->msg_kiov;
	unsigned int payload_offset = lntmsg->msg_offset;
	unsigned int payload_nob = lntmsg->msg_len;
	struct ksock_tx *tx;
	int desc_size;
	int rc;

	/* NB 'private' is different depending on what we're sending.
	 * Just ignore it...
	 */

	CDEBUG(D_NET, "sending %u bytes in %d frags to %s\n",
	       payload_nob, payload_niov, libcfs_idstr(target));

	LASSERT (payload_nob == 0 || payload_niov > 0);
	LASSERT (!in_interrupt ());

	desc_size = offsetof(struct ksock_tx,
			     tx_payload[payload_niov]);

        if (lntmsg->msg_vmflush)
		mpflag = memalloc_noreclaim_save();

	tx = ksocknal_alloc_tx(KSOCK_MSG_LNET, desc_size);
	if (tx == NULL) {
		CERROR("Can't allocate tx desc type %d size %d\n",
		       type, desc_size);
		if (lntmsg->msg_vmflush)
			memalloc_noreclaim_restore(mpflag);
		return -ENOMEM;
	}

	tx->tx_conn = NULL;                     /* set when assigned a conn */
	tx->tx_lnetmsg = lntmsg;

	tx->tx_niov = 1;
	tx->tx_kiov = tx->tx_payload;
	tx->tx_nkiov = lnet_extract_kiov(payload_niov, tx->tx_kiov,
					 payload_niov, payload_kiov,
					 payload_offset, payload_nob);

	LASSERT(tx->tx_nkiov <= LNET_MAX_IOV);

	if (payload_nob >= *ksocknal_tunables.ksnd_zc_min_payload)
		tx->tx_zc_capable = 1;

	tx->tx_msg.ksm_csum = 0;
	tx->tx_msg.ksm_type = KSOCK_MSG_LNET;
	tx->tx_msg.ksm_zc_cookies[0] = 0;
	tx->tx_msg.ksm_zc_cookies[1] = 0;

	/* The first fragment will be set later in pro_pack */
	rc = ksocknal_launch_packet(ni, tx, target);
	/*
	 * We can't test lntsmg->msg_vmflush again as lntmsg may
	 * have been freed.
	 */
	if (!mpflag)
		memalloc_noreclaim_restore(mpflag);

	if (rc == 0)
		return (0);

	lntmsg->msg_health_status = tx->tx_hstatus;
	ksocknal_free_tx(tx);
	return -EIO;
}

void
ksocknal_thread_fini (void)
{
	if (atomic_dec_and_test(&ksocknal_data.ksnd_nthreads))
		wake_up_var(&ksocknal_data.ksnd_nthreads);
}

int
ksocknal_new_packet(struct ksock_conn *conn, int nob_to_skip)
{
        static char ksocknal_slop_buffer[4096];
	int nob;
	unsigned int niov;
	int skipped;

        LASSERT(conn->ksnc_proto != NULL);

        if ((*ksocknal_tunables.ksnd_eager_ack & conn->ksnc_type) != 0) {
                /* Remind the socket to ack eagerly... */
                ksocknal_lib_eager_ack(conn);
        }

	if (nob_to_skip == 0) {         /* right at next packet boundary now */
		conn->ksnc_rx_started = 0;
		smp_mb();                       /* racing with timeout thread */

		switch (conn->ksnc_proto->pro_version) {
		case  KSOCK_PROTO_V2:
		case  KSOCK_PROTO_V3:
			conn->ksnc_rx_state = SOCKNAL_RX_KSM_HEADER;
			conn->ksnc_rx_iov = (struct kvec *)&conn->ksnc_rx_iov_space;
			conn->ksnc_rx_iov[0].iov_base = (char *)&conn->ksnc_msg;

			conn->ksnc_rx_nob_wanted = sizeof(struct ksock_msg_hdr);
			conn->ksnc_rx_nob_left = sizeof(struct ksock_msg_hdr);
			conn->ksnc_rx_iov[0].iov_len =
				sizeof(struct ksock_msg_hdr);
			break;

		case KSOCK_PROTO_V1:
			/* Receiving bare struct lnet_hdr_nid4 */
			conn->ksnc_rx_state = SOCKNAL_RX_LNET_HEADER;
			conn->ksnc_rx_nob_wanted = sizeof(struct lnet_hdr_nid4);
			conn->ksnc_rx_nob_left = sizeof(struct lnet_hdr_nid4);

			conn->ksnc_rx_iov = (struct kvec *)&conn->ksnc_rx_iov_space;
			conn->ksnc_rx_iov[0].iov_base =
				(void *)&conn->ksnc_msg.ksm_u.lnetmsg_nid4;
			conn->ksnc_rx_iov[0].iov_len =
				sizeof(struct lnet_hdr_nid4);
			break;

		default:
			LBUG();
		}
                conn->ksnc_rx_niov = 1;

                conn->ksnc_rx_kiov = NULL;
                conn->ksnc_rx_nkiov = 0;
                conn->ksnc_rx_csum = ~0;
                return (1);
        }

        /* Set up to skip as much as possible now.  If there's more left
         * (ran out of iov entries) we'll get called again */

	conn->ksnc_rx_state = SOCKNAL_RX_SLOP;
	conn->ksnc_rx_nob_left = nob_to_skip;
	conn->ksnc_rx_iov = (struct kvec *)&conn->ksnc_rx_iov_space;
	skipped = 0;
	niov = 0;

	do {
		nob = min_t(int, nob_to_skip, sizeof(ksocknal_slop_buffer));

		conn->ksnc_rx_iov[niov].iov_base = ksocknal_slop_buffer;
		conn->ksnc_rx_iov[niov].iov_len  = nob;
		niov++;
		skipped += nob;
		nob_to_skip -= nob;

	} while (nob_to_skip != 0 &&    /* mustn't overflow conn's rx iov */
		 niov < sizeof(conn->ksnc_rx_iov_space) / sizeof(struct kvec));

        conn->ksnc_rx_niov = niov;
        conn->ksnc_rx_kiov = NULL;
        conn->ksnc_rx_nkiov = 0;
        conn->ksnc_rx_nob_wanted = skipped;
        return (0);
}

static int
ksocknal_process_receive(struct ksock_conn *conn,
			 struct page **rx_scratch_pgs,
			 struct kvec *scratch_iov)
{
	struct _lnet_hdr_nid4 *lhdr;
	struct lnet_processid *id;
	struct lnet_hdr hdr;
	int rc;

	LASSERT(refcount_read(&conn->ksnc_conn_refcount) > 0);

	/* NB: sched lock NOT held */
	/* SOCKNAL_RX_LNET_HEADER is here for backward compatibility */
	LASSERT(conn->ksnc_rx_state == SOCKNAL_RX_KSM_HEADER ||
		conn->ksnc_rx_state == SOCKNAL_RX_LNET_PAYLOAD ||
		conn->ksnc_rx_state == SOCKNAL_RX_LNET_HEADER ||
		conn->ksnc_rx_state == SOCKNAL_RX_SLOP);
 again:
	if (conn->ksnc_rx_nob_wanted != 0) {
		rc = ksocknal_receive(conn, rx_scratch_pgs,
				      scratch_iov);

		if (rc <= 0) {
			struct lnet_processid *ksnp_id;

			ksnp_id = &conn->ksnc_peer->ksnp_id;

			LASSERT(rc != -EAGAIN);
			if (rc == 0)
				CDEBUG(D_NET, "[%p] EOF from %s ip %pISp\n",
				       conn, libcfs_idstr(ksnp_id),
				       &conn->ksnc_peeraddr);
			else if (!conn->ksnc_closing)
				CERROR("[%p] Error %d on read from %s ip %pISp\n",
				       conn, rc, libcfs_idstr(ksnp_id),
				       &conn->ksnc_peeraddr);

                        /* it's not an error if conn is being closed */
                        ksocknal_close_conn_and_siblings (conn,
                                                          (conn->ksnc_closing) ? 0 : rc);
                        return (rc == 0 ? -ESHUTDOWN : rc);
                }

                if (conn->ksnc_rx_nob_wanted != 0) {
                        /* short read */
                        return (-EAGAIN);
                }
        }
	switch (conn->ksnc_rx_state) {
	case SOCKNAL_RX_KSM_HEADER:
		if (conn->ksnc_flip) {
			__swab32s(&conn->ksnc_msg.ksm_type);
			__swab32s(&conn->ksnc_msg.ksm_csum);
			__swab64s(&conn->ksnc_msg.ksm_zc_cookies[0]);
			__swab64s(&conn->ksnc_msg.ksm_zc_cookies[1]);
		}

		if (conn->ksnc_msg.ksm_type == KSOCK_MSG_NOOP &&
		    conn->ksnc_msg.ksm_csum != 0 &&     /* has checksum */
		    conn->ksnc_msg.ksm_csum != conn->ksnc_rx_csum) {
			/* NOOP Checksum error */
			CERROR("%s: Checksum error, wire:0x%08X data:0x%08X\n",
			       libcfs_idstr(&conn->ksnc_peer->ksnp_id),
			       conn->ksnc_msg.ksm_csum, conn->ksnc_rx_csum);
			ksocknal_new_packet(conn, 0);
			ksocknal_close_conn_and_siblings(conn, -EPROTO);
			return (-EIO);
		}

		if (conn->ksnc_msg.ksm_zc_cookies[1] != 0) {
			__u64 cookie = 0;

			LASSERT(conn->ksnc_proto != &ksocknal_protocol_v1x);

			if (conn->ksnc_msg.ksm_type == KSOCK_MSG_NOOP)
				cookie = conn->ksnc_msg.ksm_zc_cookies[0];

			rc = conn->ksnc_proto->pro_handle_zcack(
				conn, cookie, conn->ksnc_msg.ksm_zc_cookies[1]);

			if (rc != 0) {
				CERROR("%s: Unknown ZC-ACK cookie: %llu, %llu\n",
				       libcfs_idstr(&conn->ksnc_peer->ksnp_id),
				       cookie,
				       conn->ksnc_msg.ksm_zc_cookies[1]);
				ksocknal_new_packet(conn, 0);
				ksocknal_close_conn_and_siblings(conn, -EPROTO);
				return rc;
			}
		}

		switch (conn->ksnc_msg.ksm_type) {
		case KSOCK_MSG_NOOP:
			ksocknal_new_packet(conn, 0);
			return 0;	/* NOOP is done and just return */

		case KSOCK_MSG_LNET:

			conn->ksnc_rx_state = SOCKNAL_RX_LNET_HEADER;
			conn->ksnc_rx_nob_wanted = sizeof(struct lnet_hdr_nid4);
			conn->ksnc_rx_nob_left = sizeof(struct lnet_hdr_nid4);

			conn->ksnc_rx_iov = conn->ksnc_rx_iov_space.iov;
			conn->ksnc_rx_iov[0].iov_base =
				(void *)&conn->ksnc_msg.ksm_u.lnetmsg_nid4;
			conn->ksnc_rx_iov[0].iov_len =
				sizeof(struct lnet_hdr_nid4);

			conn->ksnc_rx_niov = 1;
			conn->ksnc_rx_kiov = NULL;
			conn->ksnc_rx_nkiov = 0;

			goto again;     /* read lnet header now */

		default:
			CERROR("%s: Unknown message type: %x\n",
			       libcfs_idstr(&conn->ksnc_peer->ksnp_id),
			       conn->ksnc_msg.ksm_type);
			ksocknal_new_packet(conn, 0);
			ksocknal_close_conn_and_siblings(conn, -EPROTO);
			return -EPROTO;
		}

	case SOCKNAL_RX_LNET_HEADER:
		/* unpack message header */
		conn->ksnc_proto->pro_unpack(&conn->ksnc_msg, &hdr);

		if ((conn->ksnc_peer->ksnp_id.pid & LNET_PID_USERFLAG) != 0) {
			/* Userspace peer_ni */
			id = &conn->ksnc_peer->ksnp_id;

			/* Substitute process ID assigned at connection time */
			hdr.src_pid = id->pid;
			hdr.src_nid = id->nid;
		}

		conn->ksnc_rx_state = SOCKNAL_RX_PARSE;
		ksocknal_conn_addref(conn);     /* ++ref while parsing */


		rc = lnet_parse(conn->ksnc_peer->ksnp_ni,
				&hdr,
				&conn->ksnc_peer->ksnp_id.nid,
				conn, 0);
		if (rc < 0) {
			/* I just received garbage: give up on this conn */
			ksocknal_new_packet(conn, 0);
			ksocknal_close_conn_and_siblings(conn, rc);
			ksocknal_conn_decref(conn);
			return (-EPROTO);
		}

		/* I'm racing with ksocknal_recv() */
		LASSERT(conn->ksnc_rx_state == SOCKNAL_RX_PARSE ||
			conn->ksnc_rx_state == SOCKNAL_RX_LNET_PAYLOAD);

		if (conn->ksnc_rx_state != SOCKNAL_RX_LNET_PAYLOAD)
			return 0;

		/* ksocknal_recv() got called */
		goto again;

	case SOCKNAL_RX_LNET_PAYLOAD:
		/* payload all received */
		rc = 0;

		if (conn->ksnc_rx_nob_left == 0 &&   /* not truncating */
		    conn->ksnc_msg.ksm_csum != 0 &&  /* has checksum */
		    conn->ksnc_msg.ksm_csum != conn->ksnc_rx_csum) {
			CERROR("%s: Checksum error, wire:0x%08X data:0x%08X\n",
			       libcfs_idstr(&conn->ksnc_peer->ksnp_id),
			       conn->ksnc_msg.ksm_csum, conn->ksnc_rx_csum);
			rc = -EIO;
		}

		if (rc == 0 && conn->ksnc_msg.ksm_zc_cookies[0] != 0) {
			LASSERT(conn->ksnc_proto != &ksocknal_protocol_v1x);

			lhdr = (void *)&conn->ksnc_msg.ksm_u.lnetmsg_nid4;
			id = &conn->ksnc_peer->ksnp_id;

			rc = conn->ksnc_proto->pro_handle_zcreq(
				conn,
				conn->ksnc_msg.ksm_zc_cookies[0],
				*ksocknal_tunables.ksnd_nonblk_zcack ||
				le64_to_cpu(lhdr->src_nid) !=
				lnet_nid_to_nid4(&id->nid));
		}

		if (rc && conn->ksnc_lnet_msg)
			conn->ksnc_lnet_msg->msg_health_status =
				LNET_MSG_STATUS_REMOTE_ERROR;
		lnet_finalize(conn->ksnc_lnet_msg, rc);

		if (rc != 0) {
			ksocknal_new_packet(conn, 0);
			ksocknal_close_conn_and_siblings(conn, rc);
			return (-EPROTO);
		}
		fallthrough;

	case SOCKNAL_RX_SLOP:
		/* starting new packet? */
		if (ksocknal_new_packet(conn, conn->ksnc_rx_nob_left))
			return 0;	/* come back later */
		goto again;		/* try to finish reading slop now */

	default:
		break;
	}

        /* Not Reached */
        LBUG ();
        return (-EINVAL);                       /* keep gcc happy */
}

int
ksocknal_recv(struct lnet_ni *ni, void *private, struct lnet_msg *msg,
	      int delayed, unsigned int niov,
	      struct bio_vec *kiov, unsigned int offset, unsigned int mlen,
	      unsigned int rlen)
{
	struct ksock_conn *conn = private;
	struct ksock_sched *sched = conn->ksnc_scheduler;

        LASSERT (mlen <= rlen);

	conn->ksnc_lnet_msg = msg;
	conn->ksnc_rx_nob_wanted = mlen;
	conn->ksnc_rx_nob_left   = rlen;

	if (mlen == 0) {
		conn->ksnc_rx_nkiov = 0;
		conn->ksnc_rx_kiov = NULL;
		conn->ksnc_rx_iov = conn->ksnc_rx_iov_space.iov;
		conn->ksnc_rx_niov = 0;
	} else {
		conn->ksnc_rx_niov = 0;
		conn->ksnc_rx_iov  = NULL;
		conn->ksnc_rx_kiov = conn->ksnc_rx_iov_space.kiov;
		conn->ksnc_rx_nkiov =
			lnet_extract_kiov(LNET_MAX_IOV, conn->ksnc_rx_kiov,
					  niov, kiov, offset, mlen);
	}

	LASSERT(conn->ksnc_rx_nkiov <= LNET_MAX_IOV);
        LASSERT (mlen ==
                 lnet_iov_nob (conn->ksnc_rx_niov, conn->ksnc_rx_iov) +
                 lnet_kiov_nob (conn->ksnc_rx_nkiov, conn->ksnc_rx_kiov));

        LASSERT (conn->ksnc_rx_scheduled);

	spin_lock_bh(&sched->kss_lock);

	switch (conn->ksnc_rx_state) {
	case SOCKNAL_RX_PARSE_WAIT:
		list_add_tail(&conn->ksnc_rx_list, &sched->kss_rx_conns);
		wake_up(&sched->kss_waitq);
		LASSERT(conn->ksnc_rx_ready);
		break;

        case SOCKNAL_RX_PARSE:
                /* scheduler hasn't noticed I'm parsing yet */
                break;
        }

        conn->ksnc_rx_state = SOCKNAL_RX_LNET_PAYLOAD;

	spin_unlock_bh(&sched->kss_lock);
	ksocknal_conn_decref(conn);
	return 0;
}

static inline int
ksocknal_sched_cansleep(struct ksock_sched *sched)
{
	int           rc;

	spin_lock_bh(&sched->kss_lock);

	rc = (!ksocknal_data.ksnd_shuttingdown &&
	      list_empty(&sched->kss_rx_conns) &&
	      list_empty(&sched->kss_tx_conns));

	spin_unlock_bh(&sched->kss_lock);
	return rc;
}

int ksocknal_scheduler(void *arg)
{
	struct ksock_sched *sched;
	struct ksock_conn *conn;
	struct ksock_tx	*tx;
	int rc;
	long id = (long)arg;
	struct page **rx_scratch_pgs;
	struct kvec *scratch_iov;

	sched = ksocknal_data.ksnd_schedulers[KSOCK_THREAD_CPT(id)];

	LIBCFS_CPT_ALLOC(rx_scratch_pgs, lnet_cpt_table(), sched->kss_cpt,
			 sizeof(*rx_scratch_pgs) * LNET_MAX_IOV);
	if (!rx_scratch_pgs) {
		CERROR("Unable to allocate scratch pages\n");
		return -ENOMEM;
	}

	LIBCFS_CPT_ALLOC(scratch_iov, lnet_cpt_table(), sched->kss_cpt,
			 sizeof(*scratch_iov) * LNET_MAX_IOV);
	if (!scratch_iov) {
		CERROR("Unable to allocate scratch iov\n");
		return -ENOMEM;
	}

	rc = cfs_cpt_bind(lnet_cpt_table(), sched->kss_cpt);
	if (rc != 0) {
		CWARN("Can't set CPU partition affinity to %d: %d\n",
			sched->kss_cpt, rc);
	}

	spin_lock_bh(&sched->kss_lock);

	while (!ksocknal_data.ksnd_shuttingdown) {
		bool did_something = false;

		/* Ensure I progress everything semi-fairly */
		conn = list_first_entry_or_null(&sched->kss_rx_conns,
						struct ksock_conn,
						ksnc_rx_list);
		if (conn) {
			list_del(&conn->ksnc_rx_list);

			LASSERT(conn->ksnc_rx_scheduled);
			LASSERT(conn->ksnc_rx_ready);

			/* clear rx_ready in case receive isn't complete.
			 * Do it BEFORE we call process_recv, since
			 * data_ready can set it any time after we release
			 * kss_lock. */
			conn->ksnc_rx_ready = 0;
			spin_unlock_bh(&sched->kss_lock);

			rc = ksocknal_process_receive(conn, rx_scratch_pgs,
						      scratch_iov);

			spin_lock_bh(&sched->kss_lock);

			/* I'm the only one that can clear this flag */
			LASSERT(conn->ksnc_rx_scheduled);

			/* Did process_receive get everything it wanted? */
			if (rc == 0)
				conn->ksnc_rx_ready = 1;

			if (conn->ksnc_rx_state == SOCKNAL_RX_PARSE) {
				/* Conn blocked waiting for ksocknal_recv()
				 * I change its state (under lock) to signal
				 * it can be rescheduled */
				conn->ksnc_rx_state = SOCKNAL_RX_PARSE_WAIT;
			} else if (conn->ksnc_rx_ready) {
				/* reschedule for rx */
				list_add_tail(&conn->ksnc_rx_list,
						   &sched->kss_rx_conns);
			} else {
				conn->ksnc_rx_scheduled = 0;
				/* drop my ref */
				ksocknal_conn_decref(conn);
			}

			did_something = true;
		}

		if (!list_empty(&sched->kss_tx_conns)) {
			LIST_HEAD(zlist);

			list_splice_init(&sched->kss_zombie_noop_txs, &zlist);

			conn = list_first_entry(&sched->kss_tx_conns,
						struct ksock_conn,
						ksnc_tx_list);
			list_del(&conn->ksnc_tx_list);

			LASSERT(conn->ksnc_tx_scheduled);
			LASSERT(conn->ksnc_tx_ready);
			LASSERT(!list_empty(&conn->ksnc_tx_queue));

			tx = list_first_entry(&conn->ksnc_tx_queue,
					      struct ksock_tx, tx_list);

			if (conn->ksnc_tx_carrier == tx)
				ksocknal_next_tx_carrier(conn);

			/* dequeue now so empty list => more to send */
			list_del(&tx->tx_list);

			/* Clear tx_ready in case send isn't complete.  Do
			 * it BEFORE we call process_transmit, since
			 * write_space can set it any time after we release
			 * kss_lock. */
			conn->ksnc_tx_ready = 0;
			spin_unlock_bh(&sched->kss_lock);

			if (!list_empty(&zlist)) {
				/* free zombie noop txs, it's fast because
				 * noop txs are just put in freelist */
				ksocknal_txlist_done(NULL, &zlist, 0);
			}

			rc = ksocknal_process_transmit(conn, tx, scratch_iov);

			if (rc == -ENOMEM || rc == -EAGAIN) {
				/* Incomplete send: replace tx on HEAD of tx_queue */
				spin_lock_bh(&sched->kss_lock);
				list_add(&tx->tx_list,
					 &conn->ksnc_tx_queue);
			} else {
				/* Complete send; tx -ref */
				ksocknal_tx_decref(tx);

				spin_lock_bh(&sched->kss_lock);
				/* assume space for more */
				conn->ksnc_tx_ready = 1;
			}

			if (rc == -ENOMEM) {
				/* Do nothing; after a short timeout, this
				 * conn will be reposted on kss_tx_conns. */
			} else if (conn->ksnc_tx_ready &&
				   !list_empty(&conn->ksnc_tx_queue)) {
				/* reschedule for tx */
				list_add_tail(&conn->ksnc_tx_list,
					      &sched->kss_tx_conns);
			} else {
				conn->ksnc_tx_scheduled = 0;
				/* drop my ref */
				ksocknal_conn_decref(conn);
			}

			did_something = true;
		}
		if (!did_something ||	/* nothing to do */
		    need_resched()) {	/* hogging CPU? */
			spin_unlock_bh(&sched->kss_lock);

			if (!did_something) {   /* wait for something to do */
				rc = wait_event_interruptible_exclusive(
					sched->kss_waitq,
					!ksocknal_sched_cansleep(sched));
				LASSERT (rc == 0);
			} else {
				cond_resched();
			}

			spin_lock_bh(&sched->kss_lock);
		}
	}

	spin_unlock_bh(&sched->kss_lock);
	CFS_FREE_PTR_ARRAY(rx_scratch_pgs, LNET_MAX_IOV);
	CFS_FREE_PTR_ARRAY(scratch_iov, LNET_MAX_IOV);
	ksocknal_thread_fini();
	return 0;
}

/*
 * Add connection to kss_rx_conns of scheduler
 * and wakeup the scheduler.
 */
void ksocknal_read_callback(struct ksock_conn *conn)
{
	struct ksock_sched *sched;

	sched = conn->ksnc_scheduler;

	spin_lock_bh(&sched->kss_lock);

	conn->ksnc_rx_ready = 1;

	if (!conn->ksnc_rx_scheduled) {  /* not being progressed */
		list_add_tail(&conn->ksnc_rx_list,
				  &sched->kss_rx_conns);
		conn->ksnc_rx_scheduled = 1;
		/* extra ref for scheduler */
		ksocknal_conn_addref(conn);

		wake_up (&sched->kss_waitq);
	}
	spin_unlock_bh(&sched->kss_lock);
}

/*
 * Add connection to kss_tx_conns of scheduler
 * and wakeup the scheduler.
 */
void ksocknal_write_callback(struct ksock_conn *conn)
{
	struct ksock_sched *sched;

	sched = conn->ksnc_scheduler;

	spin_lock_bh(&sched->kss_lock);

	conn->ksnc_tx_ready = 1;

	if (!conn->ksnc_tx_scheduled && /* not being progressed */
	    !list_empty(&conn->ksnc_tx_queue)) { /* packets to send */
		list_add_tail(&conn->ksnc_tx_list, &sched->kss_tx_conns);
		conn->ksnc_tx_scheduled = 1;
		/* extra ref for scheduler */
		ksocknal_conn_addref(conn);

		wake_up(&sched->kss_waitq);
	}

	spin_unlock_bh(&sched->kss_lock);
}

static const struct ksock_proto *
ksocknal_parse_proto_version(struct ksock_hello_msg *hello)
{
	__u32   version = 0;

	if (hello->kshm_magic == LNET_PROTO_MAGIC)
		version = hello->kshm_version;
	else if (hello->kshm_magic == __swab32(LNET_PROTO_MAGIC))
		version = __swab32(hello->kshm_version);

	if (version) {
#if SOCKNAL_VERSION_DEBUG
		if (*ksocknal_tunables.ksnd_protocol == 1)
			return NULL;

		if (*ksocknal_tunables.ksnd_protocol == 2 &&
		    version == KSOCK_PROTO_V3)
			return NULL;
#endif
		if (version == KSOCK_PROTO_V2)
			return &ksocknal_protocol_v2x;

		if (version == KSOCK_PROTO_V3)
			return &ksocknal_protocol_v3x;

		return NULL;
	}

	if (hello->kshm_magic == le32_to_cpu(LNET_PROTO_TCP_MAGIC)) {
		struct lnet_magicversion *hmv;

		BUILD_BUG_ON(sizeof(struct lnet_magicversion) !=
			     offsetof(struct ksock_hello_msg, kshm_src_nid));

		hmv = (struct lnet_magicversion *)hello;

		if (hmv->version_major == cpu_to_le16 (KSOCK_PROTO_V1_MAJOR) &&
		    hmv->version_minor == cpu_to_le16 (KSOCK_PROTO_V1_MINOR))
			return &ksocknal_protocol_v1x;
	}

	return NULL;
}

int
ksocknal_send_hello(struct lnet_ni *ni, struct ksock_conn *conn,
		    struct lnet_nid *peer_nid, struct ksock_hello_msg *hello)
{
	/* CAVEAT EMPTOR: this byte flips 'ipaddrs' */
	struct ksock_net *net = (struct ksock_net *)ni->ni_data;

	LASSERT(hello->kshm_nips <= LNET_INTERFACES_NUM);

	/* rely on caller to hold a ref on socket so it wouldn't disappear */
	LASSERT(conn->ksnc_proto != NULL);

	hello->kshm_src_nid = ni->ni_nid;
	hello->kshm_dst_nid = *peer_nid;
	hello->kshm_src_pid = the_lnet.ln_pid;

	hello->kshm_src_incarnation = net->ksnn_incarnation;
	hello->kshm_ctype = conn->ksnc_type;

	return conn->ksnc_proto->pro_send_hello(conn, hello);
}

static int
ksocknal_invert_type(int type)
{
	switch (type) {
	case SOCKLND_CONN_ANY:
	case SOCKLND_CONN_CONTROL:
		return (type);
	case SOCKLND_CONN_BULK_IN:
		return SOCKLND_CONN_BULK_OUT;
	case SOCKLND_CONN_BULK_OUT:
		return SOCKLND_CONN_BULK_IN;
	default:
		return (SOCKLND_CONN_NONE);
	}
}

int
ksocknal_recv_hello(struct lnet_ni *ni, struct ksock_conn *conn,
		    struct ksock_hello_msg *hello,
		    struct lnet_processid *peerid,
		    __u64 *incarnation)
{
	/* Return < 0        fatal error
	 *        0          success
	 *        EALREADY   lost connection race
	 *        EPROTO     protocol version mismatch
	 */
	struct socket *sock = conn->ksnc_sock;
	int active = (conn->ksnc_proto != NULL);
	int timeout;
	int proto_match;
	int rc;
	const struct ksock_proto *proto;
	struct lnet_processid recv_id;

	/* socket type set on active connections - not set on passive */
	LASSERT(!active == !(conn->ksnc_type != SOCKLND_CONN_NONE));

	timeout = active ? ksocknal_timeout() :
		lnet_acceptor_timeout();

	rc = lnet_sock_read(sock, &hello->kshm_magic,
			    sizeof(hello->kshm_magic), timeout);
	if (rc != 0) {
		CERROR("Error %d reading HELLO from %pIS\n",
		       rc, &conn->ksnc_peeraddr);
		LASSERT(rc < 0);
		return rc;
	}

	if (hello->kshm_magic != LNET_PROTO_MAGIC &&
	    hello->kshm_magic != __swab32(LNET_PROTO_MAGIC) &&
	    hello->kshm_magic != le32_to_cpu(LNET_PROTO_TCP_MAGIC)) {
		/* Unexpected magic! */
		CERROR("Bad magic(1) %#08x (%#08x expected) from %pIS\n",
		       __cpu_to_le32 (hello->kshm_magic),
		       LNET_PROTO_TCP_MAGIC, &conn->ksnc_peeraddr);
		return -EPROTO;
	}

	rc = lnet_sock_read(sock, &hello->kshm_version,
			    sizeof(hello->kshm_version), timeout);
	if (rc != 0) {
		CERROR("Error %d reading HELLO from %pIS\n",
		       rc, &conn->ksnc_peeraddr);
		LASSERT(rc < 0);
		return rc;
	}

	proto = ksocknal_parse_proto_version(hello);
	if (proto == NULL) {
		if (!active) {
			/* unknown protocol from peer_ni,
			 * tell peer_ni my protocol.
			 */
			conn->ksnc_proto = &ksocknal_protocol_v3x;
#if SOCKNAL_VERSION_DEBUG
			if (*ksocknal_tunables.ksnd_protocol == 2)
				conn->ksnc_proto = &ksocknal_protocol_v2x;
			else if (*ksocknal_tunables.ksnd_protocol == 1)
				conn->ksnc_proto = &ksocknal_protocol_v1x;
#endif
			hello->kshm_nips = 0;
			ksocknal_send_hello(ni, conn, &ni->ni_nid,
					    hello);
		}

		CERROR("Unknown protocol version (%d.x expected) from %pIS\n",
		       conn->ksnc_proto->pro_version, &conn->ksnc_peeraddr);

		return -EPROTO;
	}

	proto_match = (conn->ksnc_proto == proto);
	conn->ksnc_proto = proto;

	/* receive the rest of hello message anyway */
	rc = conn->ksnc_proto->pro_recv_hello(conn, hello, timeout);
	if (rc != 0) {
		CERROR("Error %d reading or checking hello from from %pIS\n",
		       rc, &conn->ksnc_peeraddr);
		LASSERT(rc < 0);
		return rc;
	}

	*incarnation = hello->kshm_src_incarnation;

	if (LNET_NID_IS_ANY(&hello->kshm_src_nid)) {
		CERROR("Expecting a HELLO hdr with a NID, but got LNET_NID_ANY from %pIS\n",
		       &conn->ksnc_peeraddr);
		return -EPROTO;
	}

	if (!active &&
	    rpc_get_port((struct sockaddr *)&conn->ksnc_peeraddr) >
	    LNET_ACCEPTOR_MAX_RESERVED_PORT) {
		/* Userspace NAL assigns peer_ni process ID from socket */
		recv_id.pid = rpc_get_port((struct sockaddr *)
					   &conn->ksnc_peeraddr) |
			LNET_PID_USERFLAG;
		LASSERT(conn->ksnc_peeraddr.ss_family == AF_INET);
		memset(&recv_id.nid, 0, sizeof(recv_id.nid));
		recv_id.nid.nid_type = ni->ni_nid.nid_type;
		recv_id.nid.nid_num = ni->ni_nid.nid_num;
		recv_id.nid.nid_addr[0] =
			((struct sockaddr_in *)
			 &conn->ksnc_peeraddr)->sin_addr.s_addr;
	} else {
		recv_id.nid = hello->kshm_src_nid;
		recv_id.pid = hello->kshm_src_pid;
	}

	if (!active) {
		*peerid = recv_id;

		/* peer_ni determines type */
		conn->ksnc_type = ksocknal_invert_type(hello->kshm_ctype);
		if (conn->ksnc_type == SOCKLND_CONN_NONE) {
			CERROR("Unexpected type %d from %s ip %pIS\n",
			       hello->kshm_ctype, libcfs_idstr(peerid),
			       &conn->ksnc_peeraddr);
			return -EPROTO;
		}
		return 0;
	}

	if (peerid->pid != recv_id.pid ||
	    !nid_same(&peerid->nid,  &recv_id.nid)) {
		LCONSOLE_ERROR_MSG(0x130,
				   "Connected successfully to %s on host %pIS, but they claimed they were %s; please check your Lustre configuration.\n",
				   libcfs_idstr(peerid),
				   &conn->ksnc_peeraddr,
				   libcfs_idstr(&recv_id));
		return -EPROTO;
	}

	if (hello->kshm_ctype == SOCKLND_CONN_NONE) {
		/* Possible protocol mismatch or I lost the connection race */
		return proto_match ? EALREADY : EPROTO;
	}

	if (ksocknal_invert_type(hello->kshm_ctype) != conn->ksnc_type) {
		CERROR("Mismatched types: me %d, %s ip %pIS %d\n",
		       conn->ksnc_type, libcfs_idstr(peerid),
		       &conn->ksnc_peeraddr,
		       hello->kshm_ctype);
		return -EPROTO;
	}
	return 0;
}

static bool
ksocknal_connect(struct ksock_conn_cb *conn_cb)
{
	LIST_HEAD(zombies);
	struct ksock_peer_ni *peer_ni = conn_cb->ksnr_peer;
	int type;
	int wanted;
	struct socket *sock;
	time64_t deadline;
	bool retry_later = false;
	int rc = 0;

	deadline = ktime_get_seconds() + ksocknal_timeout();

	write_lock_bh(&ksocknal_data.ksnd_global_lock);

	LASSERT(conn_cb->ksnr_scheduled);
	LASSERT(!conn_cb->ksnr_connecting);

	conn_cb->ksnr_connecting = 1;

	for (;;) {
		wanted = ksocknal_conn_cb_mask() & ~conn_cb->ksnr_connected;

		/* stop connecting if peer_ni/cb got closed under me, or
		 * conn cb got connected while queued
		 */
		if (peer_ni->ksnp_closing || conn_cb->ksnr_deleted ||
		    wanted == 0) {
			retry_later = false;
			break;
		}

		/* reschedule if peer_ni is connecting to me */
		if (peer_ni->ksnp_accepting > 0) {
			CDEBUG(D_NET,
			       "peer_ni %s(%d) already connecting to me, retry later.\n",
			       libcfs_nidstr(&peer_ni->ksnp_id.nid),
			       peer_ni->ksnp_accepting);
			retry_later = true;
		}

		if (retry_later) /* needs reschedule */
			break;

		if ((wanted & BIT(SOCKLND_CONN_ANY)) != 0) {
			type = SOCKLND_CONN_ANY;
		} else if ((wanted & BIT(SOCKLND_CONN_CONTROL)) != 0) {
			type = SOCKLND_CONN_CONTROL;
		} else if ((wanted & BIT(SOCKLND_CONN_BULK_IN)) != 0 &&
			   conn_cb->ksnr_blki_conn_count <= conn_cb->ksnr_blko_conn_count) {
			type = SOCKLND_CONN_BULK_IN;
		} else {
			LASSERT ((wanted & BIT(SOCKLND_CONN_BULK_OUT)) != 0);
			type = SOCKLND_CONN_BULK_OUT;
		}

		write_unlock_bh(&ksocknal_data.ksnd_global_lock);

		if (ktime_get_seconds() >= deadline) {
			rc = -ETIMEDOUT;
			lnet_connect_console_error(
				rc, &peer_ni->ksnp_id.nid,
				(struct sockaddr *)&conn_cb->ksnr_addr);
			goto failed;
		}

		sock = lnet_connect(&peer_ni->ksnp_id.nid,
				    conn_cb->ksnr_myiface,
				    (struct sockaddr *)&conn_cb->ksnr_addr,
				    peer_ni->ksnp_ni->ni_net_ns);
		if (IS_ERR(sock)) {
			rc = PTR_ERR(sock);
			goto failed;
		}

		rc = ksocknal_create_conn(peer_ni->ksnp_ni, conn_cb, sock,
					  type);
		if (rc < 0) {
			lnet_connect_console_error(
				rc, &peer_ni->ksnp_id.nid,
				(struct sockaddr *)&conn_cb->ksnr_addr);
			goto failed;
		}

		/* A +ve RC means I have to retry because I lost the connection
		 * race or I have to renegotiate protocol version
		 */
		retry_later = (rc != 0);
		if (retry_later)
			CDEBUG(D_NET, "peer_ni %s: conn race, retry later.\n",
			       libcfs_nidstr(&peer_ni->ksnp_id.nid));

		write_lock_bh(&ksocknal_data.ksnd_global_lock);
	}

	conn_cb->ksnr_scheduled = 0;
	conn_cb->ksnr_connecting = 0;

	if (retry_later) {
		/* re-queue for attention; this frees me up to handle
		 * the peer_ni's incoming connection request
		 */

		if (rc == EALREADY ||
		    (rc == 0 && peer_ni->ksnp_accepting > 0)) {
			/* We want to introduce a delay before next
			 * attempt to connect if we lost conn race, but
			 * the race is resolved quickly usually, so
			 * min_reconnectms should be good heuristic
			 */
			conn_cb->ksnr_retry_interval =
				*ksocknal_tunables.ksnd_min_reconnectms / 1000;
			conn_cb->ksnr_timeout = ktime_get_seconds() +
						conn_cb->ksnr_retry_interval;
		}

		ksocknal_launch_connection_locked(conn_cb);
	}

	write_unlock_bh(&ksocknal_data.ksnd_global_lock);
	return retry_later;

 failed:
	write_lock_bh(&ksocknal_data.ksnd_global_lock);

	conn_cb->ksnr_scheduled = 0;
	conn_cb->ksnr_connecting = 0;

	/* This is a retry rather than a new connection */
	conn_cb->ksnr_retry_interval *= 2;
	conn_cb->ksnr_retry_interval =
		max_t(time64_t, conn_cb->ksnr_retry_interval,
		      *ksocknal_tunables.ksnd_min_reconnectms / 1000);
	conn_cb->ksnr_retry_interval =
		min_t(time64_t, conn_cb->ksnr_retry_interval,
		      *ksocknal_tunables.ksnd_max_reconnectms / 1000);

	LASSERT(conn_cb->ksnr_retry_interval);
	conn_cb->ksnr_timeout = ktime_get_seconds() +
				conn_cb->ksnr_retry_interval;

	if (!list_empty(&peer_ni->ksnp_tx_queue) &&
	    peer_ni->ksnp_accepting == 0 &&
	    !ksocknal_find_connecting_conn_cb_locked(peer_ni)) {
		struct ksock_conn *conn;

		/* ksnp_tx_queue is queued on a conn on successful
		 * connection for V1.x and V2.x
		 */
		conn = list_first_entry_or_null(&peer_ni->ksnp_conns,
						struct ksock_conn, ksnc_list);
		if (conn)
			LASSERT(conn->ksnc_proto == &ksocknal_protocol_v3x ||
				conn->ksnc_proto == &ksocknal_protocol_v4x);

		/* take all the blocked packets while I've got the lock and
		 * complete below...
		 */
		list_splice_init(&peer_ni->ksnp_tx_queue, &zombies);
	}

	write_unlock_bh(&ksocknal_data.ksnd_global_lock);

	ksocknal_peer_failed(peer_ni);
	ksocknal_txlist_done(peer_ni->ksnp_ni, &zombies, rc);
	return 0;
}

/*
 * check whether we need to create more connds.
 * It will try to create new thread if it's necessary, @timeout can
 * be updated if failed to create, so caller wouldn't keep try while
 * running out of resource.
 */
static int
ksocknal_connd_check_start(time64_t sec, long *timeout)
{
        int rc;
        int total = ksocknal_data.ksnd_connd_starting +
                    ksocknal_data.ksnd_connd_running;

        if (unlikely(ksocknal_data.ksnd_init < SOCKNAL_INIT_ALL)) {
                /* still in initializing */
                return 0;
        }

        if (total >= *ksocknal_tunables.ksnd_nconnds_max ||
            total > ksocknal_data.ksnd_connd_connecting + SOCKNAL_CONND_RESV) {
                /* can't create more connd, or still have enough
                 * threads to handle more connecting */
                return 0;
        }

        if (list_empty(&ksocknal_data.ksnd_connd_routes)) {
                /* no pending connecting request */
                return 0;
        }

        if (sec - ksocknal_data.ksnd_connd_failed_stamp <= 1) {
                /* may run out of resource, retry later */
                *timeout = cfs_time_seconds(1);
                return 0;
        }

        if (ksocknal_data.ksnd_connd_starting > 0) {
                /* serialize starting to avoid flood */
                return 0;
        }

        ksocknal_data.ksnd_connd_starting_stamp = sec;
        ksocknal_data.ksnd_connd_starting++;
	spin_unlock_bh(&ksocknal_data.ksnd_connd_lock);

	/* NB: total is the next id */
	rc = ksocknal_thread_start(ksocknal_connd, NULL,
				   "socknal_cd%02d", total);

	spin_lock_bh(&ksocknal_data.ksnd_connd_lock);
        if (rc == 0)
                return 1;

        /* we tried ... */
        LASSERT(ksocknal_data.ksnd_connd_starting > 0);
        ksocknal_data.ksnd_connd_starting--;
	ksocknal_data.ksnd_connd_failed_stamp = ktime_get_real_seconds();

        return 1;
}

/*
 * check whether current thread can exit, it will return 1 if there are too
 * many threads and no creating in past 120 seconds.
 * Also, this function may update @timeout to make caller come back
 * again to recheck these conditions.
 */
static int
ksocknal_connd_check_stop(time64_t sec, long *timeout)
{
        int val;

        if (unlikely(ksocknal_data.ksnd_init < SOCKNAL_INIT_ALL)) {
                /* still in initializing */
                return 0;
        }

        if (ksocknal_data.ksnd_connd_starting > 0) {
                /* in progress of starting new thread */
                return 0;
        }

        if (ksocknal_data.ksnd_connd_running <=
            *ksocknal_tunables.ksnd_nconnds) { /* can't shrink */
                return 0;
        }

        /* created thread in past 120 seconds? */
        val = (int)(ksocknal_data.ksnd_connd_starting_stamp +
                    SOCKNAL_CONND_TIMEOUT - sec);

        *timeout = (val > 0) ? cfs_time_seconds(val) :
                               cfs_time_seconds(SOCKNAL_CONND_TIMEOUT);
        if (val > 0)
                return 0;

        /* no creating in past 120 seconds */

        return ksocknal_data.ksnd_connd_running >
               ksocknal_data.ksnd_connd_connecting + SOCKNAL_CONND_RESV;
}

/* Go through connd_cbs queue looking for a conn_cb that we can process
 * right now, @timeout_p can be updated if we need to come back later */
static struct ksock_conn_cb *
ksocknal_connd_get_conn_cb_locked(signed long *timeout_p)
{
	time64_t now = ktime_get_seconds();
	time64_t conn_timeout;
	struct ksock_conn_cb *conn_cb;

	/* connd_routes can contain both pending and ordinary routes */
	list_for_each_entry(conn_cb, &ksocknal_data.ksnd_connd_routes,
			    ksnr_connd_list) {

		conn_timeout = conn_cb->ksnr_timeout;

		if (conn_cb->ksnr_retry_interval == 0 ||
		    now >= conn_timeout)
			return conn_cb;

		if (*timeout_p == MAX_SCHEDULE_TIMEOUT ||
		    *timeout_p > cfs_time_seconds(conn_timeout - now))
			*timeout_p = cfs_time_seconds(conn_timeout - now);
	}

	return NULL;
}

int
ksocknal_connd(void *arg)
{
	spinlock_t *connd_lock = &ksocknal_data.ksnd_connd_lock;
	struct ksock_connreq *cr;
	wait_queue_entry_t wait;
	int cons_retry = 0;

	init_wait(&wait);

	spin_lock_bh(connd_lock);

	LASSERT(ksocknal_data.ksnd_connd_starting > 0);
	ksocknal_data.ksnd_connd_starting--;
	ksocknal_data.ksnd_connd_running++;

	while (!ksocknal_data.ksnd_shuttingdown) {
		struct ksock_conn_cb *conn_cb = NULL;
		time64_t sec = ktime_get_real_seconds();
		long timeout = MAX_SCHEDULE_TIMEOUT;
		bool dropped_lock = false;

		if (ksocknal_connd_check_stop(sec, &timeout)) {
			/* wakeup another one to check stop */
			wake_up(&ksocknal_data.ksnd_connd_waitq);
			break;
		}

		if (ksocknal_connd_check_start(sec, &timeout)) {
			/* created new thread */
			dropped_lock = true;
		}

		cr = list_first_entry_or_null(&ksocknal_data.ksnd_connd_connreqs,
					      struct ksock_connreq, ksncr_list);
		if (cr) {
			/* Connection accepted by the listener */
			list_del(&cr->ksncr_list);
			spin_unlock_bh(connd_lock);
			dropped_lock = true;

			ksocknal_create_conn(cr->ksncr_ni, NULL,
					     cr->ksncr_sock, SOCKLND_CONN_NONE);
			lnet_ni_decref(cr->ksncr_ni);
			LIBCFS_FREE(cr, sizeof(*cr));

			spin_lock_bh(connd_lock);
		}

		/* Only handle an outgoing connection request if there
		 * is a thread left to handle incoming connections and
		 * create new connd
		 */
		if (ksocknal_data.ksnd_connd_connecting + SOCKNAL_CONND_RESV <
		    ksocknal_data.ksnd_connd_running)
			conn_cb = ksocknal_connd_get_conn_cb_locked(&timeout);

		if (conn_cb) {
			list_del(&conn_cb->ksnr_connd_list);
			ksocknal_data.ksnd_connd_connecting++;
			spin_unlock_bh(connd_lock);
			dropped_lock = true;

			if (ksocknal_connect(conn_cb)) {
				/* consecutive retry */
				if (cons_retry++ > SOCKNAL_INSANITY_RECONN) {
					CWARN("massive consecutive re-connecting to %pIS\n",
					      &conn_cb->ksnr_addr);
					cons_retry = 0;
				}
			} else {
				cons_retry = 0;
			}

			ksocknal_conn_cb_decref(conn_cb);

			spin_lock_bh(connd_lock);
			ksocknal_data.ksnd_connd_connecting--;
		}

		if (dropped_lock) {
			if (!need_resched())
				continue;
			spin_unlock_bh(connd_lock);
			cond_resched();
			spin_lock_bh(connd_lock);
			continue;
		}

		/* Nothing to do for 'timeout'  */
		set_current_state(TASK_INTERRUPTIBLE);
		add_wait_queue_exclusive(&ksocknal_data.ksnd_connd_waitq,
					 &wait);
		spin_unlock_bh(connd_lock);

		schedule_timeout(timeout);

		remove_wait_queue(&ksocknal_data.ksnd_connd_waitq, &wait);
		spin_lock_bh(connd_lock);
	}
	ksocknal_data.ksnd_connd_running--;
	spin_unlock_bh(connd_lock);

	ksocknal_thread_fini();
	return 0;
}

static struct ksock_conn *
ksocknal_find_timed_out_conn(struct ksock_peer_ni *peer_ni)
{
        /* We're called with a shared lock on ksnd_global_lock */
	struct ksock_conn *conn;
	struct ksock_tx *tx;
	struct ksock_sched *sched;

	list_for_each_entry(conn, &peer_ni->ksnp_conns, ksnc_list) {
		int error;

                /* Don't need the {get,put}connsock dance to deref ksnc_sock */
                LASSERT (!conn->ksnc_closing);
		sched = conn->ksnc_scheduler;

		error = conn->ksnc_sock->sk->sk_err;
                if (error != 0) {
                        ksocknal_conn_addref(conn);

			switch (error) {
			case ECONNRESET:
				CNETERR("A connection with %s (%pISp) was reset; it may have rebooted.\n",
					libcfs_idstr(&peer_ni->ksnp_id),
					&conn->ksnc_peeraddr);
				break;
			case ETIMEDOUT:
				CNETERR("A connection with %s (%pISp) timed out; the network or node may be down.\n",
					libcfs_idstr(&peer_ni->ksnp_id),
					&conn->ksnc_peeraddr);
				break;
			default:
				CNETERR("An unexpected network error %d occurred with %s (%pISp\n",
					error,
					libcfs_idstr(&peer_ni->ksnp_id),
					&conn->ksnc_peeraddr);
				break;
			}

			return conn;
		}

		if (conn->ksnc_rx_started &&
		    ktime_get_seconds() >= conn->ksnc_rx_deadline) {
			/* Timed out incomplete incoming message */
			ksocknal_conn_addref(conn);
			CNETERR("Timeout receiving from %s (%pISp), state %d wanted %d left %d\n",
				libcfs_idstr(&peer_ni->ksnp_id),
				&conn->ksnc_peeraddr,
				conn->ksnc_rx_state,
				conn->ksnc_rx_nob_wanted,
				conn->ksnc_rx_nob_left);
			return conn;
		}

		spin_lock_bh(&sched->kss_lock);
		if ((!list_empty(&conn->ksnc_tx_queue) ||
		     conn->ksnc_sock->sk->sk_wmem_queued != 0) &&
		    ktime_get_seconds() >= conn->ksnc_tx_deadline) {
			/* Timed out messages queued for sending or
			 * buffered in the socket's send buffer
			 */
			ksocknal_conn_addref(conn);
			list_for_each_entry(tx, &conn->ksnc_tx_queue,
					    tx_list)
				tx->tx_hstatus =
					LNET_MSG_STATUS_LOCAL_TIMEOUT;
			CNETERR("Timeout sending data to %s (%pISp) the network or that node may be down.\n",
				libcfs_idstr(&peer_ni->ksnp_id),
				&conn->ksnc_peeraddr);
				spin_unlock_bh(&sched->kss_lock);
				return conn;
		}
		spin_unlock_bh(&sched->kss_lock);
	}

	return (NULL);
}

static inline void
ksocknal_flush_stale_txs(struct ksock_peer_ni *peer_ni)
{
	struct ksock_tx	*tx;
	LIST_HEAD(stale_txs);

	write_lock_bh(&ksocknal_data.ksnd_global_lock);

	while ((tx = list_first_entry_or_null(&peer_ni->ksnp_tx_queue,
					      struct ksock_tx,
					      tx_list)) != NULL) {
		if (ktime_get_seconds() < tx->tx_deadline)
			break;

		tx->tx_hstatus = LNET_MSG_STATUS_LOCAL_TIMEOUT;

		list_move_tail(&tx->tx_list, &stale_txs);
	}

	write_unlock_bh(&ksocknal_data.ksnd_global_lock);

	ksocknal_txlist_done(peer_ni->ksnp_ni, &stale_txs, -ETIMEDOUT);
}

static int
ksocknal_send_keepalive_locked(struct ksock_peer_ni *peer_ni)
__must_hold(&ksocknal_data.ksnd_global_lock)
{
	struct ksock_sched *sched;
	struct ksock_conn *conn;
	struct ksock_tx *tx;

	/* last_alive will be updated by create_conn */
	if (list_empty(&peer_ni->ksnp_conns))
                return 0;

	if (peer_ni->ksnp_proto != &ksocknal_protocol_v3x &&
	    peer_ni->ksnp_proto != &ksocknal_protocol_v4x)
		return 0;

        if (*ksocknal_tunables.ksnd_keepalive <= 0 ||
	    ktime_get_seconds() < peer_ni->ksnp_last_alive +
				  *ksocknal_tunables.ksnd_keepalive)
                return 0;

	if (ktime_get_seconds() < peer_ni->ksnp_send_keepalive)
                return 0;

        /* retry 10 secs later, so we wouldn't put pressure
         * on this peer_ni if we failed to send keepalive this time */
	peer_ni->ksnp_send_keepalive = ktime_get_seconds() + 10;

        conn = ksocknal_find_conn_locked(peer_ni, NULL, 1);
        if (conn != NULL) {
                sched = conn->ksnc_scheduler;

		spin_lock_bh(&sched->kss_lock);
		if (!list_empty(&conn->ksnc_tx_queue)) {
			spin_unlock_bh(&sched->kss_lock);
			/* there is an queued ACK, don't need keepalive */
			return 0;
		}

		spin_unlock_bh(&sched->kss_lock);
	}

	read_unlock(&ksocknal_data.ksnd_global_lock);

	/* cookie = 1 is reserved for keepalive PING */
	tx = ksocknal_alloc_tx_noop(1, 1);
	if (tx == NULL) {
		read_lock(&ksocknal_data.ksnd_global_lock);
		return -ENOMEM;
	}

	if (ksocknal_launch_packet(peer_ni->ksnp_ni, tx, &peer_ni->ksnp_id)
	    == 0) {
		read_lock(&ksocknal_data.ksnd_global_lock);
		return 1;
	}

	ksocknal_free_tx(tx);
	read_lock(&ksocknal_data.ksnd_global_lock);

	return -EIO;
}


static void
ksocknal_check_peer_timeouts(int idx)
{
	struct hlist_head *peers = &ksocknal_data.ksnd_peers[idx];
	struct ksock_peer_ni *peer_ni;
	struct ksock_conn *conn;
	struct ksock_tx *tx;

 again:
	/* NB. We expect to have a look at all the peers and not find any
	 * connections to time out, so we just use a shared lock while we
	 * take a look...
	 */
	read_lock(&ksocknal_data.ksnd_global_lock);

	hlist_for_each_entry(peer_ni, peers, ksnp_list) {
		struct ksock_tx *tx_stale;
		time64_t deadline = 0;
		int resid = 0;
		int n = 0;

		if (ksocknal_send_keepalive_locked(peer_ni) != 0) {
			read_unlock(&ksocknal_data.ksnd_global_lock);
			goto again;
		}

		conn = ksocknal_find_timed_out_conn(peer_ni);

		if (conn != NULL) {
			read_unlock(&ksocknal_data.ksnd_global_lock);

			ksocknal_close_conn_and_siblings(conn, -ETIMEDOUT);

			/* NB we won't find this one again, but we can't
			 * just proceed with the next peer_ni, since we dropped
			 * ksnd_global_lock and it might be dead already!
			 */
			ksocknal_conn_decref(conn);
			goto again;
		}

		/* we can't process stale txs right here because we're
		 * holding only shared lock
		 */
		tx = list_first_entry_or_null(&peer_ni->ksnp_tx_queue,
					      struct ksock_tx, tx_list);
		if (tx && ktime_get_seconds() >= tx->tx_deadline) {
			ksocknal_peer_addref(peer_ni);
			read_unlock(&ksocknal_data.ksnd_global_lock);

			ksocknal_flush_stale_txs(peer_ni);

			ksocknal_peer_decref(peer_ni);
			goto again;
		}

		if (list_empty(&peer_ni->ksnp_zc_req_list))
			continue;

		tx_stale = NULL;
		spin_lock(&peer_ni->ksnp_lock);
		list_for_each_entry(tx, &peer_ni->ksnp_zc_req_list, tx_zc_list) {
			if (ktime_get_seconds() < tx->tx_deadline)
                                break;
                        /* ignore the TX if connection is being closed */
                        if (tx->tx_conn->ksnc_closing)
                                continue;
                        n++;
			if (tx_stale == NULL)
				tx_stale = tx;
                }

		if (tx_stale == NULL) {
			spin_unlock(&peer_ni->ksnp_lock);
			continue;
		}

		deadline = tx_stale->tx_deadline;
		resid    = tx_stale->tx_resid;
		conn     = tx_stale->tx_conn;
		ksocknal_conn_addref(conn);

		spin_unlock(&peer_ni->ksnp_lock);
		read_unlock(&ksocknal_data.ksnd_global_lock);

		CERROR("Total %d stale ZC_REQs for peer_ni %s detected; the "
		       "oldest(%p) timed out %lld secs ago, "
		       "resid: %d, wmem: %d\n",
		       n, libcfs_nidstr(&peer_ni->ksnp_id.nid), tx_stale,
		       ktime_get_seconds() - deadline,
		       resid, conn->ksnc_sock->sk->sk_wmem_queued);

                ksocknal_close_conn_and_siblings (conn, -ETIMEDOUT);
                ksocknal_conn_decref(conn);
                goto again;
        }

	read_unlock(&ksocknal_data.ksnd_global_lock);
}

int ksocknal_reaper(void *arg)
{
	wait_queue_entry_t wait;
	struct ksock_conn *conn;
	struct ksock_sched *sched;
	LIST_HEAD(enomem_conns);
	int nenomem_conns;
	time64_t timeout;
	int i;
	int peer_index = 0;
	time64_t deadline = ktime_get_seconds();

	init_wait(&wait);

	spin_lock_bh(&ksocknal_data.ksnd_reaper_lock);

        while (!ksocknal_data.ksnd_shuttingdown) {
		conn = list_first_entry_or_null(&ksocknal_data.ksnd_deathrow_conns,
						struct ksock_conn, ksnc_list);
		if (conn) {
			list_del(&conn->ksnc_list);

			spin_unlock_bh(&ksocknal_data.ksnd_reaper_lock);

			ksocknal_terminate_conn(conn);
			ksocknal_conn_decref(conn);

			spin_lock_bh(&ksocknal_data.ksnd_reaper_lock);
                        continue;
                }

		conn = list_first_entry_or_null(&ksocknal_data.ksnd_zombie_conns,
						struct ksock_conn, ksnc_list);
		if (conn) {
			list_del(&conn->ksnc_list);

			spin_unlock_bh(&ksocknal_data.ksnd_reaper_lock);

			ksocknal_destroy_conn(conn);

			spin_lock_bh(&ksocknal_data.ksnd_reaper_lock);
			continue;
		}

		list_splice_init(&ksocknal_data.ksnd_enomem_conns,
				 &enomem_conns);

		spin_unlock_bh(&ksocknal_data.ksnd_reaper_lock);

                /* reschedule all the connections that stalled with ENOMEM... */
                nenomem_conns = 0;
		while ((conn = list_first_entry_or_null(&enomem_conns,
							struct ksock_conn,
							ksnc_tx_list)) != NULL) {
			list_del(&conn->ksnc_tx_list);

                        sched = conn->ksnc_scheduler;

			spin_lock_bh(&sched->kss_lock);

			LASSERT(conn->ksnc_tx_scheduled);
			conn->ksnc_tx_ready = 1;
			list_add_tail(&conn->ksnc_tx_list,
					  &sched->kss_tx_conns);
			wake_up(&sched->kss_waitq);

			spin_unlock_bh(&sched->kss_lock);
                        nenomem_conns++;
                }

		/* careful with the jiffy wrap... */
		while ((timeout = deadline - ktime_get_seconds()) <= 0) {
			const int n = 4;
			const int p = 1;
			int  chunk = HASH_SIZE(ksocknal_data.ksnd_peers);
			unsigned int lnd_timeout;

			/* Time to check for timeouts on a few more peers: I
			 * do checks every 'p' seconds on a proportion of the
			 * peer_ni table and I need to check every connection
			 * 'n' times within a timeout interval, to ensure I
			 * detect a timeout on any connection within (n+1)/n
			 * times the timeout interval.
			 */

			lnd_timeout = ksocknal_timeout();
			if (lnd_timeout > n * p)
				chunk = (chunk * n * p) / lnd_timeout;
			if (chunk == 0)
				chunk = 1;

			for (i = 0; i < chunk; i++) {
				ksocknal_check_peer_timeouts(peer_index);
				peer_index = (peer_index + 1) %
					HASH_SIZE(ksocknal_data.ksnd_peers);
			}

			deadline += p;
		}

                if (nenomem_conns != 0) {
                        /* Reduce my timeout if I rescheduled ENOMEM conns.
                         * This also prevents me getting woken immediately
                         * if any go back on my enomem list. */
                        timeout = SOCKNAL_ENOMEM_RETRY;
                }
		ksocknal_data.ksnd_reaper_waketime = ktime_get_seconds() +
						     timeout;

		set_current_state(TASK_INTERRUPTIBLE);
		add_wait_queue(&ksocknal_data.ksnd_reaper_waitq, &wait);

		if (!ksocknal_data.ksnd_shuttingdown &&
		    list_empty(&ksocknal_data.ksnd_deathrow_conns) &&
		    list_empty(&ksocknal_data.ksnd_zombie_conns))
			schedule_timeout(cfs_time_seconds(timeout));

		set_current_state(TASK_RUNNING);
		remove_wait_queue(&ksocknal_data.ksnd_reaper_waitq, &wait);

		spin_lock_bh(&ksocknal_data.ksnd_reaper_lock);
	}

	spin_unlock_bh(&ksocknal_data.ksnd_reaper_lock);

	ksocknal_thread_fini();
	return 0;
}
