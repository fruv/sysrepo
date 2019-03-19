/**
 * @file replay.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief notification replay routines
 *
 * @copyright
 * Copyright (c) 2019 CESNET, z.s.p.o.
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
 */
#include "common.h"

#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <assert.h>

static sr_error_info_t *
sr_write(int fd, const void *buf, size_t count)
{
    sr_error_info_t *err_info = NULL;
    ssize_t ret;
    size_t written;

    written = 0;
    do {
        errno = 0;
        ret = write(fd, ((const char *)buf) + written, count - written);
        if ((ret == -1) || ((ret < (signed)(count - written)) && errno && (errno != EINTR))) {
            SR_ERRINFO_SYSERRNO(&err_info, "write");
            return err_info;
        }

        written += ret;
    } while (written < count);

    return NULL;
}

static sr_error_info_t *
sr_read(int fd, void *buf, size_t count)
{
    sr_error_info_t *err_info = NULL;
    ssize_t ret;
    size_t have_read;

    have_read = 0;
    do {
        errno = 0;
        ret = read(fd, ((char *)buf) + have_read, count - have_read);
        if (!ret) {
            /* EOF */
            return NULL;
        }
        if ((ret == -1) || ((ret < (signed)(count - have_read)) && errno && (errno != EINTR))) {
            SR_ERRINFO_SYSERRNO(&err_info, "read");
            return err_info;
        }

        have_read += ret;
    } while (have_read < count);

    return NULL;
}

static sr_error_info_t *
sr_replay_open_file(const char *mod_name, time_t from_ts, time_t to_ts, int flags, int *notif_fd)
{
    sr_error_info_t *err_info = NULL;
    char *path = NULL;
    mode_t perm = SR_FILE_PERM;

    *notif_fd = -1;

    if ((flags & O_CREAT) && (flags & O_EXCL)) {
        /* creating a file, learn module permissions */
        if ((err_info = sr_perm_get(mod_name, NULL, NULL, &perm))) {
            goto cleanup;
        }
    }

    if ((err_info = sr_path_notif_file(mod_name, from_ts, to_ts, &path))) {
        goto cleanup;
    }

    *notif_fd = open(path, flags, perm);
    if (*notif_fd == -1) {
        sr_errinfo_new(&err_info, SR_ERR_SYS, NULL, "Failed to open file \"%s\" (%s).", path, strerror(errno));
        goto cleanup;
    }

    if ((flags & O_CREAT) && (flags & O_EXCL)) {
        SR_LOG_INF("Replay file \"%s\" created.", strrchr(path, '/') + 1);
    }

    /* success */

cleanup:
    free(path);
    return err_info;
}

/*
 * from_ts = 0
 * to_ts = 0
 * - find latest file
 *
 * from_ts =/= 0
 * to_ts = 0
 * - find file possibly containing no-earlier-than from_ts (replay start_time)
 *
 * from_ts =/= 0
 * to_ts =/= 0
 * - find next file
 */
sr_error_info_t *
sr_replay_find_file(const char *mod_name, time_t from_ts, time_t to_ts, time_t *file_from_ts, time_t *file_to_ts)
{
    sr_error_info_t *err_info = NULL;
    DIR *dir = NULL;
    struct dirent *dirent;
    char *dir_path = NULL, *prefix = NULL, *ptr;
    time_t ts1, ts2;
    int pref_len;

    assert((from_ts && to_ts) || (from_ts && !to_ts) || (!from_ts && !to_ts));

    *file_from_ts = 0;
    *file_to_ts = 0;

    if ((err_info = sr_path_notif_dir(&dir_path))) {
        goto cleanup;
    }

    dir = opendir(dir_path);
    if (!dir) {
        sr_errinfo_new(&err_info, SR_ERR_INTERNAL, NULL, "Opening directory \"%s\" failed (%s).", dir_path, strerror(errno));
        goto cleanup;
    }

    /* this is the prefix for all notification files of this module */
    pref_len = asprintf(&prefix, "%s.notif.", mod_name);
    if (pref_len == -1) {
        SR_ERRINFO_MEM(&err_info);
        goto cleanup;
    }

    while ((dirent = readdir(dir))) {
        if (strncmp(dirent->d_name, prefix, pref_len)) {
            continue;
        }

        /* read timestamps */
        errno = 0;
        ts1 = strtoull(dirent->d_name + pref_len, &ptr, 10);
        if (errno || (ptr[0] != '-')) {
            SR_LOG_WRN("Invalid notification file \"%s\" encountered.", dirent->d_name);
            continue;
        }
        ts2 = strtoull(ptr + 1, &ptr, 10);
        if (errno || (ptr[0] != '\0')) {
            SR_LOG_WRN("Invalid notification file \"%s\" encountered.", dirent->d_name);
            continue;
        }

        if (ts1 > ts2) {
            /* what? */
            SR_LOG_WRN("Invalid notification file \"%s\" encountered.", dirent->d_name);
            continue;
        }

        if (from_ts && to_ts) {
            if ((from_ts > ts1) || (to_ts > ts2) || ((from_ts == ts1) && (to_ts == ts2))) {
                /* this file was already processed */
                continue;
            }

            /* we want the next earliest file */
            if ((*file_from_ts && (ts1 >= *file_from_ts)) && (*file_to_ts && ((ts2 >= *file_to_ts)))) {
                continue;
            }
        } else if (from_ts) {
            if (from_ts > ts2) {
                /* there are no notifications of interest in this file */
                continue;
            }

            /* we want the earliest file */
            if ((*file_from_ts && (ts1 >= *file_from_ts)) && (*file_to_ts && ((ts2 >= *file_to_ts)))) {
                continue;
            }
        } else {
            /* we want the latest file */
            if ((*file_from_ts && (ts1 <= *file_from_ts)) && (*file_to_ts && ((ts2 <= *file_to_ts)))) {
                continue;
            }
        }

        /* remember these timestamps */
        *file_from_ts = ts1;
        *file_to_ts = ts2;
    }

    /* success */

cleanup:
    free(dir_path);
    free(prefix);
    closedir(dir);
    return err_info;
}

static sr_error_info_t *
sr_replay_write_ts_notif(int notif_fd, const char *notif_lyb, uint32_t notif_lyb_len, time_t notif_ts, const char *notif_name)
{
    sr_error_info_t *err_info = NULL;

    /* timestamp */
    if ((err_info = sr_write(notif_fd, &notif_ts, sizeof notif_ts))) {
        return err_info;
    }

    /* notification length */
    if ((err_info = sr_write(notif_fd, &notif_lyb_len, sizeof notif_lyb_len))) {
        return err_info;
    }

    /* notification */
    if ((err_info = sr_write(notif_fd, notif_lyb, notif_lyb_len))) {
        return err_info;
    }

    /* fsync */
    if (fsync(notif_fd) == -1) {
        SR_ERRINFO_SYSERRNO(&err_info, "fsync");
        return err_info;
    }

    SR_LOG_INF("Notification \"%s\" stored for replay.", notif_name);

    return NULL;
}

static sr_error_info_t *
sr_replay_rename_file(const char *mod_name, time_t old_from_ts, time_t old_to_ts, time_t new_to_ts)
{
    sr_error_info_t *err_info = NULL;
    char *old_path = NULL, *new_path = NULL;

    assert(old_to_ts <= new_to_ts);

    if (old_to_ts == new_to_ts) {
        /* nothing to do */
        return NULL;
    }

    /* old file name */
    if ((err_info = sr_path_notif_file(mod_name, old_from_ts, old_to_ts, &old_path))) {
        goto cleanup;
    }

    /* new file name */
    if ((err_info = sr_path_notif_file(mod_name, old_from_ts, new_to_ts, &new_path))) {
        goto cleanup;
    }

    /* rename */
    if (rename(old_path, new_path) == -1) {
        SR_ERRINFO_SYSERRNO(&err_info, "rename");
        goto cleanup;
    }

    SR_LOG_INF("Replay file \"%s\" renamed to \"%s\".", strrchr(old_path, '/') + 1, strrchr(new_path, '/') + 1);

    /* success */

cleanup:
    free(old_path);
    free(new_path);
    return err_info;
}

sr_error_info_t *
sr_replay_store(sr_conn_ctx_t *conn, const struct lyd_node *notif, time_t notif_ts)
{
    sr_error_info_t *err_info = NULL;
    sr_mod_t *shm_mod;
    char *notif_lyb = NULL;
    time_t from_ts, to_ts;
    size_t file_size;
    int notif_lyb_len, fd = -1;
    const struct lys_module *ly_mod;
    struct lyd_node *notif_op;

    assert(notif && !notif->parent);

    ly_mod = lyd_node_module(notif);
    notif_op = (struct lyd_node *)notif;
    if ((err_info = sr_ly_find_last_parent(&notif_op, LYS_NOTIF))) {
        goto cleanup;
    }
    SR_CHECK_INT_GOTO(notif_op->schema->nodetype != LYS_NOTIF, err_info, cleanup);

    /* find SHM mod for replay lock and check if replay is even supported */
    shm_mod = sr_shmmain_find_module(conn->main_shm.addr, ly_mod->name, 0);
    SR_CHECK_INT_GOTO(!shm_mod, err_info, cleanup);

    if (!(shm_mod->flags & SR_MOD_REPLAY_SUPPORT)) {
        /* nothing to do */
        goto cleanup;
    }

    /* convert notification into LYB */
    if (lyd_print_mem(&notif_lyb, notif, LYD_LYB, LYP_WITHSIBLINGS)) {
        sr_errinfo_new_ly(&err_info, ly_mod->ctx);
        goto cleanup;
    }

    /* learn its length */
    notif_lyb_len = lyd_lyb_data_length(notif_lyb);
    SR_CHECK_INT_GOTO(notif_lyb_len == -1, err_info, cleanup);

    /* REPLAY WRITE LOCK */
    if ((err_info = sr_rwlock(&shm_mod->replay_lock, SR_MOD_LOCK_TIMEOUT, 1, __func__))) {
        goto cleanup;
    }

    /* find the latest notification file for this module */
    if ((err_info = sr_replay_find_file(ly_mod->name, 0, 0, &from_ts, &to_ts))) {
        goto cleanup_unlock;
    }

    if (from_ts && to_ts) {
        /* open the file */
        if ((err_info = sr_replay_open_file(ly_mod->name, from_ts, to_ts, O_WRONLY | O_APPEND, &fd))) {
            goto cleanup_unlock;
        }

        /* check file size */
        if ((err_info = sr_file_get_size(fd, &file_size))) {
            goto cleanup_unlock;
        }

        if (file_size + sizeof notif_ts + sizeof notif_lyb_len + notif_lyb_len <= SR_EV_NOTIF_FILE_MAX_SIZE * 1024) {
            /* add the notification into the file if there is still space */
            if ((err_info = sr_replay_write_ts_notif(fd, notif_lyb, notif_lyb_len, notif_ts, notif_op->schema->name))) {
                goto cleanup_unlock;
            }

            /* update notification file name */
            if ((err_info = sr_replay_rename_file(ly_mod->name, from_ts, to_ts, notif_ts))) {
                goto cleanup_unlock;
            }

            /* we are done */
            goto cleanup_unlock;
        }

        /* we will create a new file, close this one */
        close(fd);
        fd = -1;
    }

    /* creating a new file */
    if ((err_info = sr_replay_open_file(ly_mod->name, notif_ts, notif_ts, O_WRONLY | O_APPEND | O_CREAT | O_EXCL, &fd))) {
        goto cleanup_unlock;
    }

    /* store the notification */
    if ((err_info = sr_replay_write_ts_notif(fd, notif_lyb, notif_lyb_len, notif_ts, notif_op->schema->name))) {
        goto cleanup_unlock;
    }

    /* success */

cleanup_unlock:
    /* REPLAY WRITE UNLOCK */
    sr_rwunlock(&shm_mod->replay_lock, 1);
cleanup:
    if (fd > -1) {
        close(fd);
    }
    free(notif_lyb);
    return err_info;
}

static sr_error_info_t *
sr_replay_read_ts(int notif_fd, time_t *notif_ts)
{
    *notif_ts = 0;
    return sr_read(notif_fd, notif_ts, sizeof *notif_ts);
}

static sr_error_info_t *
sr_replay_read_notif(int notif_fd, struct ly_ctx *ly_ctx, struct lyd_node **notif)
{
    sr_error_info_t *err_info = NULL;
    char *notif_lyb = NULL;
    uint32_t notif_lyb_len;

    /* read the length */
    if ((err_info = sr_read(notif_fd, &notif_lyb_len, sizeof notif_lyb_len))) {
        goto cleanup;
    }

    /* read the notification */
    notif_lyb = malloc(notif_lyb_len);
    SR_CHECK_MEM_GOTO(!notif_lyb, err_info, cleanup);

    if ((err_info = sr_read(notif_fd, notif_lyb, notif_lyb_len))) {
        goto cleanup;
    }

    /* parse the notification */
    ly_errno = 0;
    *notif = lyd_parse_mem(ly_ctx, notif_lyb, LYD_LYB, LYD_OPT_NOTIF | LYD_OPT_NOEXTDEPS | LYD_OPT_STRICT, NULL);
    if (ly_errno) {
        sr_errinfo_new_ly(&err_info, ly_ctx);
        goto cleanup;
    }

cleanup:
    free(notif_lyb);
    return err_info;
}

static sr_error_info_t *
sr_replay_skip_notif(int notif_fd)
{
    sr_error_info_t *err_info = NULL;
    uint32_t notif_lyb_len;

    /* read notification length */
    if ((err_info = sr_read(notif_fd, &notif_lyb_len, sizeof notif_lyb_len))) {
        return err_info;
    }

    /* skip the notification */
    if (lseek(notif_fd, notif_lyb_len, SEEK_CUR) == -1) {
        SR_ERRINFO_SYSERRNO(&err_info, "lseek");
        return err_info;
    }

    return NULL;
}

sr_error_info_t *
sr_replay_notify(sr_conn_ctx_t *conn, const char *mod_name, const char *xpath, time_t start_time, time_t stop_time,
        sr_event_notif_cb cb, sr_event_notif_tree_cb tree_cb, void *private_data)
{
    sr_error_info_t *err_info = NULL;
    sr_mod_t *shm_mod;
    time_t file_from_ts, file_to_ts, notif_ts;
    struct ly_set *set = NULL;
    struct lyd_node *notif = NULL, *notif_op;
    int fd = -1;
    sr_sid_t sid = {0};

    /* find SHM mod for replay lock and check if replay is even supported */
    shm_mod = sr_shmmain_find_module(conn->main_shm.addr, mod_name, 0);
    SR_CHECK_INT_GOTO(!shm_mod, err_info, cleanup);

    if (!(shm_mod->flags & SR_MOD_REPLAY_SUPPORT)) {
        /* nothing to do */
        sr_errinfo_new(&err_info, SR_ERR_INVAL_ARG, NULL, "Module \"%s\" does not support notification replay.", mod_name);
        goto cleanup;
    }

    /* find first file */
    if ((err_info = sr_replay_find_file(mod_name, start_time, 0, &file_from_ts, &file_to_ts))) {
        goto cleanup;
    }

    /* is this a valid notification file? */
    while (file_from_ts && file_to_ts && (!stop_time || (file_from_ts <= stop_time))) {
        /* open the file */
        if ((err_info = sr_replay_open_file(mod_name, file_from_ts, file_to_ts, O_RDONLY, &fd))) {
            goto cleanup;
        }

        /* skip all earlier notifications */
        do {
            if ((err_info = sr_replay_read_ts(fd, &notif_ts))) {
                goto cleanup;
            }
            if (!notif_ts) {
                sr_errinfo_new(&err_info, SR_ERR_INTERNAL, NULL, "Unexpected notification file EOF.");
                goto cleanup;
            }
            if ((notif_ts < start_time) && (err_info = sr_replay_skip_notif(fd))) {
                goto cleanup;
            }
        } while (notif_ts < start_time);

        /* replay notifications until stop_time is reached */
        while (notif_ts && (!stop_time || (notif_ts <= stop_time))) {

            /* parse notification */
            lyd_free_withsiblings(notif);
            if ((err_info = sr_replay_read_notif(fd, conn->ly_ctx, &notif))) {
                goto cleanup;
            }

            /* make sure the XPath filter matches something */
            if (xpath) {
                ly_set_free(set);
                set = lyd_find_path(notif, xpath);
                SR_CHECK_INT_GOTO(!set, err_info, cleanup);
            }

            if (!xpath || set->number) {
                /* find notification node */
                notif_op = notif;
                if ((err_info = sr_ly_find_last_parent(&notif_op, LYS_NOTIF))) {
                    goto cleanup;
                }
                SR_CHECK_INT_GOTO(notif_op->schema->nodetype != LYS_NOTIF, err_info, cleanup);

                /* call callback */
                if ((err_info = sr_notif_call_callback(conn, cb, tree_cb, private_data, SR_EV_NOTIF_REPLAY, notif_op,
                        notif_ts, sid))) {
                    goto cleanup;
                }
            }

            /* read next timestamp */
            if ((err_info = sr_replay_read_ts(fd, &notif_ts))) {
                goto cleanup;
            }
        }

        /* no more notifications should be replayed */
        if (stop_time && (notif_ts > stop_time)) {
            break;
        }

        /* find next notification file and read from it */
        if ((err_info = sr_replay_find_file(mod_name, file_from_ts, file_to_ts, &file_from_ts, &file_to_ts))) {
            goto cleanup;
        }
    }

    /* replay last notification if the subscription continues */
    notif_ts = time(NULL);
    if ((!stop_time || (stop_time >= notif_ts)) && (err_info = sr_notif_call_callback(conn, cb, tree_cb, private_data,
            SR_EV_NOTIF_REPLAY_COMPLETE, NULL, stop_time ? stop_time : notif_ts, sid))) {
        goto cleanup;
    }

    /* success */

cleanup:
    if (fd > -1) {
        close(fd);
    }
    lyd_free_withsiblings(notif);
    ly_set_free(set);
    return err_info;
}
