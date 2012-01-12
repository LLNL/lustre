/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.sun.com/software/products/lustre/docs/GPLv2.pdf
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * libcfs/libcfs/tracefile.c
 *
 * Author: Zach Brown <zab@clusterfs.com>
 * Author: Phil Schwan <phil@clusterfs.com>
 */


#define DEBUG_SUBSYSTEM S_LNET
#define LUSTRE_TRACEFILE_PRIVATE
#include "tracefile.h"

#include <libcfs/libcfs.h>

/* XXX move things up to the top, comment */
union cfs_trace_data_union (*cfs_trace_data[TCD_MAX_TYPES])[CFS_NR_CPUS] __cacheline_aligned;

char cfs_tracefile[TRACEFILE_NAME_SIZE];
long long cfs_tracefile_size = CFS_TRACEFILE_SIZE;
static struct tracefiled_ctl trace_tctl;
cfs_semaphore_t cfs_trace_thread_sem;
static int thread_running = 0;

cfs_atomic_t cfs_tage_allocated = CFS_ATOMIC_INIT(0);

static void put_pages_on_tcd_daemon_list(struct page_collection *pc,
                                         struct cfs_trace_cpu_data *tcd);

static inline struct cfs_trace_page *
cfs_tage_from_list(cfs_list_t *list)
{
        return cfs_list_entry(list, struct cfs_trace_page, linkage);
}

static struct cfs_trace_page *cfs_tage_alloc(int gfp)
{
        cfs_page_t            *page;
        struct cfs_trace_page *tage;

        /* My caller is trying to free memory */
        if (!cfs_in_interrupt() && cfs_memory_pressure_get())
                return NULL;

        /*
         * Don't spam console with allocation failures: they will be reported
         * by upper layer anyway.
         */
        gfp |= CFS_ALLOC_NOWARN;
        page = cfs_alloc_page(gfp);
        if (page == NULL)
                return NULL;

        tage = cfs_alloc(sizeof(*tage), gfp);
        if (tage == NULL) {
                cfs_free_page(page);
                return NULL;
        }

        tage->page = page;
        cfs_atomic_inc(&cfs_tage_allocated);
        return tage;
}

static void cfs_tage_free(struct cfs_trace_page *tage)
{
        __LASSERT(tage != NULL);
        __LASSERT(tage->page != NULL);

        cfs_free_page(tage->page);
        cfs_free(tage);
        cfs_atomic_dec(&cfs_tage_allocated);
}

static void cfs_tage_to_tail(struct cfs_trace_page *tage,
                             cfs_list_t *queue)
{
        __LASSERT(tage != NULL);
        __LASSERT(queue != NULL);

        cfs_list_move_tail(&tage->linkage, queue);
}

int cfs_trace_refill_stock(struct cfs_trace_cpu_data *tcd, int gfp,
                           cfs_list_t *stock)
{
        int i;

        /*
         * XXX nikita: do NOT call portals_debug_msg() (CDEBUG/ENTRY/EXIT)
         * from here: this will lead to infinite recursion.
         */

        for (i = 0; i + tcd->tcd_cur_stock_pages < TCD_STOCK_PAGES ; ++ i) {
                struct cfs_trace_page *tage;

                tage = cfs_tage_alloc(gfp);
                if (tage == NULL)
                        break;
                cfs_list_add_tail(&tage->linkage, stock);
        }
        return i;
}

/* return a page that has 'len' bytes left at the end */
static struct cfs_trace_page *
cfs_trace_get_tage_try(struct cfs_trace_cpu_data *tcd, unsigned long len)
{
        struct cfs_trace_page *tage;

        if (tcd->tcd_cur_pages > 0) {
                __LASSERT(!cfs_list_empty(&tcd->tcd_pages));
                tage = cfs_tage_from_list(tcd->tcd_pages.prev);
                if (tage->used + len <= CFS_PAGE_SIZE)
                        return tage;
        }

        if (tcd->tcd_cur_pages < tcd->tcd_max_pages) {
                if (tcd->tcd_cur_stock_pages > 0) {
                        tage = cfs_tage_from_list(tcd->tcd_stock_pages.prev);
                        -- tcd->tcd_cur_stock_pages;
                        cfs_list_del_init(&tage->linkage);
                } else {
                        tage = cfs_tage_alloc(CFS_ALLOC_ATOMIC);
                        if (tage == NULL) {
                                if (printk_ratelimit())
                                        printk(CFS_KERN_WARNING
                                               "cannot allocate a tage (%ld)\n",
                                       tcd->tcd_cur_pages);
                                return NULL;
                        }
                }

                tage->used = 0;
                tage->cpu = cfs_smp_processor_id();
                tage->type = tcd->tcd_type;
                cfs_list_add_tail(&tage->linkage, &tcd->tcd_pages);
                tcd->tcd_cur_pages++;

                if (tcd->tcd_cur_pages > 8 && thread_running) {
                        struct tracefiled_ctl *tctl = &trace_tctl;
                        /*
                         * wake up tracefiled to process some pages.
                         */
                        cfs_waitq_signal(&tctl->tctl_waitq);
                }
                return tage;
        }
        return NULL;
}

static void cfs_tcd_shrink(struct cfs_trace_cpu_data *tcd)
{
        int pgcount = tcd->tcd_cur_pages / 10;
        struct page_collection pc;
        struct cfs_trace_page *tage;
        struct cfs_trace_page *tmp;

        /*
         * XXX nikita: do NOT call portals_debug_msg() (CDEBUG/ENTRY/EXIT)
         * from here: this will lead to infinite recursion.
         */

        if (printk_ratelimit())
                printk(CFS_KERN_WARNING "debug daemon buffer overflowed; "
                       "discarding 10%% of pages (%d of %ld)\n",
                       pgcount + 1, tcd->tcd_cur_pages);

        CFS_INIT_LIST_HEAD(&pc.pc_pages);
        cfs_spin_lock_init(&pc.pc_lock);

        cfs_list_for_each_entry_safe_typed(tage, tmp, &tcd->tcd_pages,
                                           struct cfs_trace_page, linkage) {
                if (pgcount-- == 0)
                        break;

                cfs_list_move_tail(&tage->linkage, &pc.pc_pages);
                tcd->tcd_cur_pages--;
        }
        put_pages_on_tcd_daemon_list(&pc, tcd);
}

/* return a page that has 'len' bytes left at the end */
static struct cfs_trace_page *cfs_trace_get_tage(struct cfs_trace_cpu_data *tcd,
                                                 unsigned long len)
{
        struct cfs_trace_page *tage;

        /*
         * XXX nikita: do NOT call portals_debug_msg() (CDEBUG/ENTRY/EXIT)
         * from here: this will lead to infinite recursion.
         */

        if (len > CFS_PAGE_SIZE) {
                printk(CFS_KERN_ERR
                       "cowardly refusing to write %lu bytes in a page\n", len);
                return NULL;
        }

        tage = cfs_trace_get_tage_try(tcd, len);
        if (tage != NULL)
                return tage;
        if (thread_running)
                cfs_tcd_shrink(tcd);
        if (tcd->tcd_cur_pages > 0) {
                tage = cfs_tage_from_list(tcd->tcd_pages.next);
                tage->used = 0;
                cfs_tage_to_tail(tage, &tcd->tcd_pages);
        }
        return tage;
}

int libcfs_debug_vmsg1(struct libcfs_debug_msg_data *msgdata,
                       const char *format, ...)
{
        va_list args;
        int     rc;

        va_start(args, format);
        rc = libcfs_debug_vmsg2(msgdata, format, args, NULL);
        va_end(args);

        return rc;
}
EXPORT_SYMBOL(libcfs_debug_vmsg1);

int libcfs_debug_vmsg2(struct libcfs_debug_msg_data *msgdata, const char *format1,
                       va_list args, const char *format2, ...)
{
        struct cfs_trace_cpu_data *tcd = NULL;
        struct ptldebug_header     header = {0};
        struct cfs_trace_page     *tage;
        /* string_buf is used only if tcd != NULL, and is always set then */
        char                      *string_buf = NULL;
        char                      *debug_buf;
        int                        known_size;
        int                        needed = 85; /* average message length */
        int                        max_nob;
        va_list                    ap;
        int                        depth;
        int                        i;
        int                        remain;
        int                        mask = msgdata->msg_mask;
        char                      *file = (char *) msgdata->msg_file;
        cfs_debug_limit_state_t   *cdls = msgdata->msg_cdls;

        if (strchr(file, '/'))
                file = strrchr(file, '/') + 1;

        tcd = cfs_trace_get_tcd();

        /* cfs_trace_get_tcd() grabs a lock, which disables preemption and
         * pins us to a particular CPU.  This avoids an smp_processor_id()
         * warning on Linux when debugging is enabled. */
        cfs_set_ptldebug_header(&header, msgdata, CDEBUG_STACK());

        if (tcd == NULL)                /* arch may not log in IRQ context */
                goto console;

        if (tcd->tcd_cur_pages == 0)
                header.ph_flags |= PH_FLAG_FIRST_RECORD;

        if (tcd->tcd_shutting_down) {
                cfs_trace_put_tcd(tcd);
                tcd = NULL;
                goto console;
        }

        depth = __current_nesting_level();
        known_size = strlen(file) + 1 + depth;
        if (msgdata->msg_fn)
                known_size += strlen(msgdata->msg_fn) + 1;

        if (libcfs_debug_binary)
                known_size += sizeof(header);

        /*/
         * '2' used because vsnprintf return real size required for output
         * _without_ terminating NULL.
         * if needed is to small for this format.
         */
        for (i = 0; i < 2; i++) {
                tage = cfs_trace_get_tage(tcd, needed + known_size + 1);
                if (tage == NULL) {
                        if (needed + known_size > CFS_PAGE_SIZE)
                                mask |= D_ERROR;

                        cfs_trace_put_tcd(tcd);
                        tcd = NULL;
                        goto console;
                }

                string_buf = (char *)cfs_page_address(tage->page) +
                                        tage->used + known_size;

                max_nob = CFS_PAGE_SIZE - tage->used - known_size;
                if (max_nob <= 0) {
                        printk(CFS_KERN_EMERG "negative max_nob: %d\n",
                               max_nob);
                        mask |= D_ERROR;
                        cfs_trace_put_tcd(tcd);
                        tcd = NULL;
                        goto console;
                }

                needed = 0;
                if (format1) {
                        va_copy(ap, args);
                        needed = vsnprintf(string_buf, max_nob, format1, ap);
                        va_end(ap);
                }

                if (format2) {
                        remain = max_nob - needed;
                        if (remain < 0)
                                remain = 0;

                        va_start(ap, format2);
                        needed += vsnprintf(string_buf + needed, remain,
                                            format2, ap);
                        va_end(ap);
                }

                if (needed < max_nob) /* well. printing ok.. */
                        break;
        }

        if (*(string_buf+needed-1) != '\n')
                printk(CFS_KERN_INFO "format at %s:%d:%s doesn't end in newline\n",
                       file, msgdata->msg_line, msgdata->msg_fn);

        header.ph_len = known_size + needed;
        debug_buf = (char *)cfs_page_address(tage->page) + tage->used;

        if (libcfs_debug_binary) {
                memcpy(debug_buf, &header, sizeof(header));
                tage->used += sizeof(header);
                debug_buf += sizeof(header);
        }

        /* indent message according to the nesting level */
        while (depth-- > 0) {
                *(debug_buf++) = '.';
                ++ tage->used;
        }

        strcpy(debug_buf, file);
        tage->used += strlen(file) + 1;
        debug_buf += strlen(file) + 1;

        if (msgdata->msg_fn) {
                strcpy(debug_buf, msgdata->msg_fn);
                tage->used += strlen(msgdata->msg_fn) + 1;
                debug_buf += strlen(msgdata->msg_fn) + 1;
        }

        __LASSERT(debug_buf == string_buf);

        tage->used += needed;
        __LASSERT (tage->used <= CFS_PAGE_SIZE);

console:
        if ((mask & libcfs_printk) == 0) {
                /* no console output requested */
                if (tcd != NULL)
                        cfs_trace_put_tcd(tcd);
                return 1;
        }

        if (cdls != NULL) {
                if (libcfs_console_ratelimit &&
                    cdls->cdls_next != 0 &&     /* not first time ever */
                    !cfs_time_after(cfs_time_current(), cdls->cdls_next)) {
                        /* skipping a console message */
                        cdls->cdls_count++;
                        if (tcd != NULL)
                                cfs_trace_put_tcd(tcd);
                        return 1;
                }

                if (cfs_time_after(cfs_time_current(), cdls->cdls_next +
                                                       libcfs_console_max_delay
                                                       + cfs_time_seconds(10))) {
                        /* last timeout was a long time ago */
                        cdls->cdls_delay /= libcfs_console_backoff * 4;
                } else {
                        cdls->cdls_delay *= libcfs_console_backoff;

                        if (cdls->cdls_delay < libcfs_console_min_delay)
                                cdls->cdls_delay = libcfs_console_min_delay;
                        else if (cdls->cdls_delay > libcfs_console_max_delay)
                                cdls->cdls_delay = libcfs_console_max_delay;
                }

                /* ensure cdls_next is never zero after it's been seen */
                cdls->cdls_next = (cfs_time_current() + cdls->cdls_delay) | 1;
        }

        if (tcd != NULL) {
                cfs_print_to_console(&header, mask, string_buf, needed, file,
                                     msgdata->msg_fn);
                cfs_trace_put_tcd(tcd);
        } else {
                string_buf = cfs_trace_get_console_buffer();

                needed = 0;
                if (format1 != NULL) {
                        va_copy(ap, args);
                        needed = vsnprintf(string_buf,
                                           CFS_TRACE_CONSOLE_BUFFER_SIZE,
                                           format1, ap);
                        va_end(ap);
                }
                if (format2 != NULL) {
                        remain = CFS_TRACE_CONSOLE_BUFFER_SIZE - needed;
                        if (remain > 0) {
                                va_start(ap, format2);
                                needed += vsnprintf(string_buf+needed, remain, format2, ap);
                                va_end(ap);
                        }
                }
                cfs_print_to_console(&header, mask,
                                     string_buf, needed, file, msgdata->msg_fn);

                cfs_trace_put_console_buffer(string_buf);
        }

        if (cdls != NULL && cdls->cdls_count != 0) {
                string_buf = cfs_trace_get_console_buffer();

                needed = snprintf(string_buf, CFS_TRACE_CONSOLE_BUFFER_SIZE,
                         "Skipped %d previous similar message%s\n",
                         cdls->cdls_count, (cdls->cdls_count > 1) ? "s" : "");

                cfs_print_to_console(&header, mask,
                                 string_buf, needed, file, msgdata->msg_fn);

                cfs_trace_put_console_buffer(string_buf);
                cdls->cdls_count = 0;
        }

        return 0;
}
EXPORT_SYMBOL(libcfs_debug_vmsg2);

void
libcfs_assertion_failed(const char *expr, struct libcfs_debug_msg_data *msgdata)
{
        libcfs_debug_msg(msgdata, "ASSERTION(%s) failed\n", expr);
        /* cfs_enter_debugger(); */
        lbug_with_loc(msgdata);
}
EXPORT_SYMBOL(libcfs_assertion_failed);

void
cfs_trace_assertion_failed(const char *str, struct libcfs_debug_msg_data *m)
{
        struct ptldebug_header hdr;

        libcfs_panic_in_progress = 1;
        libcfs_catastrophe = 1;
        cfs_mb();

        cfs_set_ptldebug_header(&hdr, m, CDEBUG_STACK());

        cfs_print_to_console(&hdr, D_EMERG, str, strlen(str),
                             m->msg_file, m->msg_fn);

        LIBCFS_PANIC("Lustre debug assertion failure\n");

        /* not reached */
}

static void
panic_collect_pages(struct page_collection *pc)
{
        /* Do the collect_pages job on a single CPU: assumes that all other
         * CPUs have been stopped during a panic.  If this isn't true for some
         * arch, this will have to be implemented separately in each arch.  */
        int                        i;
        int                        j;
        struct cfs_trace_cpu_data *tcd;

        CFS_INIT_LIST_HEAD(&pc->pc_pages);

        cfs_tcd_for_each(tcd, i, j) {
                cfs_list_splice_init(&tcd->tcd_pages, &pc->pc_pages);
                tcd->tcd_cur_pages = 0;

                if (pc->pc_want_daemon_pages) {
                        cfs_list_splice_init(&tcd->tcd_daemon_pages,
                                             &pc->pc_pages);
                        tcd->tcd_cur_daemon_pages = 0;
                }
        }
}

static void collect_pages_on_all_cpus(struct page_collection *pc)
{
        struct cfs_trace_cpu_data *tcd;
        int i, cpu;

        cfs_spin_lock(&pc->pc_lock);
        cfs_for_each_possible_cpu(cpu) {
                cfs_tcd_for_each_type_lock(tcd, i, cpu) {
                        cfs_list_splice_init(&tcd->tcd_pages, &pc->pc_pages);
                        tcd->tcd_cur_pages = 0;
                        if (pc->pc_want_daemon_pages) {
                                cfs_list_splice_init(&tcd->tcd_daemon_pages,
                                                     &pc->pc_pages);
                                tcd->tcd_cur_daemon_pages = 0;
                        }
                }
        }
        cfs_spin_unlock(&pc->pc_lock);
}

static void collect_pages(struct page_collection *pc)
{
        CFS_INIT_LIST_HEAD(&pc->pc_pages);

        if (libcfs_panic_in_progress)
                panic_collect_pages(pc);
        else
                collect_pages_on_all_cpus(pc);
}

static void put_pages_back_on_all_cpus(struct page_collection *pc)
{
        struct cfs_trace_cpu_data *tcd;
        cfs_list_t *cur_head;
        struct cfs_trace_page *tage;
        struct cfs_trace_page *tmp;
        int i, cpu;

        cfs_spin_lock(&pc->pc_lock);
        cfs_for_each_possible_cpu(cpu) {
                cfs_tcd_for_each_type_lock(tcd, i, cpu) {
                        cur_head = tcd->tcd_pages.next;

                        cfs_list_for_each_entry_safe_typed(tage, tmp,
                                                           &pc->pc_pages,
                                                           struct cfs_trace_page,
                                                           linkage) {

                                __LASSERT_TAGE_INVARIANT(tage);

                                if (tage->cpu != cpu || tage->type != i)
                                        continue;

                                cfs_tage_to_tail(tage, cur_head);
                                tcd->tcd_cur_pages++;
                        }
                }
        }
        cfs_spin_unlock(&pc->pc_lock);
}

static void put_pages_back(struct page_collection *pc)
{
        if (!libcfs_panic_in_progress)
                put_pages_back_on_all_cpus(pc);
}

/* Add pages to a per-cpu debug daemon ringbuffer.  This buffer makes sure that
 * we have a good amount of data at all times for dumping during an LBUG, even
 * if we have been steadily writing (and otherwise discarding) pages via the
 * debug daemon. */
static void put_pages_on_tcd_daemon_list(struct page_collection *pc,
                                         struct cfs_trace_cpu_data *tcd)
{
        struct cfs_trace_page *tage;
        struct cfs_trace_page *tmp;

        cfs_spin_lock(&pc->pc_lock);
        cfs_list_for_each_entry_safe_typed(tage, tmp, &pc->pc_pages,
                                           struct cfs_trace_page, linkage) {

                __LASSERT_TAGE_INVARIANT(tage);

                if (tage->cpu != tcd->tcd_cpu || tage->type != tcd->tcd_type)
                        continue;

                cfs_tage_to_tail(tage, &tcd->tcd_daemon_pages);
                tcd->tcd_cur_daemon_pages++;

                if (tcd->tcd_cur_daemon_pages > tcd->tcd_max_pages) {
                        struct cfs_trace_page *victim;

                        __LASSERT(!cfs_list_empty(&tcd->tcd_daemon_pages));
                        victim = cfs_tage_from_list(tcd->tcd_daemon_pages.next);

                        __LASSERT_TAGE_INVARIANT(victim);

                        cfs_list_del(&victim->linkage);
                        cfs_tage_free(victim);
                        tcd->tcd_cur_daemon_pages--;
                }
        }
        cfs_spin_unlock(&pc->pc_lock);
}

static void put_pages_on_daemon_list(struct page_collection *pc)
{
        struct cfs_trace_cpu_data *tcd;
        int i, cpu;

        cfs_for_each_possible_cpu(cpu) {
                cfs_tcd_for_each_type_lock(tcd, i, cpu)
                        put_pages_on_tcd_daemon_list(pc, tcd);
        }
}

void cfs_trace_debug_print(void)
{
        struct page_collection pc;
        struct cfs_trace_page *tage;
        struct cfs_trace_page *tmp;

        cfs_spin_lock_init(&pc.pc_lock);

        pc.pc_want_daemon_pages = 1;
        collect_pages(&pc);
        cfs_list_for_each_entry_safe_typed(tage, tmp, &pc.pc_pages,
                                           struct cfs_trace_page, linkage) {
                char *p, *file, *fn;
                cfs_page_t *page;

                __LASSERT_TAGE_INVARIANT(tage);

                page = tage->page;
                p = cfs_page_address(page);
                while (p < ((char *)cfs_page_address(page) + tage->used)) {
                        struct ptldebug_header *hdr;
                        int len;
                        hdr = (void *)p;
                        p += sizeof(*hdr);
                        file = p;
                        p += strlen(file) + 1;
                        fn = p;
                        p += strlen(fn) + 1;
                        len = hdr->ph_len - (int)(p - (char *)hdr);

                        cfs_print_to_console(hdr, D_EMERG, p, len, file, fn);

                        p += len;
                }

                cfs_list_del(&tage->linkage);
                cfs_tage_free(tage);
        }
}

int cfs_tracefile_dump_all_pages(char *filename)
{
        struct page_collection pc;
        cfs_file_t *filp;
        struct cfs_trace_page *tage;
        struct cfs_trace_page *tmp;
        int rc;

        CFS_DECL_MMSPACE;

        cfs_tracefile_write_lock();

        filp = cfs_filp_open(filename,
                             O_CREAT|O_EXCL|O_WRONLY|O_LARGEFILE, 0600, &rc);
        if (!filp) {
                if (rc != -EEXIST)
                        printk(CFS_KERN_ERR
                               "LustreError: can't open %s for dump: rc %d\n",
                               filename, rc);
                goto out;
        }

        cfs_spin_lock_init(&pc.pc_lock);
        pc.pc_want_daemon_pages = 1;
        collect_pages(&pc);
        if (cfs_list_empty(&pc.pc_pages)) {
                rc = 0;
                goto close;
        }

        /* ok, for now, just write the pages.  in the future we'll be building
         * iobufs with the pages and calling generic_direct_IO */
        CFS_MMSPACE_OPEN;
        cfs_list_for_each_entry_safe_typed(tage, tmp, &pc.pc_pages,
                                           struct cfs_trace_page, linkage) {

                __LASSERT_TAGE_INVARIANT(tage);

                rc = cfs_filp_write(filp, cfs_page_address(tage->page),
                                    tage->used, cfs_filp_poff(filp));
                if (rc != (int)tage->used) {
                        printk(CFS_KERN_WARNING "wanted to write %u but wrote "
                               "%d\n", tage->used, rc);
                        put_pages_back(&pc);
                        __LASSERT(cfs_list_empty(&pc.pc_pages));
                        break;
                }
                cfs_list_del(&tage->linkage);
                cfs_tage_free(tage);
        }
        CFS_MMSPACE_CLOSE;
        rc = cfs_filp_fsync(filp);
        if (rc)
                printk(CFS_KERN_ERR "sync returns %d\n", rc);
 close:
        cfs_filp_close(filp);
 out:
        cfs_tracefile_write_unlock();
        return rc;
}

void cfs_trace_flush_pages(void)
{
        struct page_collection pc;
        struct cfs_trace_page *tage;
        struct cfs_trace_page *tmp;

        cfs_spin_lock_init(&pc.pc_lock);

        pc.pc_want_daemon_pages = 1;
        collect_pages(&pc);
        cfs_list_for_each_entry_safe_typed(tage, tmp, &pc.pc_pages,
                                           struct cfs_trace_page, linkage) {

                __LASSERT_TAGE_INVARIANT(tage);

                cfs_list_del(&tage->linkage);
                cfs_tage_free(tage);
        }
}

int cfs_trace_copyin_string(char *knl_buffer, int knl_buffer_nob,
                            const char *usr_buffer, int usr_buffer_nob)
{
        int    nob;

        if (usr_buffer_nob > knl_buffer_nob)
                return -EOVERFLOW;

        if (cfs_copy_from_user((void *)knl_buffer,
                           (void *)usr_buffer, usr_buffer_nob))
                return -EFAULT;

        nob = strnlen(knl_buffer, usr_buffer_nob);
        while (nob-- >= 0)                      /* strip trailing whitespace */
                if (!isspace(knl_buffer[nob]))
                        break;

        if (nob < 0)                            /* empty string */
                return -EINVAL;

        if (nob == knl_buffer_nob)              /* no space to terminate */
                return -EOVERFLOW;

        knl_buffer[nob + 1] = 0;                /* terminate */
        return 0;
}

int cfs_trace_copyout_string(char *usr_buffer, int usr_buffer_nob,
                             const char *knl_buffer, char *append)
{
        /* NB if 'append' != NULL, it's a single character to append to the
         * copied out string - usually "\n", for /proc entries and "" (i.e. a
         * terminating zero byte) for sysctl entries */
        int   nob = strlen(knl_buffer);

        if (nob > usr_buffer_nob)
                nob = usr_buffer_nob;

        if (cfs_copy_to_user(usr_buffer, knl_buffer, nob))
                return -EFAULT;

        if (append != NULL && nob < usr_buffer_nob) {
                if (cfs_copy_to_user(usr_buffer + nob, append, 1))
                        return -EFAULT;

                nob++;
        }

        return nob;
}
EXPORT_SYMBOL(cfs_trace_copyout_string);

int cfs_trace_allocate_string_buffer(char **str, int nob)
{
        if (nob > 2 * CFS_PAGE_SIZE)            /* string must be "sensible" */
                return -EINVAL;

        *str = cfs_alloc(nob, CFS_ALLOC_STD | CFS_ALLOC_ZERO);
        if (*str == NULL)
                return -ENOMEM;

        return 0;
}

void cfs_trace_free_string_buffer(char *str, int nob)
{
        cfs_free(str);
}

int cfs_trace_dump_debug_buffer_usrstr(void *usr_str, int usr_str_nob)
{
        char         *str;
        int           rc;

        rc = cfs_trace_allocate_string_buffer(&str, usr_str_nob + 1);
        if (rc != 0)
                return rc;

        rc = cfs_trace_copyin_string(str, usr_str_nob + 1,
                                     usr_str, usr_str_nob);
        if (rc != 0)
                goto out;

#if !defined(__WINNT__)
        if (str[0] != '/') {
                rc = -EINVAL;
                goto out;
        }
#endif
        rc = cfs_tracefile_dump_all_pages(str);
out:
        cfs_trace_free_string_buffer(str, usr_str_nob + 1);
        return rc;
}

int cfs_trace_daemon_command(char *str)
{
        int       rc = 0;

        cfs_tracefile_write_lock();

        if (strcmp(str, "stop") == 0) {
                cfs_tracefile_write_unlock();
                cfs_trace_stop_thread();
                cfs_tracefile_write_lock();
                memset(cfs_tracefile, 0, sizeof(cfs_tracefile));

        } else if (strncmp(str, "size=", 5) == 0) {
                cfs_tracefile_size = simple_strtoul(str + 5, NULL, 0);
                if (cfs_tracefile_size < 10 || cfs_tracefile_size > 20480)
                        cfs_tracefile_size = CFS_TRACEFILE_SIZE;
                else
                        cfs_tracefile_size <<= 20;

        } else if (strlen(str) >= sizeof(cfs_tracefile)) {
                rc = -ENAMETOOLONG;
#ifndef __WINNT__
        } else if (str[0] != '/') {
                rc = -EINVAL;
#endif
        } else {
                strcpy(cfs_tracefile, str);

                printk(CFS_KERN_INFO
                       "Lustre: debug daemon will attempt to start writing "
                       "to %s (%lukB max)\n", cfs_tracefile,
                       (long)(cfs_tracefile_size >> 10));

                cfs_trace_start_thread();
        }

        cfs_tracefile_write_unlock();
        return rc;
}

int cfs_trace_daemon_command_usrstr(void *usr_str, int usr_str_nob)
{
        char *str;
        int   rc;

        rc = cfs_trace_allocate_string_buffer(&str, usr_str_nob + 1);
        if (rc != 0)
                return rc;

        rc = cfs_trace_copyin_string(str, usr_str_nob + 1,
                                 usr_str, usr_str_nob);
        if (rc == 0)
                rc = cfs_trace_daemon_command(str);

        cfs_trace_free_string_buffer(str, usr_str_nob + 1);
        return rc;
}

int cfs_trace_set_debug_mb(int mb)
{
        int i;
        int j;
        int pages;
        int limit = cfs_trace_max_debug_mb();
        struct cfs_trace_cpu_data *tcd;

        if (mb < cfs_num_possible_cpus()) {
                printk(KERN_ERR "Cannot set debug_mb to %d, the value should be >= %d\n",
                       mb, cfs_num_possible_cpus());
                return -EINVAL;
        }

        if (mb > limit) {
                printk(CFS_KERN_ERR "Lustre: Refusing to set debug buffer size "
                       "to %dMB - limit is %d\n", mb, limit);
                return -EINVAL;
        }

        mb /= cfs_num_possible_cpus();
        pages = mb << (20 - CFS_PAGE_SHIFT);

        cfs_tracefile_write_lock();

        cfs_tcd_for_each(tcd, i, j)
                tcd->tcd_max_pages = (pages * tcd->tcd_pages_factor) / 100;

        cfs_tracefile_write_unlock();

        return 0;
}

int cfs_trace_set_debug_mb_usrstr(void *usr_str, int usr_str_nob)
{
        char     str[32];
        int      rc;

        rc = cfs_trace_copyin_string(str, sizeof(str), usr_str, usr_str_nob);
        if (rc < 0)
                return rc;

        return cfs_trace_set_debug_mb(simple_strtoul(str, NULL, 0));
}

int cfs_trace_get_debug_mb(void)
{
        int i;
        int j;
        struct cfs_trace_cpu_data *tcd;
        int total_pages = 0;

        cfs_tracefile_read_lock();

        cfs_tcd_for_each(tcd, i, j)
                total_pages += tcd->tcd_max_pages;

        cfs_tracefile_read_unlock();

        return (total_pages >> (20 - CFS_PAGE_SHIFT)) + 1;
}

static int tracefiled(void *arg)
{
        struct page_collection pc;
        struct tracefiled_ctl *tctl = arg;
        struct cfs_trace_page *tage;
        struct cfs_trace_page *tmp;
        cfs_file_t *filp;
        int last_loop = 0;
        int rc;

        CFS_DECL_MMSPACE;

        /* we're started late enough that we pick up init's fs context */
        /* this is so broken in uml?  what on earth is going on? */
        cfs_daemonize("ktracefiled");

        cfs_spin_lock_init(&pc.pc_lock);
        cfs_complete(&tctl->tctl_start);

        while (1) {
                cfs_waitlink_t __wait;

                pc.pc_want_daemon_pages = 0;
                collect_pages(&pc);
                if (cfs_list_empty(&pc.pc_pages))
                        goto end_loop;

                filp = NULL;
                cfs_tracefile_read_lock();
                if (cfs_tracefile[0] != 0) {
                        filp = cfs_filp_open(cfs_tracefile,
                                             O_CREAT | O_RDWR | O_LARGEFILE,
                                             0600, &rc);
                        if (!(filp))
                                printk(CFS_KERN_WARNING "couldn't open %s: "
                                       "%d\n", cfs_tracefile, rc);
                }
                cfs_tracefile_read_unlock();
                if (filp == NULL) {
                        put_pages_on_daemon_list(&pc);
                        __LASSERT(cfs_list_empty(&pc.pc_pages));
                        goto end_loop;
                }

                CFS_MMSPACE_OPEN;

                cfs_list_for_each_entry_safe_typed(tage, tmp, &pc.pc_pages,
                                                   struct cfs_trace_page,
                                                   linkage) {
                        static loff_t f_pos;

                        __LASSERT_TAGE_INVARIANT(tage);

                        if (f_pos >= (off_t)cfs_tracefile_size)
                                f_pos = 0;
                        else if (f_pos > (off_t)cfs_filp_size(filp))
                                f_pos = cfs_filp_size(filp);

                        rc = cfs_filp_write(filp, cfs_page_address(tage->page),
                                            tage->used, &f_pos);
                        if (rc != (int)tage->used) {
                                printk(CFS_KERN_WARNING "wanted to write %u "
                                       "but wrote %d\n", tage->used, rc);
                                put_pages_back(&pc);
                                __LASSERT(cfs_list_empty(&pc.pc_pages));
                        }
                }
                CFS_MMSPACE_CLOSE;

                cfs_filp_close(filp);
                put_pages_on_daemon_list(&pc);
                if (!cfs_list_empty(&pc.pc_pages)) {
                        int i;

                        printk(CFS_KERN_ALERT "Lustre: trace pages aren't "
                               " empty\n");
                        printk(CFS_KERN_ERR "total cpus(%d): ",
                               cfs_num_possible_cpus());
                        for (i = 0; i < cfs_num_possible_cpus(); i++)
                                if (cfs_cpu_online(i))
                                        printk(CFS_KERN_ERR "%d(on) ", i);
                                else
                                        printk(CFS_KERN_ERR "%d(off) ", i);
                        printk(CFS_KERN_ERR "\n");

                        i = 0;
                        cfs_list_for_each_entry_safe(tage, tmp, &pc.pc_pages,
                                                     linkage)
                                printk(CFS_KERN_ERR "page %d belongs to cpu "
                                       "%d\n", ++i, tage->cpu);
                        printk(CFS_KERN_ERR "There are %d pages unwritten\n",
                               i);
                }
                __LASSERT(cfs_list_empty(&pc.pc_pages));
end_loop:
                if (cfs_atomic_read(&tctl->tctl_shutdown)) {
                        if (last_loop == 0) {
                                last_loop = 1;
                                continue;
                        } else {
                                break;
                        }
                }
                cfs_waitlink_init(&__wait);
                cfs_waitq_add(&tctl->tctl_waitq, &__wait);
                cfs_set_current_state(CFS_TASK_INTERRUPTIBLE);
                cfs_waitq_timedwait(&__wait, CFS_TASK_INTERRUPTIBLE,
                                    cfs_time_seconds(1));
                cfs_waitq_del(&tctl->tctl_waitq, &__wait);
        }
        cfs_complete(&tctl->tctl_stop);
        return 0;
}

int cfs_trace_start_thread(void)
{
        struct tracefiled_ctl *tctl = &trace_tctl;
        int rc = 0;

        cfs_mutex_down(&cfs_trace_thread_sem);
        if (thread_running)
                goto out;

        cfs_init_completion(&tctl->tctl_start);
        cfs_init_completion(&tctl->tctl_stop);
        cfs_waitq_init(&tctl->tctl_waitq);
        cfs_atomic_set(&tctl->tctl_shutdown, 0);

        if (cfs_create_thread(tracefiled, tctl, 0) < 0) {
                rc = -ECHILD;
                goto out;
        }

        cfs_wait_for_completion(&tctl->tctl_start);
        thread_running = 1;
out:
        cfs_mutex_up(&cfs_trace_thread_sem);
        return rc;
}

void cfs_trace_stop_thread(void)
{
        struct tracefiled_ctl *tctl = &trace_tctl;

        cfs_mutex_down(&cfs_trace_thread_sem);
        if (thread_running) {
                printk(CFS_KERN_INFO
                       "Lustre: shutting down debug daemon thread...\n");
                cfs_atomic_set(&tctl->tctl_shutdown, 1);
                cfs_wait_for_completion(&tctl->tctl_stop);
                thread_running = 0;
        }
        cfs_mutex_up(&cfs_trace_thread_sem);
}

int cfs_tracefile_init(int max_pages)
{
        struct cfs_trace_cpu_data *tcd;
        int                    i;
        int                    j;
        int                    rc;
        int                    factor;

        rc = cfs_tracefile_init_arch();
        if (rc != 0)
                return rc;

        cfs_tcd_for_each(tcd, i, j) {
                /* tcd_pages_factor is initialized int tracefile_init_arch. */
                factor = tcd->tcd_pages_factor;
                CFS_INIT_LIST_HEAD(&tcd->tcd_pages);
                CFS_INIT_LIST_HEAD(&tcd->tcd_stock_pages);
                CFS_INIT_LIST_HEAD(&tcd->tcd_daemon_pages);
                tcd->tcd_cur_pages = 0;
                tcd->tcd_cur_stock_pages = 0;
                tcd->tcd_cur_daemon_pages = 0;
                tcd->tcd_max_pages = (max_pages * factor) / 100;
                LASSERT(tcd->tcd_max_pages > 0);
                tcd->tcd_shutting_down = 0;
        }

        return 0;
}

static void trace_cleanup_on_all_cpus(void)
{
        struct cfs_trace_cpu_data *tcd;
        struct cfs_trace_page *tage;
        struct cfs_trace_page *tmp;
        int i, cpu;

        cfs_for_each_possible_cpu(cpu) {
                cfs_tcd_for_each_type_lock(tcd, i, cpu) {
                        tcd->tcd_shutting_down = 1;

                        cfs_list_for_each_entry_safe_typed(tage, tmp,
                                                           &tcd->tcd_pages,
                                                           struct cfs_trace_page,
                                                           linkage) {
                                __LASSERT_TAGE_INVARIANT(tage);

                                cfs_list_del(&tage->linkage);
                                cfs_tage_free(tage);
                        }

                        tcd->tcd_cur_pages = 0;
                }
        }
}

static void cfs_trace_cleanup(void)
{
        struct page_collection pc;

        CFS_INIT_LIST_HEAD(&pc.pc_pages);
        cfs_spin_lock_init(&pc.pc_lock);

        trace_cleanup_on_all_cpus();

        cfs_tracefile_fini_arch();
}

void cfs_tracefile_exit(void)
{
        cfs_trace_stop_thread();
        cfs_trace_cleanup();
}
