/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 */

#ifndef _OFD_INTERNAL_H
#define _OFD_INTERNAL_H

#include <lustre/lustre_idl.h>
#include <obd.h>
#include <obd_cksum.h>
#include <lustre_fid.h>
#include <lustre_log.h>

#define OFD_GROUPS_FILE "groups"

#define OFD_INIT_OBJID 0

#define OFD_SUBDIR_COUNT 32 /* set to zero for no subdirs */

#define OFD_ROCOMPAT_SUPP (0)

#define OFD_INCOMPAT_SUPP (OBD_INCOMPAT_GROUPS | OBD_INCOMPAT_OST | \
                              OBD_INCOMPAT_COMMON_LR)

#define OFD_GRANT_CHUNK (2ULL * PTLRPC_MAX_BRW_SIZE)
#define OFD_GRANT_SHRINK_LIMIT (16ULL * OFD_GRANT_CHUNK)
#define GRANT_FOR_LLOG      16

#define OFD_RECOVERY_TIMEOUT (obd_timeout * 5 * CFS_HZ / 2) /* *waves hands* */

extern struct file_operations ofd_per_export_stats_fops;

/* Limit the returned fields marked valid to those that we actually might set */
#define OFD_VALID_FLAGS (LA_TYPE | LA_MODE | LA_SIZE | LA_BLOCKS | \
                            LA_BLKSIZE | LA_ATIME | LA_MTIME | LA_CTIME)

/* per-client-per-object persistent state (LRU) */
struct ofd_mod_data {
        cfs_list_t       fmd_list;      /* linked to fed_mod_list */
        struct lu_fid    fmd_fid;       /* FID being written to */
        __u64            fmd_mactime_xid;/* xid highest {m,a,c}time setattr */
        cfs_time_t       fmd_expire;    /* time when the fmd should expire */
        int              fmd_refcount;  /* reference counter - list holds 1 */
};

#ifdef BGL_SUPPORT
#define OFD_FMD_MAX_NUM_DEFAULT 128 /* many active files per client on BGL */
#else
#define OFD_FMD_MAX_NUM_DEFAULT  32
#endif
#define OFD_FMD_MAX_AGE_DEFAULT ((obd_timeout + 10) * CFS_HZ)

int ofd_fmd_init(void);
void ofd_fmd_exit(void);
struct ofd_mod_data *ofd_fmd_find(struct obd_export *exp,
                                        struct lu_fid *fid);
struct ofd_mod_data *ofd_fmd_get(struct obd_export *exp,
                                       struct lu_fid *fid);
void ofd_fmd_put(struct obd_export *exp, struct ofd_mod_data *fmd);
void ofd_fmd_expire(struct obd_export *exp);
void ofd_fmd_cleanup(struct obd_export *exp);
#ifdef DO_FMD_DROP
void ofd_fmd_drop(struct obd_export *exp, struct lu_fid *fid);
#else
#define ofd_fmd_drop(exp, fid)
#endif

enum {
        LPROC_OFD_READ_BYTES = 0,
        LPROC_OFD_WRITE_BYTES = 1,
        LPROC_OFD_LAST,
};

#define OFD_MAX_CACHE_SIZE (8 * 1024 * 1024)

#ifdef LPROCFS
void ofd_tally(struct obd_export *exp, struct page **pages, int nr_pages,
                  unsigned long *blocks, int blocks_per_page, int wr);
int lproc_ofd_attach_seqstat(struct obd_device *dev);
void lprocfs_ofd_init_vars(struct lprocfs_static_vars *lvars);
#else
static inline void ofd_tally(struct obd_export *exp, struct page **pages,
                                int nr_pages, unsigned long *blocks,
                                int blocks_per_page, int wr) {}
static inline int lproc_ofd_attach_seqstat(struct obd_device *dev)
{
        return 0;
}
static inline void lprocfs_ofd_init_vars(struct lprocfs_static_vars *lvars)
{
        memset(lvars, 0, sizeof(*lvars));
}
#endif

/* Capability */

void blacklist_add(uid_t uid);
void blacklist_del(uid_t uid);
int blacklist_display(char *buf, int bufsize);

#define OFD_MAX_GROUPS       256

struct ofd_device {
        struct dt_device         ofd_dt_dev;
        struct dt_device        *ofd_osd;
        struct obd_export       *ofd_osd_exp;
        struct dt_device_param   ofd_dt_conf;
        /* DLM name-space for meta-data locks maintained by this server */
        struct ldlm_namespace   *ofd_namespace;
        /* ptlrpc handle for OST->client connections (for lock ASTs). */
        struct ptlrpc_client    *ofd_ldlm_client;

        /* transaction callbacks */
        struct dt_txn_callback   ofd_txn_cb;

        /* last_rcvd file */
        struct lu_target         ofd_lut;
        struct dt_object        *ofd_last_group_file;
        struct dt_object        *ofd_health_check_file;

        int                      ofd_subdir_count;

        /* XXX: make the following dynamic */
        int                      ofd_max_group;
        obd_id                   ofd_last_objids[OFD_MAX_GROUPS];
        cfs_mutex_t              ofd_create_locks[OFD_MAX_GROUPS];
        struct dt_object        *ofd_lastid_obj[OFD_MAX_GROUPS];
        cfs_spinlock_t           ofd_objid_lock;
        unsigned long            ofd_destroys_in_progress;

        /* statfs optimization: we cache a bit  */
        struct obd_statfs        ofd_osfs;
        __u64                    ofd_osfs_age;
        cfs_spinlock_t           ofd_osfs_lock;

        /* grants: all values in bytes */
        cfs_spinlock_t           ofd_grant_lock;
        obd_size                 ofd_tot_dirty;
        obd_size                 ofd_tot_granted;
        obd_size                 ofd_tot_pending;
        int                      ofd_tot_granted_clients;
        cfs_semaphore_t          ofd_grant_sem;

        /* ofd mod data: ofd_device wide values */
        int                      ofd_fmd_max_num; /* per ofd ofd_mod_data */
        cfs_duration_t           ofd_fmd_max_age; /* time to fmd expiry */

        cfs_spinlock_t           ofd_flags_lock;
        unsigned long            ofd_raid_degraded:1,
                                 ofd_syncjournal:1, /* sync journal on writes */
                                 ofd_sync_lock_cancel:2;/* sync on lock cancel */

        /* sptlrpc stuff */
        cfs_rwlock_t             ofd_sptlrpc_lock;
        struct sptlrpc_rule_set  ofd_sptlrpc_rset;

        /* capability related */
        unsigned int             ofd_fl_oss_capa;
        cfs_list_t               ofd_capa_keys;
        cfs_hlist_head_t        *ofd_capa_hash;
};

#define ofd_last_rcvd    ofd_lut.lut_last_rcvd
#define ofd_fsd          ofd_lut.lut_lsd
#define ofd_last_transno ofd_lut.lut_last_transno
#define ofd_transno_lock ofd_lut.lut_translock

static inline struct ofd_device *ofd_dev(struct lu_device *d)
{
        return container_of0(d, struct ofd_device, ofd_dt_dev.dd_lu_dev);
}

static inline struct obd_device *ofd_obd(struct ofd_device *ofd)
{
        return ofd->ofd_dt_dev.dd_lu_dev.ld_obd;
}

static inline struct ofd_device *ofd_exp(struct obd_export *exp)
{
        struct obd_device *obd = exp->exp_obd;
        return ofd_dev(obd->obd_lu_dev);
}

static inline char *ofd_name(struct ofd_device *ofd)
{
        return ofd->ofd_dt_dev.dd_lu_dev.ld_obd->obd_name;
}

struct ofd_object {
        struct lu_object_header ofo_header;
        struct dt_object        ofo_obj;
};

static inline struct ofd_object *ofd_obj(struct lu_object *o)
{
        return container_of0(o, struct ofd_object, ofo_obj.do_lu);
}

static inline int ofd_object_exists(struct ofd_object *obj)
{
        LASSERT(obj != NULL);
        if (lu_object_is_dying(obj->ofo_obj.do_lu.lo_header))
                return 0;
        return lu_object_exists(&obj->ofo_obj.do_lu);
}

static inline struct dt_object * fo2dt(struct ofd_object *obj)
{
        return &obj->ofo_obj;
}

static inline struct dt_object *ofd_object_child(struct ofd_object *_obj)
{
        struct lu_object *lu = &(_obj)->ofo_obj.do_lu;
        return container_of0(lu_object_next(lu), struct dt_object, do_lu);
}

static inline
struct ofd_device *ofd_obj2dev(const struct ofd_object *fo)
{
        return ofd_dev(fo->ofo_obj.do_lu.lo_dev);
}

static inline
struct lustre_capa *ofd_object_capa(const struct lu_env *env,
                                       const struct ofd_object *obj)
{
        /* TODO: see mdd_object_capa() */
        return BYPASS_CAPA;
}

static inline void ofd_read_lock(const struct lu_env *env,
                                     struct ofd_object *fo)
{
        struct dt_object  *next = ofd_object_child(fo);
        next->do_ops->do_read_lock(env, next, 0);
}

static inline void ofd_read_unlock(const struct lu_env *env,
                                       struct ofd_object *fo)
{
        struct dt_object  *next = ofd_object_child(fo);
        next->do_ops->do_read_unlock(env, next);
}

static inline void ofd_write_lock(const struct lu_env *env,
                                     struct ofd_object *fo)
{
        struct dt_object  *next = ofd_object_child(fo);
        next->do_ops->do_write_lock(env, next, 0);
}

static inline void ofd_write_unlock(const struct lu_env *env,
                                       struct ofd_object *fo)
{
        struct dt_object  *next = ofd_object_child(fo);
        next->do_ops->do_write_unlock(env, next);
}

/*
 * Common data shared by obdofd-level handlers. This is allocated per-thread
 * to reduce stack consumption.
 */
struct ofd_thread_info {
        const struct lu_env       *fti_env;

        /* request related data */
        struct obd_export         *fti_exp;
        __u64                      fti_xid;
        __u64                      fti_transno;
        __u64                      fti_pre_version;
        __u32                      fti_has_trans:1, /* has txn already? */
                                   fti_mult_trans:1;

        struct lu_fid              fti_fid;
        struct lu_attr             fti_attr;
        struct lu_attr             fti_attr2;
        struct ldlm_res_id         fti_resid;
        struct filter_fid          fti_mds_fid;
        struct ost_id              fti_ostid;
        struct ofd_object         *fti_obj;
        union {
                char               name[64]; /* for obdofd_init0()     */
                struct obd_statfs  osfs;     /* for obdofd_statfs()    */
        } fti_u;

        /* Ops object filename */
        struct lu_name             fti_name;

        struct dt_object_format    fti_dof;
        struct lu_buf              fti_buf;
        loff_t                     fti_off;
};

extern struct lu_context_key ofd_txn_thread_key;
extern struct lu_context_key ofd_thread_key;

static inline struct ofd_thread_info * ofd_info(const struct lu_env *env)
{
        struct ofd_thread_info *info;

        info = lu_context_key_get(&env->le_ctx, &ofd_thread_key);
        LASSERT(info);
        LASSERT(info->fti_env);
        LASSERT(info->fti_env == env);
        return info;
}

static inline
struct ofd_thread_info * ofd_info_init(const struct lu_env *env,
                                             struct obd_export *exp)
{
        struct ofd_thread_info *info;

        info = lu_context_key_get(&env->le_ctx, &ofd_thread_key);
        LASSERT(info);
        LASSERT(info->fti_exp == NULL);
        LASSERT(info->fti_env == NULL);
        LASSERT(info->fti_attr.la_valid == 0);

        info->fti_env = env;
        info->fti_exp = exp;
        info->fti_pre_version = 0;
        info->fti_transno = 0;
        info->fti_has_trans = 0;
        return info;
}

extern void target_recovery_fini(struct obd_device *obd);
extern void target_recovery_init(struct lu_target *lut,
                                 svc_handler_t handler);

static inline int ofd_export_stats_init(struct ofd_device *ofd,
                                           struct obd_export *exp, void *data)
{
        return 0;
}

/* ofd_capa.c */
int ofd_update_capa_key(struct ofd_device *, struct lustre_capa_key *);
int ofd_auth_capa(struct ofd_device *, struct lu_fid *, __u64,
                  struct lustre_capa *, __u64);
void ofd_free_capa_keys(struct ofd_device *ofd);

/* filter_obd.c */
int ofd_setattr(struct obd_export *exp,
                struct obd_info *oinfo, struct obd_trans_info *oti);
int ofd_destroy(struct obd_export *exp, struct obdo *oa,
                struct lov_stripe_md *md, struct obd_trans_info *oti,
                struct obd_export *md_exp, void *capa);

/* ofd_lvb.c */
extern struct ldlm_valblock_ops ofd_lvbo;


/* ofd_io.c */
int ofd_preprw(int cmd, struct obd_export *exp, struct obdo *oa, int objcount,
               struct obd_ioobj *obj, struct niobuf_remote *nb, int *nr_local,
               struct niobuf_local *res, struct obd_trans_info *oti,
               struct lustre_capa *capa);
int ofd_commitrw(int cmd, struct obd_export *exp, struct obdo *oa,
                 int objcount, struct obd_ioobj *obj, struct niobuf_remote *nb,
                 int npages, struct niobuf_local *res,
                 struct obd_trans_info *oti, int rc);
void flip_into_page_cache(struct inode *inode, struct page *new_page);

/* ofd_io_*.c */
struct ofd_iobuf;
struct ofd_iobuf *ofd_alloc_iobuf(struct filter_obd *, int rw,
                                        int num_pages);
void ofd_free_iobuf(struct ofd_iobuf *iobuf);
int ofd_iobuf_add_page(struct obd_device *obd, struct ofd_iobuf *iobuf,
                       struct inode *inode, struct page *page);
void *ofd_iobuf_get(struct filter_obd *ofd, struct obd_trans_info *oti);
void ofd_iobuf_put(struct filter_obd *ofd, struct ofd_iobuf *iobuf,
                   struct obd_trans_info *oti);
int ofd_direct_io(int rw, struct dentry *dchild, struct ofd_iobuf *iobuf,
                  struct obd_export *exp, struct iattr *attr,
                  struct obd_trans_info *oti, void **wait_handle);
int ofd_clear_truncated_page(struct inode *inode);

/* ofd_recovery.c */
struct thandle *ofd_trans_create0(const struct lu_env *env,
                                  struct ofd_device *ofd);
struct thandle *ofd_trans_create(const struct lu_env *env,
                                 struct ofd_device *ofd);
int ofd_trans_start(const struct lu_env *env,
                    struct ofd_device *ofd, struct thandle *th);
void ofd_trans_stop(const struct lu_env *env, struct ofd_device *ofd,
                    struct ofd_object *fo, struct thandle *th);
int ofd_client_free(struct lu_env *env, struct obd_export *exp);
int ofd_client_new(const struct lu_env *env, struct ofd_device *ofd,
                   struct obd_export *exp);
int ofd_client_add(const struct lu_env *env, struct ofd_device *ofd,
                   struct filter_export_data *fed, int cl_idx);
int ofd_fs_setup(const struct lu_env *env, struct ofd_device *ofd,
                 struct obd_device *obd);
void ofd_fs_cleanup(const struct lu_env *env, struct ofd_device *ofd);

/* ofd_fs.c */
obd_id ofd_last_id(struct ofd_device *ofd, obd_seq seq);
void ofd_last_id_set(struct ofd_device *ofd, obd_id id, obd_seq seq);
int ofd_last_id_write(const struct lu_env *env, struct ofd_device *ofd,
                      obd_seq seq);
int ofd_last_id_read(const struct lu_env *env, struct ofd_device *ofd,
                     obd_seq seq);
int ofd_groups_init(const struct lu_env *env, struct ofd_device *ofd);
int ofd_last_rcvd_header_write(const struct lu_env *env,
                               struct ofd_device *ofd,
                               struct thandle *th);
int ofd_last_rcvd_write(const struct lu_env *env,
                        struct ofd_device *ofd,
                        struct lsd_client_data *lcd,
                        loff_t *off, struct thandle *th);
int ofd_server_data_init(const struct lu_env *env,
                         struct ofd_device *ofd);
int ofd_group_load(const struct lu_env *env, struct ofd_device *ofd, int);
void ofd_group_fini(const struct lu_env *env, struct ofd_device *ofd, int);
int ofd_record_write(const struct lu_env *env, struct ofd_device *ofd,
                     struct dt_object *dt, struct lu_buf *buf, loff_t *off);

/* ofd_objects.c */
struct ofd_object *ofd_object_find(const struct lu_env *env,
                                   struct ofd_device *ofd,
                                   const struct lu_fid *fid);
struct
ofd_object *ofd_object_find_or_create(const struct lu_env *env,
                                      struct ofd_device *ofd,
                                      const struct lu_fid *fid,
                                      struct lu_attr *attr);
int ofd_precreate_object(const struct lu_env *env, struct ofd_device *ofd,
                         obd_id id, obd_seq seq);

void ofd_object_put(const struct lu_env *env, struct ofd_object *fo);
int ofd_attr_set(const struct lu_env *env, struct ofd_object *fo,
                 const struct lu_attr *la);
int ofd_object_punch(const struct lu_env *env, struct ofd_object *fo,
                     __u64 start, __u64 end, const struct lu_attr *la);
int ofd_object_destroy(const struct lu_env *, struct ofd_object *, int);
int ofd_attr_get(const struct lu_env *env, struct ofd_object *fo,
                 struct lu_attr *la);

/* ofd_grants.c */
void ofd_grant_discard(struct obd_export *exp);
void ofd_grant_sanity_check(struct obd_device *obd, const char *func);
void ofd_grant_incoming(const struct lu_env *env, struct obd_export *exp,
                        struct obdo *oa);
obd_size ofd_grant_space_left(const struct lu_env *env,
                              struct obd_export *exp);
int ofd_grant_client_calc(struct obd_export *exp, obd_size *left,
                          unsigned long *used, unsigned long *ungranted);
int ofd_grant_check(const struct lu_env *env, struct obd_export *exp, 
                    struct obdo *oa, struct niobuf_local *lnb, int nrpages,
                    obd_size *left, unsigned long *used);
long ofd_grant(const struct lu_env *env, struct obd_export *exp,
               obd_size current_grant, obd_size want,
               obd_size fs_space_left);
void ofd_grant_commit(struct obd_export *exp, int niocount,
                      struct niobuf_local *res);
/* ofd_obd.c */
int ofd_create(struct obd_export *exp, struct obdo *oa,
               struct lov_stripe_md **ea, struct obd_trans_info *oti);

/* The same as osc_build_res_name() */
static inline void ofd_build_resid(const struct lu_fid *fid,
                                   struct ldlm_res_id *resname)
{
        if (fid_is_idif(fid)) {
                /* get id/seq like ostid_idif_pack() does */
                osc_build_res_name(fid_idif_id(fid_seq(fid), fid_oid(fid),
                                               fid_ver(fid)),
                                   FID_SEQ_OST_MDT0, resname);
        } else {
                /* In the future, where OSTs have FID sequences allocated. */
                fid_build_reg_res_name(fid, resname);
        }
}

static inline void ofd_fid_from_resid(struct lu_fid *fid,
                                      const struct ldlm_res_id *name)
{
        /* if seq is FID_SEQ_OST_MDT0 then we have IDIF and resid was built
         * using osc_build_res_name function. */
        if (fid_seq_is_mdt0(name->name[LUSTRE_RES_ID_VER_OID_OFF])) {
                struct ost_id ostid;
                ostid.oi_id = name->name[LUSTRE_RES_ID_SEQ_OFF];
                ostid.oi_seq = name->name[LUSTRE_RES_ID_VER_OID_OFF];
                fid_ostid_unpack(fid, &ostid, 0);
        } else {
                fid->f_seq = name->name[LUSTRE_RES_ID_SEQ_OFF];
                fid->f_oid = (__u32)name->name[LUSTRE_RES_ID_VER_OID_OFF];
                fid->f_ver = name->name[LUSTRE_RES_ID_VER_OID_OFF] >> 32;
        }
}

static inline void ofd_oti2info(struct ofd_thread_info *info,
                                struct obd_trans_info *oti)
{
        info->fti_xid = oti->oti_xid;
        info->fti_transno = oti->oti_transno;
        info->fti_pre_version = oti->oti_pre_version;
}

static inline void ofd_info2oti(struct ofd_thread_info *info,
                                struct obd_trans_info *oti)
{
        oti->oti_xid = info->fti_xid;
        oti->oti_transno = info->fti_transno;
        oti->oti_pre_version = info->fti_pre_version;
}

/* sync on lock cancel is useless when we force a journal flush,
 * and if we enable async journal commit, we should also turn on
 * sync on lock cancel if it is not enabled already. */
static inline void ofd_slc_set(struct ofd_device *ofd)
{
        if (ofd->ofd_syncjournal == 1)
                ofd->ofd_sync_lock_cancel = NEVER_SYNC_ON_CANCEL;
        else if (ofd->ofd_sync_lock_cancel == NEVER_SYNC_ON_CANCEL)
                ofd->ofd_sync_lock_cancel = ALWAYS_SYNC_ON_CANCEL;
}

#endif /* _OFD_INTERNAL_H */
