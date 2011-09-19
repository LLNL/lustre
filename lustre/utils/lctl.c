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
 * Copyright (c) 2002, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/utils/lctl.c
 *
 * Author: Peter J. Braam <braam@clusterfs.com>
 * Author: Phil Schwan <phil@clusterfs.com>
 * Author: Robert Read <rread@clusterfs.com>
 */

#include <stdlib.h>
#include <stdio.h>
#include <lnet/lnetctl.h>
#include "obdctl.h"
#include <libcfs/libcfsutil.h>

static int jt_quit(int argc, char **argv) {
        Parser_quit(argc, argv);
        return 0;
}

static int jt_noop(int argc, char **argv) {
        return 0;
}

static int jt_opt_ignore_errors(int argc, char **argv) {
        Parser_ignore_errors(1);
        return 0;
}

command_t cmdlist[] = {
        /* Metacommands */
        {"===== metacommands =======", jt_noop, 0, "metacommands"},
        {"--device", jt_opt_device, 0,
         "run <command> after connecting to device <devno>\n"
         "--device <devno> <command [args ...]>"},
        {"--ignore_errors", jt_opt_ignore_errors, 0,
         "ignore errors that occur during script processing\n"
         "--ignore_errors"},
        {"ignore_errors", jt_opt_ignore_errors, 0,
         "ignore errors that occur during script processing\n"
         "ignore_errors"},

        /* User interface commands */
        {"======== control =========", jt_noop, 0, "control commands"},
        {"help", Parser_help, 0, "help"},
        {"lustre_build_version", jt_get_version, 0,
         "print the build version of lustre\n"
         "usage: lustre_build_version"},
        {"exit", jt_quit, 0, "quit"},
        {"quit", jt_quit, 0, "quit"},

        /* Network configuration commands */
        {"===== network config =====", jt_noop, 0, "network config"},
        {"--net", jt_opt_net, 0,"run <command> after setting network to <net>\n"
         "usage: --net <tcp/elan/o2ib/...> <command>"},
        {"network", jt_ptl_network, 0, "configure LNET"
         "usage: network up|down"},
        {"net", jt_ptl_network, 0, "configure LNET"
         "usage: net up|down"},
        {"list_nids", jt_ptl_list_nids, 0, "list local NIDs"
         "usage: list_nids [all]"},
        {"which_nid", jt_ptl_which_nid, 0, "choose a NID"
         "usage: which_nid NID [NID...]"},
        {"interface_list", jt_ptl_print_interfaces,0,"print interface entries\n"
         "usage: interface_list"},
        {"peer_list", jt_ptl_print_peers, 0, "print peer entries\n"
         "usage: peer_list"},
        {"conn_list", jt_ptl_print_connections, 0,
         "print all the connected remote nid\n"
         "usage: conn_list"},
        {"active_tx", jt_ptl_print_active_txs, 0, "print active transmits\n"
         "usage: active_tx"},
        {"route_list", jt_ptl_print_routes, 0,
         "print the portals routing table, same as show_route\n"
         "usage: route_list"},
        {"show_route", jt_ptl_print_routes, 0,
         "print the portals routing table, same as route_list\n"
         "usage: show_route"},
        {"ping", jt_ptl_ping, 0, "Check LNET connectivity\n"
         "usage: ping nid [timeout] [pid]"},

        /* Device selection commands */
        {"==== obd device selection ====", jt_noop, 0, "device selection"},
        {"device", jt_obd_device, 0,
         "set current device to <name|devno>\n"
         "usage: device <%name|$name|devno>"},
        {"device_list", jt_obd_list, 0, "show all devices\n"
         "usage: device_list"},
        {"dl", jt_obd_list, 0, "show all devices\n"
         "usage: dl [-t]"},

        /* Device operations */
        {"==== obd device operations ====", jt_noop, 0, "device operations"},
        {"activate", jt_obd_activate, 0, "activate an import\n"},
        {"deactivate", jt_obd_deactivate, 0, "deactivate an import. "
         "This command should be used on failed OSC devices in an MDT LOV.\n"},
        {"abort_recovery", jt_obd_abort_recovery, 0,
         "abort recovery on a restarting MDT or OST device\n"},
        {"set_timeout", jt_lcfg_set_timeout, 0,
         "usage: conf_param obd_timeout=<secs>\n"},
        {"conf_param", jt_lcfg_mgsparam, 0,"set a permanent config parameter.\n"
         "This command must be run on the MGS node\n"
         "usage: conf_param [-d] <target.keyword=val>\n"
         "  -d  Remove the permanent setting."},
        {"local_param", jt_lcfg_param, 0, "set a temporary, local param\n"
         "usage: local_param <target.keyword=val>\n"},
        {"get_param", jt_lcfg_getparam, 0, "get the Lustre or LNET parameter\n"
         "usage: get_param [-n|-N|-F] <param_path1 param_path2 ...>\n"
         "Get the value of Lustre or LNET parameter from the specified path.\n"
         "The path can contain shell-style filename patterns.\n"
         "  -n  Print only the value and not parameter name.\n"
         "  -N  Print only matched parameter names and not the values.\n"
         "      (Especially useful when using patterns.)\n"
         "  -F  When -N specified, add '/', '@' or '=' for directories,\n"
         "      symlinks and writeable files, respectively."},
        {"set_param", jt_lcfg_setparam, 0, "set the Lustre or LNET parameter\n"
         "usage: set_param [-n] <param_path1=value1 param_path2=value2 ...>\n"
         "Set the value of the Lustre or LNET parameter at the specified path\n"
         "  -n  Disable printing of the key name when printing values."},
        {"list_param", jt_lcfg_listparam, 0,
         "list the Lustre or LNET parameter name\n"
         "usage: list_param [-F|-R] <param_path1 param_path2 ...>\n"
         "List the name of Lustre or LNET parameter from the specified path.\n"
         "  -F  Add '/', '@' or '=' for dirs, symlinks and writeable files,\n"
                "respectively.\n"
         "  -R  Recursively list all parameters under the specified path.\n"},

        /* Debug commands */
        {"==== debugging control ====", jt_noop, 0, "debug"},
        {"debug_daemon", jt_dbg_debug_daemon, 0,
         "debug daemon control and dump to a file\n"
         "usage: debug_daemon {start file [#MB]|stop}"},
        {"debug_kernel", jt_dbg_debug_kernel, 0,
         "get debug buffer and dump to a file, same as dk\n"
         "usage: debug_kernel [file] [raw]"},
        {"dk", jt_dbg_debug_kernel, 0,
         "get debug buffer and dump to a file, same as debug_kernel\n"
         "usage: dk [file] [raw]"},
        {"debug_file", jt_dbg_debug_file, 0,
         "convert a binary debug file dumped by the kernel to ASCII text\n"
         "usage: debug_file <input> [output]"},
        {"df", jt_dbg_debug_file, 0,
         "read debug buffer from input and dump to output, same as debug_file\n"
         "usage: df <input> [output]"},
        {"clear", jt_dbg_clear_debug_buf, 0, "clear kernel debug buffer\n"
         "usage: clear"},
        {"mark", jt_dbg_mark_debug_buf, 0,
         "insert marker text in kernel debug buffer\n"
         "usage: mark <text>"},
        {"filter", jt_dbg_filter, 0, "filter message type\n"
         "usage: filter <subsystem id/debug mask>"},
        {"show", jt_dbg_show, 0, "Show specific type of messages\n"
         "usage: show <subsystem id/debug mask>"},
        {"debug_list", jt_dbg_list, 0, "list subsystem and debug types\n"
         "usage: debug_list <subs/types>"},
        {"modules", jt_dbg_modules, 0,
         "provide gdb-friendly module information\n"
         "usage: modules <path>"},

        /* virtual block operations */
        {"==== virtual block device ====", jt_noop, 0, "virtual block device"},
        {"blockdev_attach", jt_blockdev_attach, 0,
         "attach a lustre regular file to a virtual block device\n"
         "usage: blockdev_attach <file_name> <device_name>"},
        {"blockdev_detach", jt_blockdev_detach, 0,
         "detach a lustre regular file from a virtual block device\n"
         "usage: blockdev_detach <device_name>"},
        {"blockdev_info", jt_blockdev_info, 0,
         "get the device info of a attached file\n"
         "usage: blockdev_info <device_name>"},

        /* Pool commands */
        {"===  Pools ==", jt_noop, 0, "pool management"},
        {"pool_new", jt_pool_cmd, 0,
         "add a new pool\n"
         "usage pool_new <fsname>.<poolname>"},
        {"pool_add", jt_pool_cmd, 0,
         "add the named OSTs to the pool\n"
         "usage pool_add <fsname>.<poolname> <ostname indexed list>"},
        {"pool_remove", jt_pool_cmd, 0,
         "remove the named OST from the pool\n"
         "usage pool_remove <fsname>.<poolname> <ostname indexed list>"},
        {"pool_destroy", jt_pool_cmd, 0,
         "destroy a pool\n"
         "usage pool_destroy <fsname>.<poolname>"},
        {"pool_list", jt_pool_cmd, 0,
         "list pools and pools members\n"
         "usage pool_list  <fsname>[.<poolname>] | <pathname>"},

        /* Changelog commands */
        {"===  Changelogs ==", jt_noop, 0, "changelog user management"},
        {"changelog_register", jt_changelog_register, 0,
         "register a new persistent changelog user, returns id\n"
         "usage:\tdevice <mdtname>\n\tchangelog_register [-n]"},
        {"changelog_deregister", jt_changelog_deregister, 0,
         "deregister an existing changelog user\n"
         "usage:\tdevice <mdtname>\n\tchangelog_deregister <id>"},

        /* Device configuration commands */
        {"== device setup (these are not normally used post 1.4) ==",
                jt_noop, 0, "device config"},
        {"attach", jt_lcfg_attach, 0,
         "set the type, name, and uuid of the current device\n"
         "usage: attach type name uuid"},
        {"detach", jt_obd_detach, 0,
         "remove driver (and name and uuid) from current device\n"
         "usage: detach"},
        {"setup", jt_lcfg_setup, 0,
         "type specific device configuration information\n"
         "usage: setup <args...>"},
        {"cleanup", jt_obd_cleanup, 0, "cleanup previously setup device\n"
         "usage: cleanup [force | failover]"},
        {"dump_cfg", jt_cfg_dump_log, 0,
         "print log of recorded commands for this config to kernel debug log\n"
         "usage: dump_cfg config-uuid-name"},

        /* Test only commands */
        {"==== testing (DANGEROUS) ====", jt_noop, 0, "testing (DANGEROUS)"},
        {"--threads", jt_opt_threads, 0,
         "run <threads> separate instances of <command> on device <devno>\n"
         "--threads <threads> <verbose> <devno> <command [args ...]>"},
        {"lookup", jt_obd_mdc_lookup, 0, "report file mode info\n"
         "usage: lookup <directory> <file>"},
        {"readonly", jt_obd_set_readonly, 0,
         "disable writes to the underlying device\n"},
        {"notransno", jt_obd_no_transno, 0,
         "disable sending of committed-transno updates\n"},
        {"add_uuid", jt_lcfg_add_uuid, 0, "associate a UUID with a nid\n"
         "usage: add_uuid <uuid> <nid>"},
        {"del_uuid", jt_lcfg_del_uuid, 0, "delete a UUID association\n"
         "usage: del_uuid <uuid>"},
        {"add_peer", jt_ptl_add_peer, 0, "add an peer entry\n"
         "usage: add_peer <nid> <host> <port>"},
        {"del_peer", jt_ptl_del_peer, 0, "remove an peer entry\n"
         "usage: del_peer [<nid>] [<ipaddr|pid>]"},
        {"add_conn ", jt_lcfg_add_conn, 0,
         "usage: add_conn <conn_uuid> [priority]\n"},
        {"del_conn ", jt_lcfg_del_conn, 0,
         "usage: del_conn <conn_uuid> \n"},
        {"disconnect", jt_ptl_disconnect, 0, "disconnect from a remote nid\n"
         "usage: disconnect [<nid>]"},
        {"mynid", jt_ptl_mynid, 0, "inform the socknal of the local nid. "
         "The nid defaults to hostname for tcp networks and is automatically "
         "setup for elan/myrinet networks.\n"
         "usage: mynid [<nid>]"},
        {"fail", jt_ptl_fail_nid, 0, "fail/restore communications.\n"
         "Omitting the count means indefinitely, 0 means restore, "
         "otherwise fail 'count' messages.\n"
         "usage: fail nid|_all_ [count]"},
        {"getattr", jt_obd_getattr, 0,
         "get attribute for OST object <objid>\n"
         "usage: getattr <objid>"},
        {"setattr", jt_obd_setattr, 0,
         "set mode attribute for OST object <objid>\n"
         "usage: setattr <objid> <mode>"},
         {"create", jt_obd_create, 0,
         "create <num> OST objects (with <mode>)\n"
         "usage: create [num [mode [verbose [lsm data]]]]"},
        {"destroy", jt_obd_destroy, 0,
         "destroy OST object <objid> [num [verbose]]\n"
         "usage: destroy <num> objects, starting at objid <objid>"},
        {"test_getattr", jt_obd_test_getattr, 0,
         "do <num> getattrs (on OST object <objid> (objid+1 on each thread))\n"
         "usage: test_getattr <num> [verbose [[t]objid]]"},
        {"test_setattr", jt_obd_test_setattr, 0,
         "do <num> setattrs (on OST object <objid> (objid+1 on each thread))\n"
         "usage: test_setattr <num> [verbose [[t]objid]]"},
        {"test_brw", jt_obd_test_brw, 0,
         "do <num> bulk read/writes (<npages> per I/O, on OST object <objid>)\n"
         "usage: test_brw [t]<num> [write [verbose [npages [[t]objid]]]]"},
        {"lwt", jt_ptl_lwt, 0,
         "light-weight tracing\n"
         "usage: lwt start\n"
         "       lwt stop [file]"},
        {"memhog", jt_ptl_memhog, 0,
         "memory pressure testing\n"
         "usage: memhog <page count> [<gfp flags>]"},
        {"getobjversion", jt_get_obj_version, 0,
         "get the version of an object on servers\n"
         "usage: getobjversion <fid>"},

        {"==== obsolete (DANGEROUS) ====", jt_noop, 0, "obsolete (DANGEROUS)"},
        /* some test scripts still use these */
        {"cfg_device", jt_obd_device, 0,
         "set current device to <name>\n"
         "usage: device <name>"},
        {"recover", jt_obd_recover, 0,
         "try to restore a lost connection immediately\n"
         "usage: recover [MDC/OSC device]"},
        /* saving for sanity 44a */
        {"lov_getconfig", jt_obd_lov_getconfig, 0,
         "read lov configuration from an mds device\n"
         "usage: lov_getconfig <mountpoint>"},
        /* Llog operations */
        {"llog_catlist", jt_llog_catlist, 0,
         "list all catalog logs on current device.\n"
         "usage: llog_catlist [<name|devno>]"},
        {"llog_info", jt_llog_info, 0,
         "print log header information.\n"
         "usage: llog_info <$logname|#oid#ogr#ogen> [<name|devno>]\n"
         "       oid, ogr and ogen are hexadecimal."},
        {"llog_print", jt_llog_print, 0,
         "print log content information.\n"
         "usage: llog_print <$logname|#oid#ogr#ogen> [from] [to] [<name|devno>]\n"
         "       oid, ogr and ogen are hexadecimal.\n"
         "       print all records from index 1 by default."},
        {"llog_check", jt_llog_check, 0,
         "print log content information.\n"
         "usage: llog_check <$logname|#oid#ogr#ogen> [from] [to] [<name|devno>]\n"
         "       oid, ogr and ogen are hexadecimal.\n"
         "       check all records from index 1 by default."},
         {"llog_cancel", jt_llog_cancel, 0,
         "cancel one record in log.\n"
         "usage: llog_cancel <catalog id|catalog name> <log id> <index> [<name|devno>]"},
        {"llog_remove", jt_llog_remove, 0,
         "remove one log from catalog or plain log, erase it from disk.\n"
         "usage: llog_remove <catalog id|catalog name> <log id> [<name|devno>]"},
        /* network operations */
        {"add_interface", jt_ptl_add_interface, 0, "add interface entry\n"
         "usage: add_interface ip [netmask]"},
        {"del_interface", jt_ptl_del_interface, 0, "del interface entry\n"
         "usage: del_interface [ip]"},
        {"add_route", jt_ptl_add_route, 0,
         "add an entry to the portals routing table\n"
         "usage: add_route <gateway> <target> [<target>]"},
        {"del_route", jt_ptl_del_route, 0,
         "delete route via gateway to targets from the portals routing table\n"
         "usage: del_route <gateway> [<target>] [<target>]"},
        {"set_route", jt_ptl_notify_router, 0,
         "enable/disable routes via gateway in the portals routing table\n"
         "usage: set_route <gateway> <up/down> [<time>]"},

        { 0, 0, 0, NULL }
};

int lctl_main(int argc, char **argv)
{
        int rc;

        setlinebuf(stdout);

        ptl_initialize(argc, argv);
        if (obd_initialize(argc, argv) < 0)
                exit(2);
        if (dbg_initialize(argc, argv) < 0)
                exit(3);

        Parser_init("lctl > ", cmdlist);

        if (argc > 1) {
                rc = Parser_execarg(argc - 1, argv + 1, cmdlist);
        } else {
                rc = Parser_commands();
        }

        obd_finalize(argc, argv);
        return rc < 0 ? -rc : rc;
}

#ifndef LIBLUSTRE_TEST
int main (int argc, char **argv)
{
        return (lctl_main (argc, argv));
}
#endif
