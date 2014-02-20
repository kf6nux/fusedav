/***
  This file is part of fusedav.

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pthread.h>
#include <assert.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdlib.h>
#include <grp.h>
#include <pwd.h>
#include <sys/prctl.h>
#include <glib.h>

#include "log.h"
#include "log_sections.h"
#include "statcache.h"
#include "filecache.h"
#include "session.h"
#include "fusedav.h"
#include "props.h"
#include "util.h"
#include "fusedav_config.h"
#include "signal_handling.h"
#include "stats.h"

mode_t mask = 0;
struct fuse* fuse = NULL;

#define CLOCK_SKEW 10 // seconds

// Run cache cleanup once a day.
#define CACHE_CLEANUP_INTERVAL 86400

struct fill_info {
    void *buf;
    fuse_fill_dir_t filler;
    const char *root;
};

enum ignore_freshness {OFF, ALREADY_FRESH, SAINT_MODE};

// forward reference to avoid dependency loop
static void common_unlink(const char *path, bool do_unlink, GError **gerr);

// GError mechanisms
static G_DEFINE_QUARK("FUSEDAV", fusedav)

static int processed_gerror(const char *prefix, const char *path, GError **pgerr) {
    int ret;
    GError *gerr = *pgerr;
    log_print(LOG_ERR, SECTION_FUSEDAV_DEFAULT, "%s on %s: %s -- %d: %s", prefix, path ? path : "null path", gerr->message, gerr->code, g_strerror(gerr->code));
    ret = -gerr->code;
    if (gerr->code == ENOENT) log_print(LOG_INFO, SECTION_ENHANCED, "processed_gerror: fusedav.error-ENOENT:1|c");
    else if (gerr->code == ENETDOWN) log_print(LOG_INFO, SECTION_ENHANCED, "processed_gerror: fusedav.error-ENETDOWN:1|c");
    else if (gerr->code == EIO) log_print(LOG_INFO, SECTION_ENHANCED, "processed_gerror: fusedav.error-EIO:1|c");
    else log_print(LOG_INFO, SECTION_ENHANCED, "processed_gerror: fusedav.error-OTHER:1|c");
    g_clear_error(pgerr);
    return ret;
}

static int simple_propfind_with_redirect(
        const char *path,
        int depth,
        time_t last_updated,
        props_result_callback result_callback,
        void *userdata,
        GError **gerr) {

    GError *subgerr = NULL;
    int ret;

    log_print(LOG_DEBUG, SECTION_FUSEDAV_STAT, "simple_propfind_with_redirect: Performing (%s) PROPFIND of depth %d on path %s.", last_updated > 0 ? "progressive" : "complete", depth, path);

    ret = simple_propfind(path, depth, last_updated, result_callback, userdata, &subgerr);
    if (subgerr) {
        g_propagate_prefixed_error(gerr, subgerr, "simple_propfind_with_redirect: ");
        return ret;
    }

    log_print(LOG_DEBUG, SECTION_FUSEDAV_STAT, "simple_propfind_with_redirect: Done with (%s) PROPFIND.", last_updated > 0 ? "progressive" : "complete");

    return ret;
}

static void fill_stat_generic(struct stat *st, mode_t mode, bool is_dir, int fd, GError **gerr) {

    // initialize to 0
    memset(st, 0, sizeof(struct stat));

    log_print(LOG_DEBUG, SECTION_FUSEDAV_STAT, "fill_stat_generic: Enter");

    st->st_mode = mode;
    if (is_dir) {
        st->st_mode |= S_IFDIR;
        // In a POSIX systems, directories with subdirs have nlink = 3; otherwise 2. Just use 3
        st->st_nlink = 3;
        // on local systems, directories seem to have size 4096 when they have few files.
        st->st_size = 4096;
    }
    else {
        st->st_mode |= S_IFREG;
        st->st_nlink = 1;
        // If we are creating a file, size will start at 0.
        st->st_size = 0;
    }
    st->st_atime = time(NULL);
    st->st_mtime = st->st_atime;
    st->st_ctime = st->st_mtime;
    st->st_blksize = 4096;

    if (fd >= 0) {
        st->st_size = lseek(fd, 0, SEEK_END);
        st->st_blocks = (st->st_size+511)/512;
        log_print(LOG_DEBUG, SECTION_FUSEDAV_STAT, "fill_stat_generic: seek: fd = %d : size = %d : %d %s", fd, st->st_size, errno, strerror(errno));
        if (st->st_size < 0 || inject_error(fusedav_error_fillstsize)) {
            g_set_error(gerr, fusedav_quark(), errno, "fill_stat_generic failed lseek");
            return;
        }
    }

    log_print(LOG_DEBUG, SECTION_FUSEDAV_STAT, "Done with fill_stat_generic: fd = %d : size = %d", fd, st->st_size);
}

static void getdir_propfind_callback(__unused void *userdata, const char *path, struct stat st,
    unsigned long status_code, GError **gerr) {

    struct fusedav_config *config = fuse_get_context()->private_data;
    struct stat_cache_value value;
    GError *subgerr1 = NULL ;
    GError *subgerr2 = NULL ;

    // Zero-out structure; some fields we don't populate but want to be 0, e.g. st_atim.tv_nsec
    memset(&value, 0, sizeof(struct stat_cache_value));
    value.st = st;

    log_print(LOG_INFO, SECTION_FUSEDAV_PROP, "getdir_propfind_callback: %s (%lu)", path, status_code);

    if (status_code == 410) {
        struct stat_cache_value *existing;

        log_print(LOG_DEBUG, SECTION_FUSEDAV_PROP, "getdir_propfind_callback: DELETE %s (%lu)", path, status_code);
        existing = stat_cache_value_get(config->cache, path, true, &subgerr1);
        if (subgerr1) {
            g_propagate_prefixed_error(gerr, subgerr1, "getdir_propfind_callback: ");
            return;
        }

        /* If existing indicates that the file existed at a later timestamp than this delete, keep it. */
        /* By returning early, here and other places below, we are not updating the updated field on the file,
         * which stat_cache_value_set below would do; we leave that value as it was. We are not seeing a
         * creation for this file in the propfind, so it would not be correct to update the updated value as if we had.
         */
        if (existing && existing->updated > st.st_ctime) {
            log_print(LOG_NOTICE, SECTION_FUSEDAV_PROP, "Ignoring outdated removal of path: %s (%lu %lu)", path, existing->updated, st.st_ctime);
            free(existing);
            return;
        }
        /* If existing indicates that the file existed at the same timestamp as this delete,
         * we don't know which to honor. So query the server to see if it exists there and
         * use that.
         */
        else if (existing && existing->updated == st.st_ctime) {
            CURL *session = NULL;
            long response_code = 500; // seed it as bad so we can enter the loop
            CURLcode res = CURLE_OK;

            free(existing);

            for (int idx = 0; idx < num_filesystem_server_nodes && (res != CURLE_OK || response_code >= 500); idx++) {
                bool temporary_handle = true; // self-documenting
                bool new_resolve_list;

                if (idx == 0) new_resolve_list = false;
                else new_resolve_list = true;

                if (!(session = session_request_init(path, NULL, temporary_handle, new_resolve_list)) || inject_error(fusedav_error_propfindsession)) {
                    g_set_error(gerr, fusedav_quark(), ENETDOWN, "getdir_propfind_callback(%s): failed to get request session", path);
                    return;
                }

                // This makes a "HEAD" call
                curl_easy_setopt(session, CURLOPT_NOBODY, 1);

                log_print(LOG_DEBUG, SECTION_FUSEDAV_PROP, "getdir_propfind_callback: saw 410; calling HEAD on %s", path);
                res = curl_easy_perform(session);
                if(res == CURLE_OK) {
                    curl_easy_getinfo(session, CURLINFO_RESPONSE_CODE, &response_code);
                }

                log_filesystem_nodes("getdir_propfind_callback", res, response_code, idx, path);
            }

            if (session) session_temp_handle_destroy(session);

            if(res != CURLE_OK || response_code >= 500 || inject_error(fusedav_error_propfindhead)) {
                set_saint_mode();
                g_set_error(gerr, fusedav_quark(), ENETDOWN, "getdir_propfind_callback: curl failed: %s : rc: %ld\n",
                    curl_easy_strerror(res), response_code);
                return;
            }

            if (response_code >= 400 && response_code < 500) {
                // fall through to delete
                log_print(LOG_NOTICE, SECTION_FUSEDAV_PROP, "getdir_propfind_callback: saw 410; executed HEAD; file doesn't exist: %s", path);
            }
            else {
                // REVIEW: if the file exists, do we want to call stat_cache_value_set and update its ->updated value?
                if (response_code >= 200 && response_code < 300) {
                    // We're not deleting since it exists; jump out!
                    log_print(LOG_NOTICE, SECTION_FUSEDAV_PROP, "getdir_propfind_callback: saw 410; executed HEAD; file exists: %s", path);
                }
                else {
                    // On error, prefer retaining a file which should be deleted over deleting a file which should be retained
                    g_set_error(gerr, fusedav_quark(), EINVAL,
                        "getdir_propfind_callback(%s): saw 410; HEAD returns unexpected response from curl %ld",
                        path, response_code);
                }
                return;
            }
        }

        log_print(LOG_DEBUG, SECTION_FUSEDAV_PROP, "Removing path: %s", path);
        stat_cache_delete(config->cache, path, &subgerr1);
        filecache_delete(config->cache, path, true, &subgerr2);
        // If we need to combine 2 errors, use one of the error messages in the propagated prefix
        if (subgerr1 && subgerr2) {
            g_propagate_prefixed_error(gerr, subgerr1, "getdir_propfind_callback: %s :: ", subgerr2->message);
        }
        else if (subgerr1) {
            g_propagate_prefixed_error(gerr, subgerr1, "getdir_propfind_callback: ");
        }
        else if (subgerr2) {
            g_propagate_prefixed_error(gerr, subgerr2, "getdir_propfind_callback: ");
        }
    }
    else {
        log_print(LOG_DEBUG, SECTION_FUSEDAV_PROP, "getdir_propfind_callback: CREATE %s (%lu)", path, status_code);
        stat_cache_value_set(config->cache, path, &value, &subgerr1);
        if (subgerr1) {
            g_propagate_prefixed_error(gerr, subgerr1, "getdir_propfind_callback: ");
            return;
        }
    }
}

static void getdir_cache_callback(__unused const char *path_prefix, const char *filename, void *user) {
    struct fill_info *f = user;

    assert(f);

    if (strlen(filename) > 0) {
        log_print(LOG_DEBUG, SECTION_FUSEDAV_STAT, "getdir_cache_callback path: %s", filename);
        f->filler(f->buf, filename, NULL, 0);
    }
}

static void update_directory(const char *path, bool attempt_progessive_update, GError **gerr) {
    struct fusedav_config *config = fuse_get_context()->private_data;
    GError *tmpgerr = NULL;
    bool needs_update = true;
    time_t last_updated;
    time_t timestamp;
    int propfind_result;

    // Attempt to freshen the cache.
    if (attempt_progessive_update && config->progressive_propfind) {
        timestamp = time(NULL);
        last_updated = stat_cache_read_updated_children(config->cache, path, &tmpgerr);
        if (tmpgerr) {
            g_propagate_prefixed_error(gerr, tmpgerr, "update_directory: ");
            return;
        }
        log_print(LOG_DEBUG, SECTION_FUSEDAV_STAT, "update_directory: Freshening directory data: %s", path);

        propfind_result = simple_propfind_with_redirect(path, PROPFIND_DEPTH_ONE, last_updated - CLOCK_SKEW,
            getdir_propfind_callback, NULL, &tmpgerr);
        // On true error, we set an error and return, avoiding the complete PROPFIND.
        // On sucess we avoid the complete PROPFIND
        // On ESTALE, we do a complete PROPFIND
        // If error is injected, it falls through to final else
        if (propfind_result == 0 && !inject_error(fusedav_error_updatepropfind1)) {
            log_print(LOG_DEBUG, SECTION_FUSEDAV_STAT, "update_directory: progressive PROPFIND success");
            needs_update = false;
        }
        else if (propfind_result == -ESTALE && !inject_error(fusedav_error_updatepropfind1)) {
            log_print(LOG_DEBUG, SECTION_FUSEDAV_STAT, "update_directory: progressive PROPFIND Precondition Failed.");
        }
        else if (tmpgerr) { // if injecting errors, process this error in preference to fusedav_error_updatepropfind1
            g_propagate_prefixed_error(gerr, tmpgerr, "update_directory: ");
            return;
        }
        else {
            g_set_error(gerr, fusedav_quark(), ENETDOWN, "update_directory: progressive propfind errored: ");
            return;
        }
    }

    // If we had *no data* or freshening failed, rebuild the cache with a full PROPFIND.
    if (needs_update) {
        unsigned long min_generation;

        // Up log level to NOTICE temporarily to get reports in the logs
        log_print(LOG_INFO, SECTION_FUSEDAV_STAT, "update_directory: Doing complete PROPFIND (attempt_progessive_update=%d): %s", attempt_progessive_update, path);
        timestamp = time(NULL);
        // min_generation gets value here
        min_generation = stat_cache_get_local_generation();
        // getdir_propfind_callback calls stat_cache_value_set, which makes local_generation one higher than min_generation
        propfind_result = simple_propfind_with_redirect(path, PROPFIND_DEPTH_ONE, 0, getdir_propfind_callback, NULL, &tmpgerr);
        if (tmpgerr) {
            g_propagate_prefixed_error(gerr, tmpgerr, "update_directory: ");
            return;
        }
        else if (propfind_result < 0 || inject_error(fusedav_error_updatepropfind2)) {
            g_set_error(gerr, fusedav_quark(), ENETDOWN, "update_directory: Complete PROPFIND failed on %s", path);
            return;
        }

        // All files in propfind list will have local_generation > min_generation and will not be subject to deletion
        stat_cache_delete_older(config->cache, path, min_generation, &tmpgerr);
        if (tmpgerr) {
            g_propagate_prefixed_error(gerr, tmpgerr, "update_directory: ");
            return;
        }
    }

    // Mark the directory contents as updated.
    log_print(LOG_DEBUG, SECTION_FUSEDAV_STAT, "update_directory: Marking directory %s as updated at timestamp %lu.", path, timestamp);
    stat_cache_updated_children(config->cache, path, timestamp, &tmpgerr);
    if (tmpgerr) {
        g_propagate_prefixed_error(gerr, tmpgerr, "update_directory: ");
        return;
    }
    return;
}

static int dav_readdir(
        const char *path,
        void *buf,
        fuse_fill_dir_t filler,
        __unused off_t offset,
        __unused struct fuse_file_info *fi) {

    struct fusedav_config *config = fuse_get_context()->private_data;
    struct fill_info f;
    GError *gerr = NULL;
    int ret;
    bool ignore_freshness = false;

    BUMP(dav_readdir);

    // We might get a null path if we are accessing a bare file descriptor
    // (we have unlinked the path but kept the file descriptor open)
    // Since it's a directory name, this is unexpected. While we can imagine
    // a scenario, we won't go out of our way to handle it. Exit with an error.
    if (path == NULL) {
        log_print(LOG_INFO, SECTION_FUSEDAV_DIR, "CALLBACK: dav_readdir(NULL path)");
        return -ENOENT;
    }

    log_print(LOG_INFO, SECTION_FUSEDAV_DIR, "CALLBACK: dav_readdir(%s)", path);

    f.buf = buf;
    f.filler = filler;
    f.root = path;

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    // First, attempt to hit the cache.
    ret = stat_cache_enumerate(config->cache, path, getdir_cache_callback, &f, ignore_freshness);
    if (ret < 0) {
        if (ret == -STAT_CACHE_OLD_DATA) log_print(LOG_DEBUG, SECTION_FUSEDAV_DIR, "DIR-CACHE-TOO-OLD: %s", path);
        else if (ret == -STAT_CACHE_NO_DATA) log_print(LOG_DEBUG, SECTION_FUSEDAV_DIR, "DIR_CACHE-NO-DATA available: %s", path);
        else log_print(LOG_DEBUG, SECTION_FUSEDAV_DIR, "DIR-CACHE-MISS: %s", path);

        log_print(LOG_DEBUG, SECTION_FUSEDAV_DIR, "dav_readdir: Updating directory: %s", path);
        update_directory(path, (ret == -STAT_CACHE_OLD_DATA), &gerr);
        if (gerr) {
            return processed_gerror("dav_readdir: failed to update directory: ", path, &gerr);
        }

        // Output the new data, skipping any cache freshness checks
        // (which should pass, anyway, unless it's grace mode).
        // At this point, we can only get a zero return, or an empty directory. Let both fall through and return 0
        stat_cache_enumerate(config->cache, path, getdir_cache_callback, &f, true);
    }

    log_print(LOG_DEBUG, SECTION_FUSEDAV_DIR, "dav_readdir: Successful readdir for path: %s", path);
    return 0;
}

static void getattr_propfind_callback(__unused void *userdata, const char *path, struct stat st,
        unsigned long status_code, GError **gerr) {
    struct fusedav_config *config = fuse_get_context()->private_data;
    struct stat_cache_value value;
    GError *subgerr1 = NULL;
    GError *subgerr2 = NULL;

    // Zero-out structure; some fields we don't populate but want to be 0, e.g. st_atim.tv_nsec
    memset(&value, 0, sizeof(struct stat_cache_value));
    value.st = st;

    if (status_code == 410) {
        log_print(LOG_DEBUG, SECTION_FUSEDAV_PROP, "getattr_propfind_callback: Deleting from stat cache: %s", path);
        stat_cache_delete(config->cache, path, &subgerr1);
        filecache_delete(config->cache, path, true, &subgerr2);

        // If we need to combine 2 errors, use one of the error messages in the propagated prefix
        if (subgerr1 && subgerr2) {
            log_print(LOG_WARNING, SECTION_FUSEDAV_PROP, "getattr_propfind_callback: %s: %s: %s", path, subgerr1->message, subgerr2->message);
            g_propagate_prefixed_error(gerr, subgerr1, "getattr_propfind_callback: %s :: ", subgerr2->message);
            return;
        }
        else if (subgerr1) {
            log_print(LOG_WARNING, SECTION_FUSEDAV_PROP, "getattr_propfind_callback: %s: %s", path, subgerr1->message);
            g_propagate_prefixed_error(gerr, subgerr1, "getattr_propfind_callback: ");
            return;
        }
        else if (subgerr2) {
            log_print(LOG_WARNING, SECTION_FUSEDAV_PROP, "getattr_propfind_callback: %s: %s", path, subgerr2->message);
            g_propagate_prefixed_error(gerr, subgerr2, "getattr_propfind_callback: ");
            return;
        }
    }
    else {
        log_print(LOG_DEBUG, SECTION_FUSEDAV_PROP, "getattr_propfind_callback: Adding to stat cache: %s", path);
        stat_cache_value_set(config->cache, path, &value, &subgerr1);
        if (subgerr1) {
            log_print(LOG_WARNING, SECTION_FUSEDAV_PROP, "getattr_propfind_callback: %s: %s", path, subgerr1->message);
            g_propagate_prefixed_error(gerr, subgerr1, "getattr_propfind_callback: ");
            return;
        }
    }
}

/* The calls to this function are pretty well prescribed, in get_stat(), but if called elsewhere,
 * could be problematic.
 */
static int get_stat_from_cache(const char *path, struct stat *stbuf, bool ignore_freshness, GError **gerr) {
    struct fusedav_config *config = fuse_get_context()->private_data;
    struct stat_cache_value *response;
    GError *tmpgerr = NULL;

    response = stat_cache_value_get(config->cache, path, ignore_freshness, &tmpgerr);

    if (tmpgerr) {
        g_propagate_prefixed_error(gerr, tmpgerr, "get_stat_from_cache: ");
        memset(stbuf, 0, sizeof(struct stat));
        return -1;
    }

    if (response == NULL) {
        log_print(LOG_DEBUG, SECTION_FUSEDAV_STAT, "get_stat_from_cache: NULL response from stat_cache_value_get for path %s.", path);

        if (ignore_freshness || inject_error(fusedav_error_statignorefreshness)) {
            log_print(LOG_DEBUG, SECTION_FUSEDAV_STAT, "get_stat_from_cache: Ignoring freshness and sending -ENOENT for path %s.", path);
            memset(stbuf, 0, sizeof(struct stat));
            if (use_saint_mode()) {
                g_set_error(gerr, fusedav_quark(), ENETDOWN, "get_stat_from_cache: ");
            }
            else {
                g_set_error(gerr, fusedav_quark(), ENOENT, "get_stat_from_cache: ");
            }
            return -1;
        }
        // else we're not skipping the freshness check and maybe the item is in the cache but just out of date
        log_print(LOG_DEBUG, SECTION_FUSEDAV_STAT, "get_stat_from_cache: Treating key as absent or expired for path %s.", path);
        return -EKEYEXPIRED;
    }

    log_print(LOG_DEBUG, SECTION_FUSEDAV_STAT, "get_stat_from_cache: Got response from stat_cache_value_get for path %s.", path);
    *stbuf = response->st;
    print_stat(stbuf, "stat_cache_value_get response");
    free(response);
    log_print(LOG_DEBUG, SECTION_FUSEDAV_STAT, "get_stat_from_cache(%s, stbuf, %d): returns %s", path, ignore_freshness, stbuf->st_mode ? "0" : "ENOENT");
    if (stbuf->st_mode == 0 || inject_error(fusedav_error_statstmode)) {
        g_set_error(gerr, fusedav_quark(), ENOENT, "get_stat_from_cache: stbuf mode is 0: ");
        return -1;
    }
    return 0;

}

static void get_stat(const char *path, struct stat *stbuf, GError **gerr) {
    struct fusedav_config *config = fuse_get_context()->private_data;
    char *parent_path = NULL;
    GError *tmpgerr = NULL;
    time_t parent_children_update_ts;
    bool is_base_directory;
    int ret = -ENOENT;
    bool skip_freshness_check = false;

    memset(stbuf, 0, sizeof(struct stat));

    log_print(LOG_DEBUG, SECTION_FUSEDAV_STAT, "get_stat(%s, stbuf)", path);

    log_print(LOG_DEBUG, SECTION_FUSEDAV_STAT, "Checking if path %s matches base directory.", path);
    is_base_directory = (strcmp(path, "/") == 0);

    // If it's the root directory and all attributes are specified, construct a response.
    if (is_base_directory) {

        // mode = 0 (unspecified), is_dir = true; fd = -1, irrelevant for dir
        fill_stat_generic(stbuf, 0, true, -1, &tmpgerr);
        if (tmpgerr) {
            g_propagate_prefixed_error(gerr, tmpgerr, "get_stat: ");
            return;
        }

        log_print(LOG_DEBUG, SECTION_FUSEDAV_STAT, "Used constructed stat data for base directory.");
        return;
    }

    // Check if we can directly hit this entry in the stat cache.
    ret = get_stat_from_cache(path, stbuf, skip_freshness_check, &tmpgerr);

    // Propagate the error but let the rest of the logic determine return value
    // Unless we change the logic in get_stat_from_cache, it will return ENOENT or ENETDOWN
    if (tmpgerr) {
        g_propagate_prefixed_error(gerr, tmpgerr, "get_stat: ");
        return;
    }
    else if (ret == 0) {
        return;
    }
    // else fall through, this would be for EKEYEXPIRED, indicating statcache miss

    log_print(LOG_DEBUG, SECTION_FUSEDAV_STAT, "STAT-CACHE-MISS");

    // If it's the root directory or refresh_dir_for_file_stat is false,
    // just do a single, zero-depth PROPFIND.
    if (!config->refresh_dir_for_file_stat || is_base_directory) {
        GError *subgerr = NULL;
        log_print(LOG_DEBUG, SECTION_FUSEDAV_STAT, "Performing zero-depth PROPFIND on path: %s", path);
        ret = simple_propfind_with_redirect(path, PROPFIND_DEPTH_ZERO, 0, getattr_propfind_callback, NULL, &subgerr);
        if (subgerr) {
            // Delete from cache on error; ignore errors from stat_cache_delete since we already have one
            g_propagate_prefixed_error(gerr, subgerr, "get_stat: ");
            stat_cache_delete(config->cache, path, NULL);
            goto fail;
        }
        else if (ret < 0) { // We don't ever expect ret < 0 without gerr
            stat_cache_delete(config->cache, path, &subgerr);
            if (subgerr) {
                g_propagate_prefixed_error(gerr, subgerr, "get_stat: PROPFIND failed");
                goto fail;
            }
            g_set_error(gerr, fusedav_quark(), ENETDOWN, "get_stat: PROPFIND failed");
            goto fail;
        }
        log_print(LOG_DEBUG, SECTION_FUSEDAV_STAT, "Zero-depth PROPFIND succeeded: %s", path);

        // The propfind succeeded, so if the item does not exist, then it is ENOENT.
        skip_freshness_check = true;
        // If in saint mode and the item wasn't already in the stat cache, this will return ENETDOWN.
        get_stat_from_cache(path, stbuf, skip_freshness_check, &subgerr);
        if (subgerr) {
            g_propagate_prefixed_error(gerr, subgerr, "get_stat: ");
            goto fail;
        }
        // success (we could just return, but it looks more consistent to goto finish after the goto fails above
        goto finish;
    }

    // If we're here, refresh_dir_for_file_stat is set, so we're updating
    // directory stat data to, in turn, update the desired file stat data.

    parent_path = path_parent(path);
    if (parent_path == NULL) goto fail;

    log_print(LOG_DEBUG, SECTION_FUSEDAV_STAT, "Getting parent path entry: %s", parent_path);
    parent_children_update_ts = stat_cache_read_updated_children(config->cache, parent_path, &tmpgerr);
    if (tmpgerr) {
        g_propagate_prefixed_error(gerr, tmpgerr, "get_stat: ");
        goto fail;
    }
    log_print(LOG_DEBUG, SECTION_FUSEDAV_STAT, "Parent was updated: %s %lu", parent_path, parent_children_update_ts);

    // If the parent directory is out of date, update it.
    if (parent_children_update_ts < (time(NULL) - STAT_CACHE_NEGATIVE_TTL)) {
        GError *subgerr = NULL;

        BUMP(fusedav_nonnegative_cache);
        // If parent_children_update_ts is 0, there are no entries for updated_children in statcache
        // In that case, skip the progressive propfind and go straight to complete propfind
        update_directory(parent_path, (parent_children_update_ts > 0), &subgerr);
        if (subgerr) {
            g_propagate_prefixed_error(gerr, subgerr, "get_stat: ");
            goto fail;
        }
    } else {
        BUMP(fusedav_negative_cache);
    }

    // Try again to hit the file in the stat cache.
    skip_freshness_check = true;
    // If in saint mode and the item wasn't already in the stat cache, this will return ENETDOWN.
    ret = get_stat_from_cache(path, stbuf, skip_freshness_check, &tmpgerr);
    if (tmpgerr) {
        log_print(LOG_DEBUG, SECTION_FUSEDAV_STAT, "get_stat: propagating error from get_stat_from_cache on %s", path);
        g_propagate_prefixed_error(gerr, tmpgerr, "get_stat: ");
        goto fail;
    }
    if (ret == 0) goto finish;

fail:
    memset(stbuf, 0, sizeof(struct stat));

finish:
    free(parent_path);
    return;
}

static void common_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *info, GError **gerr) {
    GError *tmpgerr = NULL;

    assert(info != NULL || path != NULL);

    if (path != NULL) {
        get_stat(path, stbuf, &tmpgerr);
        if (tmpgerr) {
            g_propagate_prefixed_error(gerr, tmpgerr, "common_getattr: ");
            return;
        }
        // These are taken care of by fill_stat_generic below if path is NULL
        if (S_ISDIR(stbuf->st_mode))
            stbuf->st_mode |= S_IFDIR;
        if (S_ISREG(stbuf->st_mode))
            stbuf->st_mode |= S_IFREG;
    }
    else {
        int fd = filecache_fd(info);
        log_print(LOG_INFO, SECTION_FUSEDAV_STAT, "common_getattr(NULL path)");
        // Fill in generic values
        // We can't be a directory if we have a null path
        // mode = 0 (unspecified), is_dir = false; fd to get size
        fill_stat_generic(stbuf, 0, false, fd, &tmpgerr);
        if (tmpgerr) {
            g_propagate_prefixed_error(gerr, tmpgerr, "common_getattr: ");
            return;
        }
    }

    // Zero-out unused nanosecond fields.
    stbuf->st_atim.tv_nsec = 0;
    stbuf->st_mtim.tv_nsec = 0;
    stbuf->st_ctim.tv_nsec = 0;

    //assert(stbuf->st_mode);

    return;
}

static int dav_fgetattr(const char *path, struct stat *stbuf, struct fuse_file_info *info) {
    GError *gerr = NULL;

    BUMP(dav_fgetattr);

    log_print(LOG_INFO, SECTION_FUSEDAV_STAT, "CALLBACK: dav_fgetattr(%s)", path?path:"null path");
    common_getattr(path, stbuf, info, &gerr);
    if (gerr) {
        if (gerr->code == ENOENT) {
            int res = -gerr->code;
            log_print(LOG_DEBUG, SECTION_FUSEDAV_STAT, "dav_fgetattr(%s): ENOENT", path?path:"null path");
            g_clear_error(&gerr);
            return res;
        }
        return processed_gerror("dav_fgetattr: ", path, &gerr);
    }
    log_print(LOG_DEBUG, SECTION_FUSEDAV_STAT, "Done: dav_fgetattr(%s)", path?path:"null path");

    return 0;
}

static int dav_getattr(const char *path, struct stat *stbuf) {
    GError *gerr = NULL;

    BUMP(dav_getattr);

    log_print(LOG_INFO, SECTION_FUSEDAV_STAT, "CALLBACK: dav_getattr(%s)", path);
    common_getattr(path, stbuf, NULL, &gerr);
    if (gerr) {
        // Don't print error on ENOENT; that's what get_attr is for
        if (gerr->code == ENOENT) {
            int res = -gerr->code;
            log_print(LOG_DEBUG, SECTION_FUSEDAV_STAT, "dav_getattr(%s): ENOENT", path?path:"null path");
            g_clear_error(&gerr);
            return res;
        }
        return processed_gerror("dav_getattr: ", path, &gerr);
    }
    print_stat(stbuf, "dav_getattr");
    log_print(LOG_DEBUG, SECTION_FUSEDAV_STAT, "Done: dav_getattr(%s)", path);

    return 0;
}

static void common_unlink(const char *path, bool do_unlink, GError **gerr) {
    struct fusedav_config *config = fuse_get_context()->private_data;
    struct stat st;
    GError *gerr2 = NULL;
    GError *gerr3 = NULL;

    get_stat(path, &st, &gerr2);
    if (gerr2) {
        g_propagate_prefixed_error(gerr, gerr2, "common_unlink: ");
        return;
    }

    if (!S_ISREG(st.st_mode) || inject_error(fusedav_error_cunlinkisdir)) {
        g_set_error(gerr, fusedav_quark(), EISDIR, "common_unlink: is a directory");
        return;
    }

    if (do_unlink) {
        CURLcode res = CURLE_OK;
        long response_code = 500; // seed it as bad so we can enter the loop

        if (use_saint_mode()) {
            g_set_error(gerr, fusedav_quark(), ENETDOWN, "common_unlink: already in saint mode");
            return;
        }

        for (int idx = 0; idx < num_filesystem_server_nodes && (res != CURLE_OK || response_code >= 500); idx++) {
            CURL *session;
            struct curl_slist *slist = NULL;
            bool new_resolve_list;

            if (idx == 0) new_resolve_list = false;
            else new_resolve_list = true;

            slist = NULL;
            if (!(session = session_request_init(path, NULL, false, new_resolve_list)) || inject_error(fusedav_error_cunlinksession)) {
                g_set_error(gerr, fusedav_quark(), ENETDOWN, "common_unlink(%s): failed to get request session", path);
                return;
            }

            curl_easy_setopt(session, CURLOPT_CUSTOMREQUEST, "DELETE");

            slist = enhanced_logging(slist, LOG_INFO, SECTION_FUSEDAV_FILE, "common_unlink: %s", path);
            if (slist) curl_easy_setopt(session, CURLOPT_HTTPHEADER, slist);

            log_print(LOG_DEBUG, SECTION_FUSEDAV_FILE, "common_unlink: calling DELETE on %s", path);
            res = curl_easy_perform(session);
            if(res == CURLE_OK) {
                curl_easy_getinfo(session, CURLINFO_RESPONSE_CODE, &response_code);
            }

            if (slist) curl_slist_free_all(slist);

            log_filesystem_nodes("common_unlink", res, response_code, idx, path);
        }

        if(res != CURLE_OK || response_code >= 500 || inject_error(fusedav_error_cunlinkcurl)) {
            set_saint_mode();
            g_set_error(gerr, fusedav_quark(), ENETDOWN, "common_unlink: DELETE failed: %s\n", curl_easy_strerror(res));
            return;
        }
    }

    log_print(LOG_DEBUG, SECTION_FUSEDAV_FILE, "common_unlink: calling filecache_delete on %s", path);
    filecache_delete(config->cache, path, true, &gerr2);

    log_print(LOG_DEBUG, SECTION_FUSEDAV_FILE, "common_unlink: calling stat_cache_delete on %s", path);
    stat_cache_delete(config->cache, path, &gerr3);

    // If we need to combine 2 errors, use one of the error messages in the propagated prefix
    if (gerr2 && gerr3) {
        g_propagate_prefixed_error(gerr, gerr2, "common_unlink: %s :: ", gerr3->message);
    }
    else if (gerr2) {
        g_propagate_prefixed_error(gerr, gerr2, "common_unlink: ");
    }
    else if (gerr3) {
        g_propagate_prefixed_error(gerr, gerr3, "common_unlink: ");
    }

    return;
}

static int dav_unlink(const char *path) {
    GError *gerr = NULL;
    bool do_unlink = true;

    BUMP(dav_unlink);

    log_print(LOG_INFO, SECTION_FUSEDAV_FILE, "CALLBACK: dav_unlink(%s)", path);

    common_unlink(path, do_unlink, &gerr);
    if (gerr) {
        return processed_gerror("dav_unlink: ", path, &gerr);
    }

    return 0;
}

static int dav_rmdir(const char *path) {
    struct fusedav_config *config = fuse_get_context()->private_data;
    GError *gerr = NULL;
    char fn[PATH_MAX];
    bool has_child;
    struct stat st;
    long response_code = 500; // seed it as bad so we can enter the loop
    CURLcode res = CURLE_OK;

    BUMP(dav_rmdir);

    log_print(LOG_INFO, SECTION_FUSEDAV_DIR, "CALLBACK: dav_rmdir(%s)", path);

    if (use_saint_mode()) {
        log_print(LOG_ERR, SECTION_FUSEDAV_DIR, "dav_rmdir(%s): already in saint mode", path);
        return -ENETDOWN;
    }

    get_stat(path, &st, &gerr);
    if (gerr) {
        return processed_gerror("dav_rmdir: ", path, &gerr);
    }

    if (!S_ISDIR(st.st_mode)) {
        log_print(LOG_INFO, SECTION_FUSEDAV_DIR, "dav_rmdir: failed to remove `%s\': Not a directory", path);
        return -ENOTDIR;
    }

    // The slash should force it to find entries in the directory after the slash, and
    // not the directory itself
    snprintf(fn, sizeof(fn), "%s/", path);

    // Check to see if it is empty ...
    // get_stat already called update_directory, which called stat_cache_updated_children
    // so the stat cache should be up to date.
    has_child = stat_cache_dir_has_child(config->cache, path);
    if (has_child) {
        log_print(LOG_INFO, SECTION_FUSEDAV_DIR, "dav_rmdir: failed to remove `%s\': Directory not empty ", path);
        return -ENOTEMPTY;
    }

    for (int idx = 0; idx < num_filesystem_server_nodes && (res != CURLE_OK || response_code >= 500); idx++) {
        CURL *session;
        struct curl_slist *slist = NULL;
        bool new_resolve_list;

        if (idx == 0) new_resolve_list = false;
        else new_resolve_list = true;

        slist = NULL;

        if (!(session = session_request_init(fn, NULL, false, new_resolve_list))) {
            log_print(LOG_WARNING, SECTION_FUSEDAV_DIR, "dav_rmdir(%s): failed to get session", path);
            return -ENETDOWN;
        }

        curl_easy_setopt(session, CURLOPT_CUSTOMREQUEST, "DELETE");

        slist = enhanced_logging(slist, LOG_INFO, SECTION_FUSEDAV_DIR, "dav_rmdir: %s", path);
        if (slist) curl_easy_setopt(session, CURLOPT_HTTPHEADER, slist);

        res = curl_easy_perform(session);
        if(res == CURLE_OK) {
            curl_easy_getinfo(session, CURLINFO_RESPONSE_CODE, &response_code);
        }

        if (slist) curl_slist_free_all(slist);

        log_filesystem_nodes("dav_rmdir", res, response_code, idx, path);
    }

    if (res != CURLE_OK || response_code >= 500) {
        set_saint_mode();
        log_print(LOG_ERR, SECTION_FUSEDAV_DIR, "dav_rmdir(%s): DELETE failed: %s", path, curl_easy_strerror(res));
        return -ENETDOWN;
    }

    log_print(LOG_DEBUG, SECTION_FUSEDAV_DIR, "dav_rmdir: removed(%s)", path);

    stat_cache_delete(config->cache, path, &gerr);
    if (gerr) {
        return processed_gerror("dav_rmdir: ", path, &gerr);
    }

    // Delete Updated_children entry for path
    stat_cache_updated_children(config->cache, path, 0, &gerr);
    if (gerr) {
        return processed_gerror("dav_rmdir: ", path, &gerr);
    }

    return 0;
}

static int dav_mkdir(const char *path, mode_t mode) {
    struct fusedav_config *config = fuse_get_context()->private_data;
    struct stat_cache_value value;
    char fn[PATH_MAX];
    GError *gerr = NULL;
    long response_code = 500; // seed it as bad so we can enter the loop
    CURLcode res = CURLE_OK;

    BUMP(dav_mkdir);

    log_print(LOG_INFO, SECTION_FUSEDAV_DIR, "CALLBACK: dav_mkdir(%s, %04o)", path, mode);

    if (use_saint_mode()) {
        log_print(LOG_ERR, SECTION_FUSEDAV_DIR, "dav_mkdir(%s): already in saint mode", path);
        return -ENETDOWN;
    }

    snprintf(fn, sizeof(fn), "%s/", path);

    for (int idx = 0; idx < num_filesystem_server_nodes && (res != CURLE_OK || response_code >= 500); idx++) {
        CURL *session;
        struct curl_slist *slist = NULL;
        bool new_resolve_list;

        if (idx == 0) new_resolve_list = false;
        else new_resolve_list = true;

        slist = NULL;

        if (!(session = session_request_init(fn, NULL, false, new_resolve_list))) {
            log_print(LOG_ERR, SECTION_FUSEDAV_DIR, "dav_mkdir(%s): failed to get session", path);
            return -ENETDOWN;
        }

        curl_easy_setopt(session, CURLOPT_CUSTOMREQUEST, "MKCOL");

        slist = enhanced_logging(slist, LOG_INFO, SECTION_FUSEDAV_DIR, "dav_mkdir: %s", path);
        if (slist) curl_easy_setopt(session, CURLOPT_HTTPHEADER, slist);

        res = curl_easy_perform(session);
        if(res == CURLE_OK) {
            curl_easy_getinfo(session, CURLINFO_RESPONSE_CODE, &response_code);
        }
        if (slist) curl_slist_free_all(slist);

        log_filesystem_nodes("dav_mkdir", res, response_code, idx, path);
    }

    if (res != CURLE_OK || response_code >= 500) {
        log_print(LOG_ERR, SECTION_FUSEDAV_DIR, "dav_mkdir(%s): MKCOL failed: %s", path, curl_easy_strerror(res));
        return -ENETDOWN;
    }

    // Zero-out structure; some fields we don't populate but want to be 0, e.g. st_atim.tv_nsec
    memset(&value, 0, sizeof(struct stat_cache_value));

    // Populate stat cache.
    // is_dir = true; fd = -1 (not a regular file)
    fill_stat_generic(&(value.st), mode, true, -1, &gerr);
    if (!gerr) {
        stat_cache_value_set(config->cache, path, &value, &gerr);
    }

    if (gerr) {
        return processed_gerror("dav_mkdir: ", path, &gerr);
    }

    return 0;
}

// REVIEW! JERRY! Check that this wasn't broken
static int dav_rename(const char *from, const char *to) {
    struct fusedav_config *config = fuse_get_context()->private_data;
    GError *gerr = NULL;
    int server_ret = -EIO;
    int local_ret = -EIO;
    int fd = -1;
    struct stat st;
    char fn[PATH_MAX];
    struct stat_cache_value *entry = NULL;
    long response_code = 500; // seed it as bad so we can enter the loop
    CURLcode res = CURLE_OK;

    BUMP(dav_rename);

    // TODO: PUNT on doing use_saint_mode here for now.

    assert(from);
    assert(to);

    log_print(LOG_INFO, SECTION_FUSEDAV_FILE, "CALLBACK: dav_rename(%s, %s)", from, to);

    get_stat(from, &st, &gerr);
    if (gerr) {
        server_ret = processed_gerror("dav_rmdir: ", from, &gerr);
        goto finish;
    }

    if (S_ISDIR(st.st_mode)) {
        snprintf(fn, sizeof(fn), "%s/", from);
        from = fn;
    }

    // JB See below about silent failures
    for (int idx = 0; idx < num_filesystem_server_nodes && (res != CURLE_OK || response_code >= 500); idx++) {
        CURL *session;
        struct curl_slist *slist = NULL;
        char *header = NULL;
        char *escaped_to;
        bool new_resolve_list;

        if (idx == 0) new_resolve_list = false;
        else new_resolve_list = true;

        slist = NULL;

        if (!(session = session_request_init(from, NULL, false, new_resolve_list))) {
            log_print(LOG_ERR, SECTION_FUSEDAV_FILE, "dav_rename: failed to get session for %d:%s", fd, from);
            goto finish;
        }

        curl_easy_setopt(session, CURLOPT_CUSTOMREQUEST, "MOVE");

        // Add the destination header.
        // @TODO: Better error handling on failure.
        escaped_to = escape_except_slashes(session, to);
        asprintf(&header, "Destination: %s%s", get_base_url(), escaped_to);
        curl_free(escaped_to);
        escaped_to = NULL;
        slist = curl_slist_append(slist, header);
        free(header);
        header = NULL;

        slist = enhanced_logging(slist, LOG_INFO, SECTION_FUSEDAV_FILE, "dav_rename: %s to %s", from, to);

        curl_easy_setopt(session, CURLOPT_HTTPHEADER, slist);

        // Do the server side move

        res = curl_easy_perform(session);
        if(res == CURLE_OK) {
            curl_easy_getinfo(session, CURLINFO_RESPONSE_CODE, &response_code);
        }
        if (slist) curl_slist_free_all(slist);

        log_filesystem_nodes("dav_rename", res, response_code, idx, to);
    }

    /* move:
     * succeeds: mv 'from' to 'to', delete 'from'
     * fails with 404: may be doing the move on an open file, so this may be ok
     *                 mv 'from' to 'to', delete 'from'
     * fails, not 404: error, exit
     */
    if (response_code == 404) {
        // We allow silent failures because we might have done a rename before the
        // file ever made it to the server
        log_print(LOG_INFO, SECTION_FUSEDAV_FILE, "dav_rename: MOVE failed with 404, recoverable: %s", curl_easy_strerror(res));
        // Allow the error code -EIO to percolate down, we need to pass the local move
    }

    if(res != CURLE_OK || response_code >= 500) {
        log_print(LOG_ERR, SECTION_FUSEDAV_FILE, "dav_rename: MOVE failed: %s", curl_easy_strerror(res));
        goto finish;
    }
    else {
        server_ret = 0;
    }

    /* If the server_side failed, then both the stat_cache and filecache moves need to succeed */
    entry = stat_cache_value_get(config->cache, from, true, &gerr);
    if (gerr) {
        local_ret = processed_gerror("dav_rename: ", from, &gerr);
        goto finish;
    }

    // No entry means that the "from" file doesn't really exist, at least it has no cache presence
    if (entry == NULL) {
        local_ret = -ENOENT;
        goto finish;
    }

    log_print(LOG_DEBUG, SECTION_FUSEDAV_FILE, "dav_rename: stat cache moving source entry to destination %d:%s", fd, to);
    stat_cache_value_set(config->cache, to, entry, &gerr);
    if (gerr) {
        local_ret = processed_gerror("dav_rename: ", to, &gerr);
        log_print(LOG_NOTICE, SECTION_FUSEDAV_FILE, "dav_rename: failed stat cache moving source entry to destination %d:%s", fd, to);
        // If the local stat_cache move fails, leave the filecache alone so we don't get mixed state
        goto finish;
    }

    stat_cache_delete(config->cache, from, &gerr);
    if (gerr) {
        local_ret = processed_gerror("dav_rename: ", from, &gerr);
        goto finish;
    }

    filecache_pdata_move(config->cache, from, to, &gerr);
    if (gerr) {
        GError *tmpgerr = NULL;
        filecache_delete(config->cache, to, true, &tmpgerr);
        if (tmpgerr) {
            // Don't propagate but do log
            log_print(LOG_NOTICE, SECTION_FUSEDAV_FILE, "dav_rename: filecache_delete failed %d:%s -- %s", fd, to, tmpgerr->message);
            g_clear_error(&tmpgerr);
        }
        local_ret = processed_gerror("dav_rename: ", to, &gerr);
        goto finish;
    }
    local_ret = 0;

finish:

    log_print(LOG_DEBUG, SECTION_FUSEDAV_FILE, "Exiting: dav_rename(%s, %s); %d %d", from, to, server_ret, local_ret);

    free(entry);

    // if either the server move or the local move succeed, we return success
    if (server_ret == 0 || local_ret == 0)
        return 0;
    return server_ret; // error from either get_stat or curl_easy_getinfo
}

static int dav_release(const char *path, __unused struct fuse_file_info *info) {
    struct fusedav_config *config = fuse_get_context()->private_data;
    GError *gerr = NULL;
    GError *gerr2 = NULL;
    int ret = 0;

    BUMP(dav_release);

    log_print(LOG_INFO, SECTION_FUSEDAV_FILE, "CALLBACK: dav_release: release(%s)", path ? path : "null path");

    // path might be NULL if we are accessing a bare file descriptor. This is not an error.
    // We still need to close the file.

    if (path != NULL) {
        bool wrote_data = filecache_sync(config->cache, path, info, true, &gerr);

        // If we didn't write data, we either got an error, which we handle below, or there is no error,
        // so just fall through (not writable, not modified are examples)
        if (wrote_data && !gerr) { // I don't think we can exit with gerr and still write data, but just to be safe...
            struct stat_cache_value value;
            int fd = filecache_fd(info);
            // Zero-out structure; some fields we don't populate but want to be 0, e.g. st_atim.tv_nsec
            memset(&value, 0, sizeof(struct stat_cache_value));
            // mode = 0 (unspecified), is_dir = false; fd to get size
            fill_stat_generic(&(value.st), 0, false, fd, &gerr2);
            if (!gerr2) {
                stat_cache_value_set(config->cache, path, &value, &gerr2);
            }
            if (gerr2) {
                ret = -gerr2->code;
                processed_gerror("dav_release: ", path, &gerr2);
            }
        }
    }

    // Call this even if path is NULL
    filecache_close(info, &gerr2);
    if (gerr2) {
        // Log but do not exit on error
        processed_gerror("dav_release: ", path, &gerr2);
    }

    // If path is NULL, gerr will also be NULL since we don't call filecache_sync.
    if (gerr) {
        // NB. We considered not removing the file from the local caches on cURL error, but
        // if we do that we can conceivably have a file in our local cache and no file on
        // the server. On the next open, we will get the dreaded unexpected 404. Also, we
        // would set up cache incoherence between this binding and the others.
        // For now, go to forensic haven even on cURL error (ENETDOWN).
        GError *subgerr = NULL;
        bool do_unlink = false;
        struct stat_cache_value *value;
        size_t st_size;

        log_print(LOG_WARNING, SECTION_FUSEDAV_FILE, "dav_release: invoking forensic_haven on %s", path);
        // The idea here is that if we fail the PUT, we want to clean up the detritus
        // left in the filecache and statcache.
        // However, we will also do this cleanup on other gErrors in filecache_sync, which include:
        // no sdata
        // ldb error on pdata_get, or null return (not in filecache)
        // error on lseek prior to PUT (why do we do the lseek?)
        // error on PUT
        // ldb error on pdata_set

        value = stat_cache_value_get(config->cache, path, true, &subgerr);
        if (subgerr) {
            log_print(LOG_NOTICE, SECTION_FUSEDAV_FILE, "dav_release: error on stat_cache_value_get on %s", path);
            // display the error but don't return it
            processed_gerror("dav_release:", path, &subgerr);
            // processed_gerror will clear the error for reuse below
        }

        // value == NULL means not found in statcache. This is not an error from the
        // point of view of the statcache, so double check here before dereferencing
        if (value == NULL) {
            log_print(LOG_NOTICE, SECTION_FUSEDAV_FILE, "dav_release: pdata NULL on %s", path);
            st_size = 0; // interpret 0 as unknown size
        }
        else {
            st_size = value->st.st_size;
        }
        free(value);

        // sdata now carries a has_error field. If we detect an error on the file,
        // we carry it forward. filecache_sync will detect and cause gerr if it sees an error.
        // Move to forensic haven
        filecache_forensic_haven(config->cache_path, config->cache, path, st_size, &subgerr);
        if (subgerr) {
            log_print(LOG_NOTICE, SECTION_FUSEDAV_FILE, "dav_release: failed filecache_forensic_haven on %s", path);
            // display the error but don't return it
            processed_gerror("dav_release:", path, &subgerr);
            // processed_gerror will clear the error for reuse below
        }
        log_print(LOG_INFO, SECTION_FUSEDAV_FILE,
            "dav_release: error on file \'%s\'; removing from %sfile and stat caches",
            path, do_unlink ? "server and " : "");
        // This will delete from filecache and statcache; depending on do_unlink might also remove from server
        // Currently, do_unlink is always false; we have taken the decision to never remove from server
        common_unlink(path, do_unlink, &subgerr);
        if (subgerr) {
            // display the error, but don't return it ...
            log_print(LOG_NOTICE, SECTION_FUSEDAV_FILE, "dav_release: failed common_unlink on %s", path);
            processed_gerror("dav_release: ", path, &subgerr);
        }
        log_print(LOG_NOTICE, SECTION_FUSEDAV_FILE, "dav_release: failed filecache_sync on %s", path);
        // return the original error
        return processed_gerror("dav_release:", path, &gerr);
    }

    log_print(LOG_DEBUG, SECTION_FUSEDAV_FILE, "END: dav_release: release(%s)", (path ? path : "null path"));

    return ret;
}

static int dav_fsync(const char *path, __unused int isdatasync, struct fuse_file_info *info) {
    struct fusedav_config *config = fuse_get_context()->private_data;
    struct stat_cache_value value;
    GError *gerr = NULL;
    bool wrote_data;

    BUMP(dav_fsync);

    // Zero-out structure; some fields we don't populate but want to be 0, e.g. st_atim.tv_nsec
    memset(&value, 0, sizeof(struct stat_cache_value));

    log_print(LOG_INFO, SECTION_FUSEDAV_FILE, "CALLBACK: dav_fsync(%s)", path ? path : "null path");

    // If path is NULL because we are accessing a bare file descriptor,
    // let filecache_sync handle it since we need to get the file
    // descriptor there
    wrote_data = filecache_sync(config->cache, path, info, true, &gerr);
    if (gerr) {
        return processed_gerror("dav_fsync: ", path, &gerr);
    }

    if (wrote_data) {
        int fd;
        fd = filecache_fd(info);
        // mode = 0 (unspecified), is_dir = false; fd to get size
        fill_stat_generic(&(value.st), 0, false, fd, &gerr);
        if (!gerr) {
            stat_cache_value_set(config->cache, path, &value, &gerr);
        }
        if (gerr) {
            return processed_gerror("dav_fsync: ", path, &gerr);
        }
    }

    return 0;
}

static int dav_flush(const char *path, struct fuse_file_info *info) {
    struct fusedav_config *config = fuse_get_context()->private_data;
    GError *gerr = NULL;

    BUMP(dav_flush);

    log_print(LOG_INFO, SECTION_FUSEDAV_FILE, "CALLBACK: dav_flush(%s)", path ? path : "null path");

    // path might be NULL because we are accessing a bare file descriptor,
    if (path != NULL) {
        bool wrote_data;
        // Zero-out structure; some fields we don't populate but want to be 0, e.g. st_atim.tv_nsec
        struct stat_cache_value value;
        memset(&value, 0, sizeof(struct stat_cache_value));

        wrote_data = filecache_sync(config->cache, path, info, true, &gerr);
        if (gerr) {
            return processed_gerror("dav_flush: ", path, &gerr);
        }

        if (wrote_data) {
            int fd;
            fd = filecache_fd(info);
            // mode = 0 (unspecified), is_dir = false; fd to get size
            fill_stat_generic(&(value.st), 0, false, fd, &gerr);
            if (!gerr) {
                stat_cache_value_set(config->cache, path, &value, &gerr);
            }
            if (gerr) {
                return processed_gerror("dav_flush: ", path, &gerr);
            }
        }
    }

    return 0;
}

static int dav_mknod(const char *path, mode_t mode, __unused dev_t rdev) {
    struct fusedav_config *config = fuse_get_context()->private_data;
    struct stat_cache_value value;
    GError *gerr = NULL;

    BUMP(dav_mknod);

    // Zero-out structure; some fields we don't populate but want to be 0, e.g. st_atim.tv_nsec
    memset(&value, 0, sizeof(struct stat_cache_value));

    log_print(LOG_INFO, SECTION_FUSEDAV_DIR, "CALLBACK: dav_mknod(%s)", path);

    // Prepopulate stat cache.
    // is_dir = false, fd = -1, can't set size
    fill_stat_generic(&(value.st), mode, false, -1, &gerr);
    if (!gerr) {
        stat_cache_value_set(config->cache, path, &value, &gerr);
    }
    if (gerr) {
        return processed_gerror("dav_mknod: ", path, &gerr);
    }

    return 0;
}

static void do_open(const char *path, struct fuse_file_info *info, GError **gerr) {
    struct fusedav_config *config = fuse_get_context()->private_data;
    GError *tmpgerr = NULL;

    assert(info);

    filecache_open(config->cache_path, config->cache, path, info, &tmpgerr);
    if (tmpgerr) {
        g_propagate_prefixed_error(gerr, tmpgerr, "do_open: ");
        return;
    }

    log_print(LOG_DEBUG, SECTION_FUSEDAV_FILE, "do_open: after filecache_open");

    return;
}

static int dav_open(const char *path, struct fuse_file_info *info) {
    GError *gerr = NULL;
    BUMP(dav_open);
    log_print(LOG_INFO, SECTION_ENHANCED, "dav_open: fusedav.opens:1|c");

    // There are circumstances where we read a write-only file, so if write-only
    // is specified, change to read-write. Otherwise, a read on that file will
    // return an EBADF.
    if (info->flags & O_WRONLY) {
        info->flags &= ~O_WRONLY;
        info->flags |= O_RDWR;
    }

    log_print(LOG_INFO, SECTION_FUSEDAV_FILE, "CALLBACK: dav_open: open(%s, %x, trunc=%x)", path, info->flags, info->flags & O_TRUNC);
    do_open(path, info, &gerr);
    if (gerr) {
        int ret = processed_gerror("dav_open: ", path, &gerr);
        log_print(LOG_DEBUG, SECTION_FUSEDAV_FILE, "CALLBACK: dav_open: returns %d", ret);
        return ret;
    }

    // Update stat cache value to reset the file size to 0 on trunc.
    if (info->flags & O_TRUNC) {
        struct stat_cache_value value;
        struct fusedav_config *config = fuse_get_context()->private_data;
        int fd = filecache_fd(info);

        // Zero-out structure; some fields we don't populate but want to be 0, e.g. st_atim.tv_nsec
        memset(&value, 0, sizeof(struct stat_cache_value));

        // mode = 0 (unspecified), is_dir = false; fd to get size
        fill_stat_generic(&(value.st), 0, false, fd, &gerr);
        if (!gerr) {
            stat_cache_value_set(config->cache, path, &value, &gerr);
        }
        if (gerr) {
            return processed_gerror("dav_open: ", path, &gerr);
        }
        log_print(LOG_DEBUG, SECTION_FUSEDAV_FILE, "dav_open: fill_stat_generic on O_TRUNC: %d--%s", value.st.st_size, path);
    }

    return 0;
}

static int dav_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *info) {
    ssize_t bytes_read;
    GError *gerr = NULL;

    BUMP(dav_read);
    log_print(LOG_INFO, SECTION_ENHANCED, "dav_read: fusedav.reads:1|c");

    // We might get a null path if we are reading from a bare file descriptor
    // (we have unlinked the path but kept the file descriptor open)
    // In this case we continue to do the read.
    log_print(LOG_INFO, SECTION_FUSEDAV_IO, "CALLBACK: dav_read(%s, %lu+%lu)", path ? path : "null path", (unsigned long) offset, (unsigned long) size);

    bytes_read = filecache_read(info, buf, size, offset, &gerr);
    if (gerr) {
        return processed_gerror("dav_read: ", path, &gerr);
    }

    if (bytes_read < 0) {
        log_print(LOG_ERR, SECTION_FUSEDAV_IO, "dav_read: filecache_read returns error");
    }

    return bytes_read;
}

static bool file_too_big(off_t fsz, off_t maxsz) {
    // NB. During tests transferring a file that was too large, the command line sftp
    // client recognized the write error, and the subsequent flush error, with the
    // following messages:
    // Couldn't write to remote file "/srv/bindings/df2b48681f46459ba91ddb48077f7a89/files/f_000c84": Failure
    // Couldn't close file: Failure
    //
    // filezilla recognized the errors, but went into a seemingly infinite loop retrying the file.
    // If killed at the filezilla client, if in the middle of a transfer, a partial file was
    // left on the server.

    // convert maxsz to bytes so we can get a precise comparison
    // We need to know the difference between a file which is exactly,
    // e.g. 256MB, and one that is some bytes larger than that.
    maxsz *= (1024 * 1024);
    log_print(LOG_DEBUG, SECTION_FUSEDAV_IO, "dav_write: fsz (%lu); maxsz (%lu)", fsz, maxsz);
    if (fsz > maxsz) {
        log_print(LOG_ERR, SECTION_FUSEDAV_IO, "dav_write: file size (%lu) is greater than max allowed (%lu)", fsz, maxsz);
        return true;
    }
    return false;
}

static int dav_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *info) {
    struct fusedav_config *config = fuse_get_context()->private_data;
    GError *gerr = NULL;
    ssize_t bytes_written;
    struct stat_cache_value value;

    BUMP(dav_write);
    log_print(LOG_INFO, SECTION_ENHANCED, "dav_write: fusedav.writes:1|c");

    // We might get a null path if we are writing to a bare file descriptor
    // (we have unlinked the path but kept the file descriptor open)
    // In this case we continue to do the write, but we skip the sync below

    log_print(LOG_INFO, SECTION_FUSEDAV_IO, "CALLBACK: dav_write(%s, %lu+%lu)", path ? path : "null path", (unsigned long) offset, (unsigned long) size);

    bytes_written = filecache_write(info, buf, size, offset, &gerr);
    if (gerr) {
        return processed_gerror("dav_write: ", path, &gerr);
    }

    if (bytes_written < 0) {
        log_print(LOG_ERR, SECTION_FUSEDAV_IO, "dav_write: filecache_write returns error");
        return bytes_written;
    }

    if (path != NULL) {
        int fd;
        filecache_sync(config->cache, path, info, false, &gerr);
        if (gerr) {
            return processed_gerror("dav_write: ", path, &gerr);
        }

        // Zero-out structure; some fields we don't populate but want to be 0, e.g. st_atim.tv_nsec
        memset(&value, 0, sizeof(struct stat_cache_value));

        fd = filecache_fd(info);
        // mode = 0 (unspecified), is_dir = false; fd to get size
        fill_stat_generic(&(value.st), 0, false, fd, &gerr);
        if (gerr) {
            return processed_gerror("dav_write: ", path, &gerr);
        }
        else {
            if (file_too_big(value.st.st_size, config->max_file_size)) {
                // The file will now carry along with it the fact that there has been an error.
                // Eventually, this will send the file to forensic haven
                filecache_set_error(info, EFBIG);
                return (-EFBIG);
            }
            stat_cache_value_set(config->cache, path, &value, &gerr);
            if (gerr) {
                return processed_gerror("dav_write: ", path, &gerr);
            }
        }
    }

   return bytes_written;
}

static int dav_ftruncate(const char *path, off_t size, struct fuse_file_info *info) {
    struct fusedav_config *config = fuse_get_context()->private_data;
    struct stat_cache_value value;
    GError *gerr = NULL;
    int fd;

    BUMP(dav_ftruncate);

    // Zero-out structure; some fields we don't populate but want to be 0, e.g. st_atim.tv_nsec
    memset(&value, 0, sizeof(struct stat_cache_value));

    log_print(LOG_INFO, SECTION_FUSEDAV_FILE, "CALLBACK: dav_ftruncate(%s, %lu)", path ? path : "null path", (unsigned long) size);

    filecache_truncate(info, size, &gerr);
    if (gerr) {
        return processed_gerror("dav_ftruncate: ", path, &gerr);
    }

    // Let sync handle a NULL path
    filecache_sync(config->cache, path, info, false, &gerr);
    if (gerr) {
        return processed_gerror("dav_ftruncate: ", path, &gerr);
    }

    fd = filecache_fd(info);
    // mode = 0 (unspecified), is_dir = false; fd to get size
    fill_stat_generic(&(value.st), 0, false, fd, &gerr);
    if (!gerr) {
        stat_cache_value_set(config->cache, path, &value, &gerr);
    }

    if (gerr) {
        return processed_gerror("dav_ftruncate: ", path, &gerr);
    }

    log_print(LOG_DEBUG, SECTION_FUSEDAV_FILE, "dav_ftruncate: returning");
    return 0;
}

static int dav_utimens(__unused const char *path, const struct timespec tv[2]) {
    struct fusedav_config *config = fuse_get_context()->private_data;
    struct stat_cache_value *value;
    GError *gerr = NULL;
    int ret = 0;

    BUMP(dav_utimens);
    log_print(LOG_INFO, SECTION_FUSEDAV_DEFAULT, "CALLBACK: dav_utimens(%s) %lu:%lu", path, tv[0].tv_sec, tv[1].tv_sec);

    // Get the entry from the stat cache. If there's an error or the
    // entry is missing, punt.
    value = stat_cache_value_get(config->cache, path, true, &gerr);
    if (gerr) {
        log_print(LOG_NOTICE, SECTION_FUSEDAV_FILE, "dav_utimens: error on stat_cache_value_get on %s", path);
        return processed_gerror("dav_utimens:", path, &gerr);
    }

    // value == NULL means not found in statcache. This is not an error from the
    // point of view of the statcache, so double check here before dereferencing
    if (value == NULL) {
        log_print(LOG_NOTICE, SECTION_FUSEDAV_FILE, "dav_utimens: pdata NULL on %s", path);
        return -ENOENT;
    }

    // Set the appropriate times. tv[0] is the last access, use it for access
    // tv[1] is last modified; use for ctime and mtime. This is not quite correct
    // for ctime (when file attributes changed), but since we don't chown or chmod,
    // having them the same is probably the most appropriate.
    // Then return to the stat cache
    value->st.st_atime = tv[0].tv_sec;
    value->st.st_ctime = tv[1].tv_sec;
    value->st.st_mtime = tv[1].tv_sec;
    stat_cache_value_set(config->cache, path, value, &gerr);
    if (gerr) {
        ret = processed_gerror("dav_utimens: ", path, &gerr);
    }

    free(value);
    return ret;
}

static int dav_chmod(__unused const char *path, __unused mode_t mode) {
    BUMP(dav_chmod);
    log_print(LOG_INFO, SECTION_FUSEDAV_DEFAULT, "CALLBACK: dav_chmod(%s, %04o)", path, mode);
    return 0;
}

static int dav_create(const char *path, mode_t mode, struct fuse_file_info *info) {
    struct fusedav_config *config = fuse_get_context()->private_data;
    struct stat_cache_value value;
    GError *gerr = NULL;
    int fd;

    BUMP(dav_create);
    log_print(LOG_INFO, SECTION_ENHANCED, "dav_create: fusedav.creates:1|c");


    log_print(LOG_INFO, SECTION_FUSEDAV_FILE, "CALLBACK: dav_create(%s, %04o)", path, mode);

    info->flags |= O_CREAT | O_TRUNC;
    do_open(path, info, &gerr);

    if (gerr) {
        return processed_gerror("dav_create: ", path, &gerr);
    }

    // @TODO: Perform a chmod here based on mode.

    filecache_sync(config->cache, path, info, false, &gerr);
    if (gerr) {
        return processed_gerror("dav_create: ", path, &gerr);
    }

    // Zero-out structure; some fields we don't populate but want to be 0, e.g. st_atim.tv_nsec
    memset(&value, 0, sizeof(struct stat_cache_value));

    fd = filecache_fd(info);
    // mode = 0 (unspecified), is_dir = false; fd to get size
    fill_stat_generic(&(value.st), 0, false, fd, &gerr);
    if (!gerr) {
        stat_cache_value_set(config->cache, path, &value, &gerr);
    }
    if (gerr) {
        return processed_gerror("dav_create: ", path, &gerr);
    }

    log_print(LOG_DEBUG, SECTION_FUSEDAV_FILE, "Done: create()");

    return 0;
}

static int dav_chown(__unused const char *path, __unused uid_t u, __unused gid_t g) {
    BUMP(dav_chown);

    return 0;
}

/* We need to accommodate the pattern
 * open
 * unlink
 * read/write
 * close
 *
 * We set hard_remove and flag_nullpath_ok, which will refrain from
 * creating fuse_hidden files, but will return NULL for path to dav functions,
 * e.g. read and write. We need to handle this.
 * The list of operations which need to handle NULL paths is:
 *   * read, write, flush, release, fsync, readdir, releasedir,
     * fsyncdir, ftruncate, fgetattr and lock
 * We don't implement releasedir, fsyncdir, and lock.
 */

struct fuse_operations dav_oper = {
    .fgetattr     = dav_fgetattr,
    .getattr     = dav_getattr,
    .readdir     = dav_readdir,
    .mknod       = dav_mknod,
    .create      = dav_create,
    .mkdir       = dav_mkdir,
    .unlink      = dav_unlink,
    .rmdir       = dav_rmdir,
    .rename      = dav_rename,
    .chmod       = dav_chmod,
    .chown       = dav_chown,
    .ftruncate    = dav_ftruncate,
    .utimens     = dav_utimens,
    .open        = dav_open,
    .read        = dav_read,
    .write       = dav_write,
    .release     = dav_release,
    .fsync       = dav_fsync,
    .flush       = dav_flush,
    .flag_nullpath_ok = 1,
};

static int config_privileges(struct fusedav_config *config) {
    if (config->run_as_gid != 0) {
        struct group *g = getgrnam(config->run_as_gid);
        if (setegid(g->gr_gid) < 0) {
            log_print(LOG_ERR, SECTION_CONFIG_DEFAULT, "Can't drop gid to %d.", g->gr_gid);
            return -1;
        }
        log_print(LOG_DEBUG, SECTION_CONFIG_DEFAULT, "Set egid to %d.", g->gr_gid);
    }

    if (config->run_as_uid != 0) {
        struct passwd *u = getpwnam(config->run_as_uid);

        // If there's no explict group set, use the user's primary gid.
        if (config->run_as_gid == 0) {
            if (setegid(u->pw_gid) < 0) {
                log_print(LOG_ERR, SECTION_CONFIG_DEFAULT, "Can't drop git to %d (which is uid %d's primary gid).", u->pw_gid, u->pw_uid);
                return -1;
            }
            log_print(LOG_DEBUG, SECTION_CONFIG_DEFAULT, "Set egid to %d (which is uid %d's primary gid).", u->pw_gid, u->pw_uid);
        }

        if (seteuid(u->pw_uid) < 0) {
            log_print(LOG_ERR, SECTION_CONFIG_DEFAULT, "Can't drop uid to %d.", u->pw_uid);
            return -1;
        }
        log_print(LOG_DEBUG, SECTION_CONFIG_DEFAULT, "Set euid to %d.", u->pw_uid);
    }

    // Ensure the core is still dumpable.
    prctl(PR_SET_DUMPABLE, 1);

    return 0;
}

static void *cache_cleanup(void *ptr) {
    struct fusedav_config *config = (struct fusedav_config *)ptr;
    GError *gerr = NULL;
    bool first = true;

    log_print(LOG_DEBUG, SECTION_FUSEDAV_DEFAULT, "enter cache_cleanup");

    while (true) {
        // We would like to do cleanup on startup, to resolve issues
        // from errant stat and file caches
        filecache_cleanup(config->cache, config->cache_path, first, &gerr);
        if (gerr) {
            processed_gerror("cache_cleanup: ", config->cache_path, &gerr);
        }
        first = false;
        stat_cache_prune(config->cache);
        if ((sleep(CACHE_CLEANUP_INTERVAL)) != 0) {
            log_print(LOG_WARNING, SECTION_FUSEDAV_DEFAULT, "cache_cleanup: sleep interrupted; exiting ...");
            return NULL;
        }
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct fusedav_config config;
    struct fuse_chan *ch = NULL;
    char *mountpoint = NULL;
    GError *gerr = NULL;
    pthread_t cache_cleanup_thread;
    pthread_t error_injection_thread;
    int ret = -1;

    // Initialize the statistics and configuration.
    memset(&stats, 0, sizeof(struct statistics));
    memset(&config, 0, sizeof(config));

    setup_signal_handlers(&gerr);
    if (gerr) goto finish;

    configure_fusedav(&config, &args, &mountpoint, &gerr);
    if (gerr) goto finish;

    mask = umask(0);
    umask(mask);

    if (!(ch = fuse_mount(mountpoint, &args))) {
        log_print(LOG_CRIT, SECTION_FUSEDAV_MAIN, "Failed to mount FUSE file system.");
        goto finish;
    }
    log_print(LOG_DEBUG, SECTION_FUSEDAV_MAIN, "Mounted the FUSE file system.");

    if (!(fuse = fuse_new(ch, &args, &dav_oper, sizeof(dav_oper), &config))) {
        log_print(LOG_CRIT, SECTION_FUSEDAV_MAIN, "Failed to create FUSE object.");
        goto finish;
    }
    log_print(LOG_DEBUG, SECTION_FUSEDAV_MAIN, "Created the FUSE object.");

    // If in development you need to run in the foreground for debugging, set nodaemon
    // We also do this for our test_dav, so we can auto-clean up processes after we run the tests
    if (config.nodaemon) {
        log_print(LOG_DEBUG, SECTION_FUSEDAV_MAIN, "Running in foreground (skipping daemonization).");
    }
    else {
        log_print(LOG_DEBUG, SECTION_FUSEDAV_MAIN, "Attempting to daemonize.");
        if (fuse_daemonize(/* 0 means daemonize */ 0) < 0) {
            log_print(LOG_CRIT, SECTION_FUSEDAV_MAIN, "Failed to daemonize.");
            goto finish;
        }
    }

    log_print(LOG_DEBUG, SECTION_FUSEDAV_MAIN, "Attempting to configure privileges.");
    if (config_privileges(&config) < 0) {
        log_print(LOG_CRIT, SECTION_FUSEDAV_MAIN, "Failed to configure privileges.");
        goto finish;
    }

    // Error injection mechanism. Should only be run during development.
    // It should only be triggered by running 'make INJECT_ERRORS=1' during build. So under
    // normal circumstances, injecting_errors is #define'd to 'false'
    if (injecting_errors) {
        if (pthread_create(&error_injection_thread, NULL, inject_error_mechanism, NULL)) {
            log_print(LOG_INFO, SECTION_FUSEDAV_MAIN, "Failed to create error injection thread.");
            goto finish;
        }
        // Sleep some amount of time to ensure that inject_error_mechanism gets a chance to get called
        // before the first inject_error call is made and segv's because the list hasn't been set up yet.
        sleep(10);
    }

    // Ensure directory exists for file content cache.
    filecache_init(config.cache_path, &gerr);
    if (gerr) {
        log_print(LOG_CRIT, SECTION_FUSEDAV_MAIN, "main: %s.", gerr->message);
        goto finish;
    }
    log_print(LOG_DEBUG, SECTION_FUSEDAV_MAIN, "Opened ldb file cache.");

    // Open the stat cache.
    stat_cache_open(&config.cache, &config.cache_supplemental, config.cache_path, &gerr);
    if (gerr) {
        processed_gerror("main: ", config.cache_path, &gerr);
        config.cache = NULL;
        goto finish;
    }
    log_print(LOG_DEBUG, SECTION_FUSEDAV_MAIN, "Opened stat cache.");

    if (pthread_create(&cache_cleanup_thread, NULL, cache_cleanup, &config)) {
        log_print(LOG_CRIT, SECTION_FUSEDAV_MAIN, "Failed to create cache cleanup thread.");
        goto finish;
    }

    log_print(LOG_NOTICE, SECTION_FUSEDAV_MAIN, "Startup complete. Entering main FUSE loop.");

    if (config.singlethread) {
        log_print(LOG_DEBUG, SECTION_FUSEDAV_MAIN, "...singlethreaded");
        if (fuse_loop(fuse) < 0) {
            log_print(LOG_CRIT, SECTION_FUSEDAV_MAIN, "Error occurred while trying to enter single-threaded FUSE loop.");
            goto finish;
        }
    }
    else {
        log_print(LOG_DEBUG, SECTION_FUSEDAV_MAIN, "...multi-threaded");
        if (fuse_loop_mt(fuse) < 0) {
            log_print(LOG_CRIT, SECTION_FUSEDAV_MAIN, "Error occurred while trying to enter multi-threaded FUSE loop.");
            goto finish;
        }
    }

    ret = 0;

    log_print(LOG_NOTICE, SECTION_FUSEDAV_MAIN, "Left main FUSE loop. Shutting down.");

finish:
    if (gerr) {
        processed_gerror("main: ", "main", &gerr);
    }

    dump_stats(false, config.cache_path); // false means output to file, not to log

    if (ch != NULL) {
        log_print(LOG_DEBUG, SECTION_FUSEDAV_MAIN, "Unmounting: %s", mountpoint);
        fuse_unmount(mountpoint, ch);
    }

    if (mountpoint != NULL) {
        free(mountpoint);
    }

    log_print(LOG_NOTICE, SECTION_FUSEDAV_MAIN, "Unmounted.");

    if (fuse) {
        fuse_destroy(fuse);
    }
    log_print(LOG_DEBUG, SECTION_FUSEDAV_MAIN, "Destroyed FUSE object.");

    fuse_opt_free_args(&args);
    log_print(LOG_DEBUG, SECTION_FUSEDAV_MAIN, "Freed arguments.");

    session_config_free();
    log_print(LOG_DEBUG, SECTION_FUSEDAV_MAIN, "Cleaned up session system.");

    // We don't capture any errors from stat_cache_close
    stat_cache_close(config.cache, config.cache_supplemental);

    log_print(LOG_NOTICE, SECTION_FUSEDAV_MAIN, "Shutdown was successful. Exiting.");

    // log statements getting lost going to journal. See if delay here
    // allows journal to catch up.
    sleep(5);

    return ret;
}
