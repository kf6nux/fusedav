/* $Id$ */

/***
  This file is part of fusedav.

  fusedav is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  fusedav is distributed in the hope that it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
  or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
  License for more details.

  You should have received a copy of the GNU General Public License
  along with fusedav; if not, write to the Free Software Foundation,
  Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <assert.h>
#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/statfs.h>
#include <getopt.h>
#include <attr/xattr.h>

#include <ne_request.h>
#include <ne_basic.h>
#include <ne_props.h>
#include <ne_utils.h>
#include <ne_socket.h>
#include <ne_auth.h>
#include <ne_dates.h>
#include <ne_redirect.h>
#include <ne_uri.h>

#include <yaml.h>

#include <fuse.h>

#include <systemd/sd-journal.h>

#include "statcache.h"
#include "filecache.h"
#include "session.h"
#include "fusedav.h"

const ne_propname query_properties[] = {
    { "DAV:", "resourcetype" },
    { "http://apache.org/dav/props/", "executable" },
    { "DAV:", "getcontentlength" },
    { "DAV:", "getlastmodified" },
    { "DAV:", "creationdate" },
    { NULL, NULL }
};

mode_t mask = 0;
int debug = 0;
struct fuse* fuse = NULL;
ne_lock_store *lock_store = NULL;
struct ne_lock *lock = NULL;
int lock_thread_exit = 0;

#define MIME_XATTR "user.mime_type"

#define MAX_REDIRECTS 10

struct fill_info {
    void *buf;
    fuse_fill_dir_t filler;
    const char *root;
};

struct fusedav_config {
    char *uri;
    char *username;
    char *password;
    char *ca_certificate;
    char *client_certificate;
    char *client_certificate_password;
    int  lock_timeout;
    bool lock_on_mount;
    bool debug;
    bool nodaemon;
    bool noattributes;
};

enum {
     KEY_HELP,
     KEY_VERSION,
};

#define FUSEDAV_OPT(t, p, v) { t, offsetof(struct fusedav_config, p), v }

static struct fuse_opt fusedav_opts[] = {
     FUSEDAV_OPT("username=%s",                    username, 0),
     FUSEDAV_OPT("password=%s",                    password, 0),
     FUSEDAV_OPT("ca_certificate=%s",              ca_certificate, 0),
     FUSEDAV_OPT("client_certificate=%s",          client_certificate, 0),
     FUSEDAV_OPT("client_certificate_password=%s", client_certificate_password, 0),
     FUSEDAV_OPT("lock_on_mount",                  lock_on_mount, true),
     FUSEDAV_OPT("lock_timeout=%i",                lock_timeout, 60),
     FUSEDAV_OPT("debug",                          debug, true),
     FUSEDAV_OPT("nodaemon",                       nodaemon, true),
     FUSEDAV_OPT("noattributes",                   noattributes, true),

     FUSE_OPT_KEY("-V",             KEY_VERSION),
     FUSE_OPT_KEY("--version",      KEY_VERSION),
     FUSE_OPT_KEY("-h",             KEY_HELP),
     FUSE_OPT_KEY("--help",         KEY_HELP),
     FUSE_OPT_KEY("-?",             KEY_HELP),
     FUSE_OPT_END
};

static int get_stat(const char *path, struct stat *stbuf);
int file_exists_or_set_null(char **path);

static pthread_once_t path_cvt_once = PTHREAD_ONCE_INIT;
static pthread_key_t path_cvt_tsd_key;

static void path_cvt_tsd_key_init(void) {
    pthread_key_create(&path_cvt_tsd_key, free);
}

static const char *path_cvt(const char *path) {
    char *r, *t;
    int l;

    pthread_once(&path_cvt_once, path_cvt_tsd_key_init);

    if ((r = pthread_getspecific(path_cvt_tsd_key)))
        free(r);

    t = malloc((l = strlen(base_directory)+strlen(path))+1);
    assert(t);
    sprintf(t, "%s%s", base_directory, path);

    if (l > 1 && t[l-1] == '/')
        t[l-1] = 0;

    r = ne_path_escape(t);
    free(t);

    pthread_setspecific(path_cvt_tsd_key, r);

    return r;
}

static int simple_propfind_with_redirect(
        ne_session *session,
        const char *path,
        int depth,
        const ne_propname *props,
        ne_props_result results,
        void *userdata) {

    int i, ret;

    for (i = 0; i < MAX_REDIRECTS; i++) {
        const ne_uri *u;

        if ((ret = ne_simple_propfind(session, path, depth, props, results, userdata)) != NE_REDIRECT)
            return ret;

        if (!(u = ne_redirect_location(session)))
            break;

        if (!session_is_local(u))
            break;

        if (debug)
            sd_journal_print(LOG_DEBUG, "REDIRECT FROM '%s' to '%s'", path, u->path);

        path = u->path;
    }

    return ret;
}

static int proppatch_with_redirect(
        ne_session *session,
        const char *path,
        const ne_proppatch_operation *ops) {

    int i, ret;

    for (i = 0; i < MAX_REDIRECTS; i++) {
        const ne_uri *u;

        if ((ret = ne_proppatch(session, path, ops)) != NE_REDIRECT)
            return ret;

        if (!(u = ne_redirect_location(session)))
            break;

        if (!session_is_local(u))
            break;

        if (debug)
            sd_journal_print(LOG_DEBUG, "REDIRECT FROM '%s' to '%s'", path, u->path);

        path = u->path;
    }

    return ret;
}


static void fill_stat(struct stat* st, const ne_prop_result_set *results, int is_dir) {
    const char *e, *gcl, *glm, *cd;
    //const char *rt;
    //const ne_propname resourcetype = { "DAV:", "resourcetype" };
    const ne_propname executable = { "http://apache.org/dav/props/", "executable" };
    const ne_propname getcontentlength = { "DAV:", "getcontentlength" };
    const ne_propname getlastmodified = { "DAV:", "getlastmodified" };
    const ne_propname creationdate = { "DAV:", "creationdate" };

    assert(st && results);

    //rt = ne_propset_value(results, &resourcetype);
    e = ne_propset_value(results, &executable);
    gcl = ne_propset_value(results, &getcontentlength);
    glm = ne_propset_value(results, &getlastmodified);
    cd = ne_propset_value(results, &creationdate);

    memset(st, 0, sizeof(struct stat));

    if (is_dir) {
        st->st_mode = S_IFDIR | 0777;
        st->st_nlink = 3;            /* find will ignore this directory if nlin <= and st_size == 0 */
        st->st_size = 4096;
    } else {
        st->st_mode = S_IFREG | (e && (*e == 'T' || *e == 't') ? 0777 : 0666);
        st->st_nlink = 1;
        st->st_size = gcl ? atoll(gcl) : 0;
    }

    st->st_atime = time(NULL);
    st->st_mtime = glm ? ne_rfc1123_parse(glm) : 0;
    st->st_ctime = cd ? ne_iso8601_parse(cd) : 0;

    st->st_blocks = (st->st_size+511)/512;
    /*sd_journal_print(LOG_DEBUG, "a: %u; m: %u; c: %u", st->st_atime, st->st_mtime, st->st_ctime);*/

    st->st_mode &= ~mask;

    st->st_uid = getuid();
    st->st_gid = getgid();
}

static char *strip_trailing_slash(char *fn, int *is_dir) {
    size_t l = strlen(fn);
    assert(fn);
    assert(is_dir);
    assert(l > 0);

    if ((*is_dir = (fn[l-1] == '/')))
        fn[l-1] = 0;

    return fn;
}

static void getdir_propfind_callback(void *userdata, const ne_uri *u, const ne_prop_result_set *results) {
    struct fill_info *f = userdata;
    struct stat st;
    char fn[PATH_MAX], *t;
    int is_dir = 0;

    assert(f);

    strncpy(fn, u->path, sizeof(fn));
    fn[sizeof(fn)-1] = 0;
    strip_trailing_slash(fn, &is_dir);

    if (strcmp(fn, f->root) && fn[0]) {
        char *h;

        if ((t = strrchr(fn, '/')))
            t++;
        else
            t = fn;

        dir_cache_add(f->root, t);
        f->filler(f->buf, h = ne_path_unescape(t), NULL, 0);
        free(h);
    }

    fill_stat(&st, results, is_dir);
    stat_cache_set(fn, &st);
}

static void getdir_cache_callback(
        const char *root,
        const char *fn,
        void *user) {

    struct fill_info *f = user;
    char path[PATH_MAX];
    char *h;

    assert(f);

    snprintf(path, sizeof(path), "%s/%s", !strcmp(root, "/") ? "" : root, fn);

    f->filler(f->buf, h = ne_path_unescape(fn), NULL, 0);
    free(h);
}

static int dav_readdir(
        const char *path,
        void *buf,
        fuse_fill_dir_t filler,
        __unused ne_off_t offset,
        __unused struct fuse_file_info *fi) {

    struct fill_info f;
    ne_session *session;

    path = path_cvt(path);

    if (debug)
        sd_journal_print(LOG_DEBUG, "getdir(%s)", path);

    f.buf = buf;
    f.filler = filler;
    f.root = path;

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    if (dir_cache_enumerate(path, getdir_cache_callback, &f) < 0) {

        if (debug)
            sd_journal_print(LOG_DEBUG, "DIR-CACHE-MISS");

        if (!(session = session_get(1)))
            return -EIO;

        dir_cache_begin(path);

        if (simple_propfind_with_redirect(session, path, NE_DEPTH_ONE, query_properties, getdir_propfind_callback, &f) != NE_OK) {
            dir_cache_finish(path, 2);
            sd_journal_print(LOG_ERR, "PROPFIND failed: %s", ne_get_error(session));
            return -ENOENT;
        }

        dir_cache_finish(path, 1);
    }

    return 0;
}

static void getattr_propfind_callback(void *userdata, const ne_uri *u, const ne_prop_result_set *results) {
    struct stat *st = (struct stat*) userdata;
    char fn[PATH_MAX];
    int is_dir;

    assert(st);

    strncpy(fn, u->path, sizeof(fn));
    fn[sizeof(fn)-1] = 0;
    strip_trailing_slash(fn, &is_dir);

    fill_stat(st, results, is_dir);
    stat_cache_set(fn, st);
}

static int print_stat(struct stat *stbuf) {
    if (debug) {
        sd_journal_print(LOG_DEBUG, "stat.st_mode=%o", stbuf->st_mode);
        sd_journal_print(LOG_DEBUG, "stat.st_nlink=%ld", stbuf->st_nlink);
        sd_journal_print(LOG_DEBUG, "stat.st_uid=%d", stbuf->st_uid);
        sd_journal_print(LOG_DEBUG, "stat.st_gid=%d", stbuf->st_gid);
        sd_journal_print(LOG_DEBUG, "stat.st_size=%ld", stbuf->st_size);
        sd_journal_print(LOG_DEBUG, "stat.st_blksize=%ld", stbuf->st_blksize);
        sd_journal_print(LOG_DEBUG, "stat.st_blocks=%ld", stbuf->st_blocks);
        sd_journal_print(LOG_DEBUG, "stat.st_atime=%ld", stbuf->st_atime);
        sd_journal_print(LOG_DEBUG, "stat.st_mtime=%ld", stbuf->st_mtime);
        sd_journal_print(LOG_DEBUG, "stat.st_ctime=%ld", stbuf->st_ctime);
    }
    return 0;
}

static int get_stat(const char *path, struct stat *stbuf) {
    ne_session *session;

    if (!(session = session_get(1)))
        return -EIO;

    if (stat_cache_get(path, stbuf) == 0) {
        print_stat(stbuf);
        return stbuf->st_mode == 0 ? -ENOENT : 0;
    } else {
        if (debug)
            sd_journal_print(LOG_DEBUG, "STAT-CACHE-MISS");

        if (simple_propfind_with_redirect(session, path, NE_DEPTH_ZERO, query_properties, getattr_propfind_callback, stbuf) != NE_OK) {
            stat_cache_invalidate(path);
            sd_journal_print(LOG_NOTICE, "PROPFIND failed: %s", ne_get_error(session));
            return -ENOENT;
        }
        print_stat(stbuf);

        return 0;
    }
}

static int dav_getattr(const char *path, struct stat *stbuf) {
    path = path_cvt(path);
    if (debug)
        sd_journal_print(LOG_DEBUG, "getattr(%s)", path);
    return get_stat(path, stbuf);
}

static int dav_unlink(const char *path) {
    int r;
    struct stat st;
    ne_session *session;

    path = path_cvt(path);

    if (debug)
        sd_journal_print(LOG_DEBUG, "unlink(%s)", path);

    if (!(session = session_get(1)))
        return -EIO;

    if ((r = get_stat(path, &st)) < 0)
        return r;

    if (!S_ISREG(st.st_mode))
        return -EISDIR;

    if (ne_delete(session, path)) {
        sd_journal_print(LOG_ERR, "DELETE failed: %s", ne_get_error(session));
        return -ENOENT;
    }

    stat_cache_invalidate(path);
    dir_cache_invalidate_parent(path);

    return 0;
}

static int dav_rmdir(const char *path) {
    char fn[PATH_MAX];
    int r;
    struct stat st;
    ne_session *session;

    path = path_cvt(path);

    if (debug)
        sd_journal_print(LOG_DEBUG, "rmdir(%s)", path);

    if (!(session = session_get(1)))
        return -EIO;

    if ((r = get_stat(path, &st)) < 0)
        return r;

    if (!S_ISDIR(st.st_mode))
        return -ENOTDIR;

    snprintf(fn, sizeof(fn), "%s/", path);

    if (ne_delete(session, fn)) {
        sd_journal_print(LOG_ERR, "DELETE failed: %s", ne_get_error(session));
        return -ENOENT;
    }

    stat_cache_invalidate(path);
    dir_cache_invalidate_parent(path);

    return 0;
}

static int dav_mkdir(const char *path, __unused mode_t mode) {
    char fn[PATH_MAX];
    ne_session *session;

    path = path_cvt(path);

    if (debug)
        sd_journal_print(LOG_DEBUG, "mkdir(%s)", path);

    if (!(session = session_get(1)))
        return -EIO;

    snprintf(fn, sizeof(fn), "%s/", path);

    if (ne_mkcol(session, fn)) {
        sd_journal_print(LOG_ERR, "MKCOL failed: %s", ne_get_error(session));
        return -ENOENT;
    }

    stat_cache_invalidate(path);
    dir_cache_invalidate_parent(path);

    return 0;
}

static int dav_rename(const char *from, const char *to) {
    ne_session *session;
    int r = 0;
    struct stat st;
    char fn[PATH_MAX], *_from;

    from = _from = strdup(path_cvt(from));
    assert(from);
    to = path_cvt(to);

    if (debug)
        sd_journal_print(LOG_DEBUG, "rename(%s, %s)", from, to);

    if (!(session = session_get(1))) {
        r = -EIO;
        goto finish;
    }

    if ((r = get_stat(from, &st)) < 0)
        goto finish;

    if (S_ISDIR(st.st_mode)) {
        snprintf(fn, sizeof(fn), "%s/", from);
        from = fn;
    }

    if (ne_move(session, 1, from, to)) {
        sd_journal_print(LOG_ERR, "MOVE failed: %s", ne_get_error(session));
        r = -ENOENT;
        goto finish;
    }

    stat_cache_invalidate(from);
    stat_cache_invalidate(to);

    dir_cache_invalidate_parent(from);
    dir_cache_invalidate_parent(to);

finish:

    free(_from);

    return r;
}

static int dav_release(const char *path, __unused struct fuse_file_info *info) {
    void *f = NULL;
    int r = 0;
    ne_session *session;

    path = path_cvt(path);

    if (debug)
        sd_journal_print(LOG_DEBUG, "release(%s)", path);

    if (!(session = session_get(1))) {
        r = -EIO;
        goto finish;
    }

    if (!(f = file_cache_get(path))) {
        sd_journal_print(LOG_DEBUG, "release() called for closed file");
        r = -EFAULT;
        goto finish;
    }

    if (file_cache_close(f) < 0) {
        r = -errno;
        goto finish;
    }

finish:
    if (f)
        file_cache_unref(f);

    return r;
}

static int dav_fsync(const char *path, __unused int isdatasync, __unused struct fuse_file_info *info) {
    void *f = NULL;
    int r = 0;
    ne_session *session;

    path = path_cvt(path);
    if (debug)
        sd_journal_print(LOG_DEBUG, "fsync(%s)", path);

    if (!(session = session_get(1))) {
        r = -EIO;
        goto finish;
    }

    if (!(f = file_cache_get(path))) {
        sd_journal_print(LOG_DEBUG, "fsync() called for closed file");
        r = -EFAULT;
        goto finish;
    }

    if (file_cache_sync(f) < 0) {
        r = -errno;
        goto finish;
    }

finish:

    if (f)
        file_cache_unref(f);

    return r;
}

static int dav_mknod(const char *path, mode_t mode, __unused dev_t rdev) {
    char tempfile[PATH_MAX];
    int fd;
    ne_session *session;
    struct stat st;

    path = path_cvt(path);
    if (debug)
        sd_journal_print(LOG_DEBUG, "mknod(%s)", path);

    if (!(session = session_get(1)))
        return -EIO;

    if (!S_ISREG(mode))
        return -ENOTSUP;

    snprintf(tempfile, sizeof(tempfile), "%s/fusedav-empty-XXXXXX", "/tmp");
    if ((fd = mkstemp(tempfile)) < 0)
        return -errno;

    unlink(tempfile);

    if (ne_put(session, path, fd)) {
        sd_journal_print(LOG_ERR, "mknod:PUT failed: %s", ne_get_error(session));
        close(fd);
        return -EACCES;
    }

    close(fd);

    // Prepopulate stat cache.
    st.st_mode = 040775;  // @TODO: Set to a configurable default.
    st.st_nlink = 3;
    st.st_size = 0;
    st.st_atime = time(NULL);
    st.st_mtime = st.st_atime;
    st.st_ctime = st.st_mtime;
    st.st_blksize = 0;
    st.st_blocks = 8;
    st.st_uid = getuid();
    st.st_gid = getgid();
    stat_cache_set(path, &st);

    //stat_cache_invalidate(path);
    dir_cache_invalidate_parent(path); // @TODO: Prepopulate this, too.

    return 0;
}

static int dav_open(const char *path, struct fuse_file_info *info) {
    void *f;

    if (debug)
        sd_journal_print(LOG_DEBUG, "open(%s)", path);

    path = path_cvt(path);

    if (!(f = file_cache_open(path, info->flags)))
        return -errno;

    file_cache_unref(f);

    return 0;
}

static int dav_read(const char *path, char *buf, size_t size, ne_off_t offset, __unused struct fuse_file_info *info) {
    void *f = NULL;
    ssize_t r;

    path = path_cvt(path);

    if (debug)
        sd_journal_print(LOG_DEBUG, "read(%s, %lu+%lu)", path, (unsigned long) offset, (unsigned long) size);

    if (!(f = file_cache_get(path))) {
        sd_journal_print(LOG_WARNING, "read() called for closed file");
        r = -EFAULT;
        goto finish;
    }

    if ((r = file_cache_read(f, buf, size, offset)) < 0) {
        r = -errno;
        goto finish;
    }

finish:
    if (f)
        file_cache_unref(f);

    return r;
}

static int dav_write(const char *path, const char *buf, size_t size, ne_off_t offset, __unused struct fuse_file_info *info) {
    void *f = NULL;
    ssize_t r;

    path = path_cvt(path);

    if (debug)
        sd_journal_print(LOG_DEBUG, "write(%s, %lu+%lu)", path, (unsigned long) offset, (unsigned long) size);

    if (!(f = file_cache_get(path))) {
        sd_journal_print(LOG_WARNING, "write() called for closed file");
        r = -EFAULT;
        goto finish;
    }

    if ((r = file_cache_write(f, buf, size, offset)) < 0) {
        r = -errno;
        goto finish;
    }

finish:
    if (f)
        file_cache_unref(f);

    return r;
}


static int dav_truncate(const char *path, ne_off_t size) {
    void *f = NULL;
    int r = 0;
    ne_session *session;

    path = path_cvt(path);

    if (debug)
        sd_journal_print(LOG_DEBUG, "truncate(%s, %lu)", path, (unsigned long) size);

    if (!(session = session_get(1)))
        r = -EIO;
        goto finish;

    if (!(f = file_cache_get(path))) {
        sd_journal_print(LOG_WARNING, "truncate() called for closed file");
        r = -EFAULT;
        goto finish;
    }

    if (file_cache_truncate(f, size) < 0) {
        r = -errno;
        goto finish;
    }

finish:
    if (f)
        file_cache_unref(f);

    return r;
}

static int dav_utimens(const char *path, const struct timespec tv[2]) {
    ne_session *session;
    const ne_propname getlastmodified = { "DAV:", "getlastmodified" };
    ne_proppatch_operation ops[2];
    int r = 0;
    char *date;
    struct fusedav_config *config = fuse_get_context()->private_data;

    if (config->noattributes) {
        if (debug)
            sd_journal_print(LOG_DEBUG, "Skipping attribute setting.");
        return r;
    }

    assert(path);

    path = path_cvt(path);

    if (debug)
        sd_journal_print(LOG_DEBUG, "utimens(%s, %lu, %lu)", path, tv[0].tv_sec, tv[1].tv_sec);

    ops[0].name = &getlastmodified;
    ops[0].type = ne_propset;
    ops[0].value = date = ne_rfc1123_date(tv[1].tv_sec);
    ops[1].name = NULL;

    if (!(session = session_get(1))) {
        r = -EIO;
        goto finish;
    }

    if (proppatch_with_redirect(session, path, ops)) {
        sd_journal_print(LOG_ERR, "PROPPATCH failed: %s", ne_get_error(session));
        r = -ENOTSUP;
        goto finish;
    }

    stat_cache_invalidate(path);  // @TODO: Update the stat cache instead.

finish:
    free(date);

    return r;
}

static const char *fix_xattr(const char *name) {
    assert(name);

    if (!strcmp(name, MIME_XATTR))
        return "user.webdav(DAV:;getcontenttype)";

    return name;
}

struct listxattr_info {
    char *list;
    size_t space, size;
};

static int listxattr_iterator(
        void *userdata,
        const ne_propname *pname,
        const char *value,
        __unused const ne_status *status) {

    struct listxattr_info *l = userdata;
    int n;

    assert(l);

    if (!value || !pname)
        return -1;

    if (l->list) {
        n = snprintf(l->list, l->space, "user.webdav(%s;%s)", pname->nspace, pname->name) + 1;

        if (n >= (int) l->space) {
            l->size += l->space;
            l->space = 0;
            return 1;

        } else {
            l->size += n;
            l->space -= n;

            if (l->list)
                l->list += n;

            return 0;
        }
    } else {
        /* Calculate space */

        l->size += strlen(pname->nspace) + strlen(pname->name) + 15;
        return 0;
    }
}

static void listxattr_propfind_callback(void *userdata, __unused const ne_uri *u, const ne_prop_result_set *results) {
    struct listxattr_info *l = userdata;
    ne_propset_iterate(results, listxattr_iterator, l);
}

static int dav_listxattr(
        const char *path,
        char *list,
        size_t size) {

    ne_session *session;
    struct listxattr_info l;


    assert(path);

    path = path_cvt(path);

    if (debug)
        sd_journal_print(LOG_DEBUG, "listxattr(%s, .., %lu)", path, (unsigned long) size);

    if (list) {
        l.list = list;
        l.space = size-1;
        l.size = 0;

        if (l.space >= sizeof(MIME_XATTR)) {
            memcpy(l.list, MIME_XATTR, sizeof(MIME_XATTR));
            l.list += sizeof(MIME_XATTR);
            l.space -= sizeof(MIME_XATTR);
            l.size += sizeof(MIME_XATTR);
        }

    } else {
        l.list = NULL;
        l.space = 0;
        l.size = sizeof(MIME_XATTR);
    }

    if (!(session = session_get(1)))
        return -EIO;

    if (simple_propfind_with_redirect(session, path, NE_DEPTH_ZERO, NULL, listxattr_propfind_callback, &l) != NE_OK) {
        sd_journal_print(LOG_ERR, "PROPFIND failed: %s", ne_get_error(session));
        return -EIO;
    }

    if (l.list) {
        assert(l.space > 0);
        *l.list = 0;
    }

    return l.size+1;
}

struct getxattr_info {
    ne_propname propname;
    char *value;
    size_t space, size;
};

static int getxattr_iterator(
        void *userdata,
        const ne_propname *pname,
        const char *value,
        __unused const ne_status *status) {

    struct getxattr_info *g = userdata;

    assert(g);

    if (!value || !pname)
        return -1;

    if (strcmp(pname->nspace, g->propname.nspace) ||
        strcmp(pname->name, g->propname.name))
        return 0;

    if (g->value) {
        size_t l;

        l = strlen(value);

        if (l > g->space)
            l = g->space;

        memcpy(g->value, value, l);
        g->size = l;
    } else {
        /* Calculate space */

        g->size = strlen(value);
        return 0;
    }

    return 0;
}

static void getxattr_propfind_callback(void *userdata, __unused const ne_uri *u, const ne_prop_result_set *results) {
    struct getxattr_info *g = userdata;
    ne_propset_iterate(results, getxattr_iterator, g);
}

static int parse_xattr(const char *name, char *dnspace, size_t dnspace_length, char *dname, size_t dname_length) {
    char *e;
    size_t k;

    assert(name);
    assert(dnspace);
    assert(dnspace_length);
    assert(dname);
    assert(dname_length);

    if (strncmp(name, "user.webdav(", 12) ||
        name[strlen(name)-1] != ')' ||
        !(e = strchr(name+12, ';')))
        return -1;

    if ((k = strcspn(name+12, ";")) > dnspace_length-1)
        return -1;

    memcpy(dnspace, name+12, k);
    dnspace[k] = 0;

    e++;

    if ((k = strlen(e)) > dname_length-1)
        return -1;

    assert(k > 0);
    k--;

    memcpy(dname, e, k);
    dname[k] = 0;

    return 0;
}

static int dav_getxattr(
        const char *path,
        const char *name,
        char *value,
        size_t size) {

    ne_session *session;
    struct getxattr_info g;
    ne_propname props[2];
    char dnspace[128], dname[128];

    assert(path);

    path = path_cvt(path);
    name = fix_xattr(name);

    if (debug)
        sd_journal_print(LOG_DEBUG, "getxattr(%s, %s, .., %lu)", path, name, (unsigned long) size);

    if (parse_xattr(name, dnspace, sizeof(dnspace), dname, sizeof(dname)) < 0)
        return -ENOATTR;

    props[0].nspace = dnspace;
    props[0].name = dname;
    props[1].nspace = NULL;
    props[1].name = NULL;

    if (value) {
        g.value = value;
        g.space = size;
        g.size = (size_t) -1;
    } else {
        g.value = NULL;
        g.space = 0;
        g.size = (size_t) -1;
    }

    g.propname = props[0];

    if (!(session = session_get(1)))
        return -EIO;

    if (simple_propfind_with_redirect(session, path, NE_DEPTH_ZERO, props, getxattr_propfind_callback, &g) != NE_OK) {
        sd_journal_print(LOG_ERR, "PROPFIND failed: %s", ne_get_error(session));
        return -EIO;
    }

    if (g.size == (size_t) -1)
        return -ENOATTR;

    return g.size;
}

static int dav_setxattr(
        const char *path,
        const char *name,
        const char *value,
        size_t size,
        int flags) {

    ne_session *session;
    ne_propname propname;
    ne_proppatch_operation ops[2];
    int r = 0;
    char dnspace[128], dname[128];
    char *value_fixed = NULL;

    assert(path);
    assert(name);
    assert(value);

    path = path_cvt(path);
    name = fix_xattr(name);

    if (debug)
        sd_journal_print(LOG_DEBUG, "setxattr(%s, %s)", path, name);

    if (flags) {
        r = ENOTSUP;
        goto finish;
    }

    if (parse_xattr(name, dnspace, sizeof(dnspace), dname, sizeof(dname)) < 0) {
        r = -ENOATTR;
        goto finish;
    }

    propname.nspace = dnspace;
    propname.name = dname;

    /* Add trailing NUL byte if required */
    if (!memchr(value, 0, size)) {
        value_fixed = malloc(size+1);
        assert(value_fixed);

        memcpy(value_fixed, value, size);
        value_fixed[size] = 0;

        value = value_fixed;
    }

    ops[0].name = &propname;
    ops[0].type = ne_propset;
    ops[0].value = value;

    ops[1].name = NULL;

    if (!(session = session_get(1))) {
        r = -EIO;
        goto finish;
    }

    if (proppatch_with_redirect(session, path, ops)) {
        sd_journal_print(LOG_ERR, "PROPPATCH failed: %s", ne_get_error(session));
        r = -ENOTSUP;
        goto finish;
    }

    stat_cache_invalidate(path);

finish:
    free(value_fixed);

    return r;
}

static int dav_removexattr(const char *path, const char *name) {
    ne_session *session;
    ne_propname propname;
    ne_proppatch_operation ops[2];
    int r = 0;
    char dnspace[128], dname[128];

    assert(path);
    assert(name);

    path = path_cvt(path);
    name = fix_xattr(name);

    if (debug)
        sd_journal_print(LOG_DEBUG, "removexattr(%s, %s)", path, name);

    if (parse_xattr(name, dnspace, sizeof(dnspace), dname, sizeof(dname)) < 0) {
        r = -ENOATTR;
        goto finish;
    }

    propname.nspace = dnspace;
    propname.name = dname;

    ops[0].name = &propname;
    ops[0].type = ne_propremove;
    ops[0].value = NULL;

    ops[1].name = NULL;

    if (!(session = session_get(1))) {
        r = -EIO;
        goto finish;
    }

    if (proppatch_with_redirect(session, path, ops)) {
        sd_journal_print(LOG_ERR, "PROPPATCH failed: %s", ne_get_error(session));
        r = -ENOTSUP;
        goto finish;
    }

    stat_cache_invalidate(path);

finish:

    return r;
}

static int dav_chmod(const char *path, mode_t mode) {
    ne_session *session;
    const ne_propname executable = { "http://apache.org/dav/props/", "executable" };
    ne_proppatch_operation ops[2];
    int r = 0;

    assert(path);

    path = path_cvt(path);

    if (debug)
        sd_journal_print(LOG_DEBUG, "chmod(%s, %04o)", path, mode);

    ops[0].name = &executable;
    ops[0].type = ne_propset;
    ops[0].value = mode & 0111 ? "T" : "F";
    ops[1].name = NULL;

    if (!(session = session_get(1))) {
        r = -EIO;
        goto finish;
    }

    if (proppatch_with_redirect(session, path, ops)) {
        sd_journal_print(LOG_ERR, "PROPPATCH failed: %s", ne_get_error(session));
        r = -ENOTSUP;
        goto finish;
    }

    stat_cache_invalidate(path);

finish:

    return r;
}

static struct fuse_operations dav_oper = {
    .getattr     = dav_getattr,
    .readdir     = dav_readdir,
    .mknod       = dav_mknod,
    .mkdir       = dav_mkdir,
    .unlink      = dav_unlink,
    .rmdir       = dav_rmdir,
    .rename      = dav_rename,
    .chmod       = dav_chmod,
    .truncate    = dav_truncate,
    .utimens     = dav_utimens,
    .open        = dav_open,
    .read        = dav_read,
    .write       = dav_write,
    .release     = dav_release,
    .fsync       = dav_fsync,
    .setxattr    = dav_setxattr,
    .getxattr    = dav_getxattr,
    .listxattr   = dav_listxattr,
    .removexattr = dav_removexattr,
};

static void exit_handler(__unused int sig) {
    static const char m[] = "*** Caught signal ***\n";
    if(fuse != NULL)
        fuse_exit(fuse);
    write(2, m, strlen(m));
}

static void empty_handler(__unused int sig) {}

static int setup_signal_handlers(void) {
    struct sigaction sa;
    sigset_t m;

    sa.sa_handler = exit_handler;
    sigemptyset(&(sa.sa_mask));
    sa.sa_flags = 0;

    if (sigaction(SIGHUP, &sa, NULL) == -1 ||
        sigaction(SIGINT, &sa, NULL) == -1 ||
        sigaction(SIGTERM, &sa, NULL) == -1) {

        sd_journal_print(LOG_CRIT, "Cannot set exit signal handlers: %s", strerror(errno));
        return -1;
    }

    sa.sa_handler = SIG_IGN;

    if (sigaction(SIGPIPE, &sa, NULL) == -1) {
        sd_journal_print(LOG_CRIT, "Cannot set ignored signals: %s", strerror(errno));
        return -1;
    }

    /* Used to shut down the locking thread */
    sa.sa_handler = empty_handler;

    if (sigaction(SIGUSR1, &sa, NULL) == -1) {
        sd_journal_print(LOG_CRIT, "Cannot set user signals: %s", strerror(errno));
        return -1;
    }

    sigemptyset(&m);
    pthread_sigmask(SIG_BLOCK, &m, &m);
    sigdelset(&m, SIGHUP);
    sigdelset(&m, SIGINT);
    sigdelset(&m, SIGTERM);
    sigaddset(&m, SIGPIPE);
    sigaddset(&m, SIGUSR1);
    pthread_sigmask(SIG_SETMASK, &m, NULL);

    return 0;
}

static int create_lock(int lock_timeout) {
    ne_session *session;
    char _owner[64], *owner;
    int i;
    int ret;

    lock = ne_lock_create();
    assert(lock);

    if (!(session = session_get(0)))
        return -1;

    if (!(owner = username))
        if (!(owner = getenv("USER")))
            if (!(owner = getenv("LOGNAME"))) {
                snprintf(_owner, sizeof(_owner), "%lu", (unsigned long) getuid());
                owner = owner;
            }

    ne_fill_server_uri(session, &lock->uri);

    lock->uri.path = strdup(base_directory);
    lock->depth = NE_DEPTH_INFINITE;
    lock->timeout = lock_timeout;
    lock->owner = strdup(owner);

    if (debug)
        sd_journal_print(LOG_DEBUG, "Acquiring lock...");

    for (i = 0; i < MAX_REDIRECTS; i++) {
        const ne_uri *u;

        if ((ret = ne_lock(session, lock)) != NE_REDIRECT)
            break;

        if (!(u = ne_redirect_location(session)))
            break;

        if (!session_is_local(u))
            break;

        if (debug)
            sd_journal_print(LOG_DEBUG, "REDIRECT FROM '%s' to '%s'", lock->uri.path, u->path);

        free(lock->uri.path);
        lock->uri.path = strdup(u->path);
    }

    if (ret) {
        sd_journal_print(LOG_ERR, "LOCK failed: %s", ne_get_error(session));
        ne_lock_destroy(lock);
        lock = NULL;
        return -1;
    }

    lock_store = ne_lockstore_create();
    assert(lock_store);

    ne_lockstore_add(lock_store, lock);

    return 0;
}

static int remove_lock(void) {
    ne_session *session;

    assert(lock);

    if (!(session = session_get(0)))
        return -1;

    if (debug)
        sd_journal_print(LOG_DEBUG, "Removing lock...");

    if (ne_unlock(session, lock)) {
        sd_journal_print(LOG_ERR, "UNLOCK failed: %s", ne_get_error(session));
        return -1;
    }

    return 0;
}

static void *lock_thread_func(void *p) {
    struct fusedav_config *conf = p;
    ne_session *session;
    sigset_t block;

    if (debug)
        sd_journal_print(LOG_DEBUG, "lock_thread entering");

    if (!(session = session_get(1)))
        return NULL;

    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);

    assert(lock);

    while (!lock_thread_exit) {
        int r, t;

        lock->timeout = conf->lock_timeout;

        pthread_sigmask(SIG_BLOCK, &block, NULL);
        r = ne_lock_refresh(session, lock);
        pthread_sigmask(SIG_UNBLOCK, &block, NULL);

        if (r) {
            sd_journal_print(LOG_ERR, "LOCK refresh failed: %s", ne_get_error(session));
            break;
        }

        if (lock_thread_exit)
            break;

        t = conf->lock_timeout/2;
        if (t <= 0)
            t = 1;
        sleep(t);
    }

    if (debug)
        sd_journal_print(LOG_DEBUG, "lock_thread exiting");

    return NULL;
}

int file_exists_or_set_null(char **path) {
    FILE *file;

    if ((file = fopen(*path, "r"))) {
        fclose(file);
        if (debug)
            sd_journal_print(LOG_DEBUG, "file_exists_or_set_null(%s): found", *path);
        return 0;
    }
    free(*path);
    *path = NULL;
    if (debug)
        sd_journal_print(LOG_DEBUG, "file_exists_or_set_null(%s): not found", *path);
    return 0;
}

static int fusedav_opt_proc(void *data, const char *arg, int key, struct fuse_args *outargs) {
    struct fusedav_config *config = data;
    
    switch (key) {
    case FUSE_OPT_KEY_NONOPT:
        if (!config->uri) {
            config->uri = strdup(arg);
            return 0;
        }
        break;

    case KEY_HELP:
        fprintf(stderr,
                "usage: %s uri mountpoint [options]\n"
                "\n"
                "general options:\n"
                "    -o opt,[opt...]  mount options\n"
                "    -h   --help      print help\n"
                "    -V   --version   print version\n"
                "\n"
                "fusedav mount options:\n"
                "    -o username=STRING\n"
                "    -o password=STRING\n"
                "    -o ca_certificate=PATH\n"
                "    -o client_certificate=PATH\n"
                "    -o client_certificate_password=STRING\n"
                "    -o lock_timeout=NUM\n"
                "    -o lock_on_mount\n"
                "    -o debug\n"
                "    -o nodaemon\n"
                "    -o noattributes\n"
                "\n"
                , outargs->argv[0]);
        fuse_opt_add_arg(outargs, "-ho");
        fuse_main(outargs->argc, outargs->argv, &dav_oper, &config);
        exit(1);
    
    case KEY_VERSION:
        fprintf(stderr, "fusedav version %s\n", PACKAGE_VERSION);
        fuse_opt_add_arg(outargs, "--version");
        fuse_main(outargs->argc, outargs->argv, &dav_oper, &config);
        exit(0);
    }
    return 1;
}

int main(int argc, char *argv[]) {
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct fusedav_config conf;
    struct fuse_chan *ch;
    char *mountpoint;
    int ret = 1;
    pthread_t lock_thread;
    int lock_thread_running = 0;
    int fail = 0;

    if (ne_sock_init()) {
        sd_journal_print(LOG_CRIT, "Failed to initialize libneon.");
        ++fail;
    }

    if (!ne_has_support(NE_FEATURE_SSL)) {
        sd_journal_print(LOG_CRIT, "fusedav requires libneon built with SSL.");
        ++fail;
    }

    if (!ne_has_support(NE_FEATURE_TS_SSL)) {
        sd_journal_print(LOG_CRIT, "fusedav requires libneon built with TS_SSL.");
        ++fail;
    }

    if (fail) {
        goto finish;
    }

    mask = umask(0);
    umask(mask);

    cache_alloc();

    if (setup_signal_handlers() < 0)
        goto finish;

    memset(&conf, 0, sizeof(conf));

    if (!fuse_opt_parse(&args, &conf, fusedav_opts, fusedav_opt_proc) < 0) {
        sd_journal_print(LOG_CRIT, "FUSE could not parse options.");
        goto finish;
    }
    if (debug)
        sd_journal_print(LOG_DEBUG, "Parsed options.");

    debug = conf.debug;
    if (debug)
        sd_journal_print(LOG_DEBUG, "Debug mode enabled.");

    if (fuse_parse_cmdline(&args, &mountpoint, NULL, NULL) < 0) {
        sd_journal_print(LOG_CRIT, "FUSE could not parse the command line.");
        goto finish;
    }
    if (debug)
        sd_journal_print(LOG_DEBUG, "Parsed command line.");

    if (!conf.uri) {
        sd_journal_print(LOG_CRIT, "Missing the required URI argument.");
        goto finish;
    }

    if (session_set_uri(conf.uri, conf.username, conf.password, conf.client_certificate, conf.ca_certificate) < 0) {
        sd_journal_print(LOG_CRIT, "Failed to initialize the session URI.");
        goto finish;
    }
    if (debug)
        sd_journal_print(LOG_DEBUG, "Set session URI and configuration.");

    if (!(ch = fuse_mount(mountpoint, &args))) {
        sd_journal_print(LOG_CRIT, "Failed to mount FUSE file system.");
        goto finish;
    }
    if (debug)
        sd_journal_print(LOG_DEBUG, "Mounted the FUSE file system.");

    if (!(fuse = fuse_new(ch, &args, &dav_oper, sizeof(dav_oper), &conf))) {
        sd_journal_print(LOG_CRIT, "Failed to create FUSE object.");
        goto finish;
    }
    if (debug)
        sd_journal_print(LOG_DEBUG, "Created the FUSE object.");

    if (conf.lock_on_mount && create_lock(conf.lock_timeout) >= 0) {
        int r;
        if ((r = pthread_create(&lock_thread, NULL, lock_thread_func, &conf)) < 0) {
            sd_journal_print(LOG_CRIT, "pthread_create(): %s", strerror(r));
            goto finish;
        }

        lock_thread_running = 1;
        if (debug)
            sd_journal_print(LOG_DEBUG, "Acquired lock.");
    }

    if (conf.nodaemon) {
        if (debug)
            sd_journal_print(LOG_DEBUG, "Running in foreground (skipping daemonization).");
    }
    else {
        if (debug)
            sd_journal_print(LOG_DEBUG, "Attempting to daemonize.");
        if (fuse_daemonize(/* run in foreground */ 0) < 0) {
            sd_journal_print(LOG_CRIT, "Failed to daemonize.");
            goto finish;
        }
    }

    if (debug)
        sd_journal_print(LOG_DEBUG, "Entering main FUSE loop.");
    if (fuse_loop_mt(fuse) < 0) {
        sd_journal_print(LOG_CRIT, "Error occurred while trying to enter main FUSE loop.");
        goto finish;
    }

    if (debug)
        sd_journal_print(LOG_DEBUG, "Exiting cleanly.");

    ret = 0;

finish:
    if (lock_thread_running) {
        lock_thread_exit = 1;
        pthread_kill(lock_thread, SIGUSR1);
        pthread_join(lock_thread, NULL);
        remove_lock();
        ne_lockstore_destroy(lock_store);

        if (debug)
            sd_journal_print(LOG_DEBUG, "Freed lock.");
    }

    if (ch != NULL) {
        if (debug)
            sd_journal_print(LOG_DEBUG, "Unmounting: %s", mountpoint);
        fuse_unmount(mountpoint, ch);
    }
    if (debug)
        sd_journal_print(LOG_DEBUG, "Unmounted.");

    if (fuse)
        fuse_destroy(fuse);
    if (debug)
        sd_journal_print(LOG_DEBUG, "Destroyed FUSE object.");

    fuse_opt_free_args(&args);
    if (debug)
        sd_journal_print(LOG_DEBUG, "Freed arguments.");

    file_cache_close_all();
    if (debug)
        sd_journal_print(LOG_DEBUG, "Closed file cache.");
    
    cache_free();
    if (debug)
        sd_journal_print(LOG_DEBUG, "Freed cache.");

    session_free();
    if (debug)
        sd_journal_print(LOG_DEBUG, "Freed session.");

    return ret;
}
