/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 *
 *   Author: Eric Barton <eric@bartonsoftware.com>
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

#include "socklnd.h"

static int sock_timeout = 50;
CFS_MODULE_PARM(sock_timeout, "i", int, 0644,
                "dead socket timeout (seconds)");

static int credits = 256;
CFS_MODULE_PARM(credits, "i", int, 0444,
                "# concurrent sends");

static int peer_credits = 8;
CFS_MODULE_PARM(peer_credits, "i", int, 0444,
                "# concurrent sends to 1 peer");

static int peer_buffer_credits = 0;
CFS_MODULE_PARM(peer_buffer_credits, "i", int, 0444,
                "# per-peer router buffer credits");

static int peer_timeout = 180;
CFS_MODULE_PARM(peer_timeout, "i", int, 0444,
                "Seconds without aliveness news to declare peer dead (<=0 to disable)");

static int nconnds = 8;
CFS_MODULE_PARM(nconnds, "i", int, 0444,
                "# connection daemons");

static int min_reconnectms = 1000;
CFS_MODULE_PARM(min_reconnectms, "i", int, 0644,
                "min connection retry interval (mS)");

static int max_reconnectms = 60000;
CFS_MODULE_PARM(max_reconnectms, "i", int, 0644,
                "max connection retry interval (mS)");

#if defined(__APPLE__) && !defined(__DARWIN8__)
# define DEFAULT_EAGER_ACK 1
#else
# define DEFAULT_EAGER_ACK 0
#endif
static int eager_ack = DEFAULT_EAGER_ACK;
CFS_MODULE_PARM(eager_ack, "i", int, 0644,
                "send tcp ack packets eagerly");

static int typed_conns = 1;
CFS_MODULE_PARM(typed_conns, "i", int, 0444,
                "use different sockets for bulk");

static int min_bulk = (1<<10);
CFS_MODULE_PARM(min_bulk, "i", int, 0644,
                "smallest 'large' message");

#ifdef __APPLE__
# ifdef __DARWIN8__
#  define DEFAULT_BUFFER_SIZE (224*1024)
# else
#  define DEFAULT_BUFFER_SIZE (1152 * 1024)
# endif
#else
# define DEFAULT_BUFFER_SIZE 0
#endif
static int tx_buffer_size = DEFAULT_BUFFER_SIZE;
CFS_MODULE_PARM(tx_buffer_size, "i", int, 0644,
                "socket tx buffer size (0 for system default)");

static int rx_buffer_size = DEFAULT_BUFFER_SIZE;
CFS_MODULE_PARM(rx_buffer_size, "i", int, 0644,
                "socket rx buffer size (0 for system default)");

static int nagle = 0;
CFS_MODULE_PARM(nagle, "i", int, 0644,
                "enable NAGLE?");

static int round_robin = 1;
CFS_MODULE_PARM(round_robin, "i", int, 0644,
                "Round robin for multiple interfaces");

static int keepalive = 30;
CFS_MODULE_PARM(keepalive, "i", int, 0644,
                "# seconds before send keepalive");

static int keepalive_idle = 30;
CFS_MODULE_PARM(keepalive_idle, "i", int, 0644,
                "# idle seconds before probe");

#ifdef HAVE_BGL_SUPPORT
#define DEFAULT_KEEPALIVE_COUNT  100
#else
#define DEFAULT_KEEPALIVE_COUNT  5
#endif
static int keepalive_count = DEFAULT_KEEPALIVE_COUNT;
CFS_MODULE_PARM(keepalive_count, "i", int, 0644,
                "# missed probes == dead");

static int keepalive_intvl = 5;
CFS_MODULE_PARM(keepalive_intvl, "i", int, 0644,
                "seconds between probes");

static int enable_csum = 0;
CFS_MODULE_PARM(enable_csum, "i", int, 0644,
                "enable check sum");

static int inject_csum_error = 0;
CFS_MODULE_PARM(inject_csum_error, "i", int, 0644,
                "set non-zero to inject a checksum error");
#ifdef CPU_AFFINITY
static int enable_irq_affinity = 0;
CFS_MODULE_PARM(enable_irq_affinity, "i", int, 0644,
                "enable IRQ affinity");
#endif

static int nonblk_zcack = 1;
CFS_MODULE_PARM(nonblk_zcack, "i", int, 0644,
                "always send ZC-ACK on non-blocking connection");

static unsigned int zc_min_payload = (16 << 10);
CFS_MODULE_PARM(zc_min_payload, "i", int, 0644,
                "minimum payload size to zero copy");

static unsigned int zc_recv = 0;
CFS_MODULE_PARM(zc_recv, "i", int, 0644,
                "enable ZC recv for Chelsio driver");

static unsigned int zc_recv_min_nfrags = 16;
CFS_MODULE_PARM(zc_recv_min_nfrags, "i", int, 0644,
                "minimum # of fragments to enable ZC recv");

#ifdef SOCKNAL_BACKOFF
static int backoff_init = 3;
CFS_MODULE_PARM(backoff_init, "i", int, 0644,
                "seconds for initial tcp backoff");

static int backoff_max = 3;
CFS_MODULE_PARM(backoff_max, "i", int, 0644,
                "seconds for maximum tcp backoff");
#endif

#if SOCKNAL_VERSION_DEBUG
static int protocol = 3;
CFS_MODULE_PARM(protocol, "i", int, 0644,
                "protocol version");
#endif

ksock_tunables_t ksocknal_tunables = {
        .ksnd_timeout         = &sock_timeout,
        .ksnd_credits         = &credits,
        .ksnd_peertxcredits   = &peer_credits,
        .ksnd_peerrtrcredits  = &peer_buffer_credits,
        .ksnd_peertimeout     = &peer_timeout,
        .ksnd_nconnds         = &nconnds,
        .ksnd_min_reconnectms = &min_reconnectms,
        .ksnd_max_reconnectms = &max_reconnectms,
        .ksnd_eager_ack       = &eager_ack,
        .ksnd_typed_conns     = &typed_conns,
        .ksnd_min_bulk        = &min_bulk,
        .ksnd_tx_buffer_size  = &tx_buffer_size,
        .ksnd_rx_buffer_size  = &rx_buffer_size,
        .ksnd_nagle           = &nagle,
        .ksnd_round_robin     = &round_robin,
        .ksnd_keepalive       = &keepalive,
        .ksnd_keepalive_idle  = &keepalive_idle,
        .ksnd_keepalive_count = &keepalive_count,
        .ksnd_keepalive_intvl = &keepalive_intvl,
        .ksnd_enable_csum     = &enable_csum,
        .ksnd_inject_csum_error = &inject_csum_error,
        .ksnd_nonblk_zcack    = &nonblk_zcack,
        .ksnd_zc_min_payload  = &zc_min_payload,
        .ksnd_zc_recv         = &zc_recv,
        .ksnd_zc_recv_min_nfrags = &zc_recv_min_nfrags,
#ifdef CPU_AFFINITY
        .ksnd_irq_affinity    = &enable_irq_affinity,
#endif
#ifdef SOCKNAL_BACKOFF
        .ksnd_backoff_init    = &backoff_init,
        .ksnd_backoff_max     = &backoff_max,
#endif
#if SOCKNAL_VERSION_DEBUG
        .ksnd_protocol        = &protocol,
#endif
};

