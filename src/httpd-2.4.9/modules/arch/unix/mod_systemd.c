/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * 
 */

#include <stdint.h>
#include <ap_config.h>
#include "ap_mpm.h"
#include <http_core.h>
#include <http_log.h>
#include <apr_version.h>
#include <apr_pools.h>
#include <apr_strings.h>
#include "unixd.h"
#include "scoreboard.h"
#include "mpm_common.h"

#include "systemd/sd-daemon.h"

#if APR_HAVE_UNISTD_H
#include <unistd.h>
#endif

#define KBYTE 1024

static pid_t pid;	/* PID of the main httpd instance */
static int server_limit, thread_limit, threads_per_child, max_servers;
static time_t last_update_time;
static unsigned long last_update_access;
static unsigned long last_update_kbytes;

static int systemd_pre_mpm(apr_pool_t *p, ap_scoreboard_e sb_type)
{
    int rv;
    last_update_time = time(0);

    ap_mpm_query(AP_MPMQ_HARD_LIMIT_THREADS, &thread_limit);
    ap_mpm_query(AP_MPMQ_HARD_LIMIT_DAEMONS, &server_limit);
    ap_mpm_query(AP_MPMQ_MAX_THREADS, &threads_per_child);
    /* work around buggy MPMs */
    if (threads_per_child == 0)
        threads_per_child = 1;
    ap_mpm_query(AP_MPMQ_MAX_DAEMONS, &max_servers);

    pid = getpid();
    
    rv = sd_notifyf(0, "READY=1\n"
                    "STATUS=Processing requests...\n"
                    "MAINPID=%lu",
                    (unsigned long) pid);
    if (rv < 0) {
        ap_log_perror(APLOG_MARK, APLOG_ERR, 0, p, 
                     "sd_notifyf returned an error %d", rv);
    }

    return OK;
}

static int systemd_monitor(apr_pool_t *p, server_rec *s)
{
    int i, j, res, rv;
    process_score *ps_record;
    worker_score *ws_record;
    unsigned long access = 0;
    unsigned long bytes = 0;
    unsigned long kbytes = 0;
    char bps[5];
    time_t now = time(0);
    time_t elapsed = now - last_update_time;

    for (i = 0; i < server_limit; ++i) {
        ps_record = ap_get_scoreboard_process(i);
        for (j = 0; j < thread_limit; ++j) {
            ws_record = ap_get_scoreboard_worker_from_indexes(i, j);
            if (ap_extended_status && !ps_record->quiescing && ps_record->pid) {
                res = ws_record->status;
                if (ws_record->access_count != 0 || 
                    (res != SERVER_READY && res != SERVER_DEAD)) {
                    access += ws_record->access_count;
                    bytes += ws_record->bytes_served;
                    if (bytes >= KBYTE) {
                        kbytes += (bytes >> 10);
                        bytes = bytes & 0x3ff;
                    }
                }
            }
        }
    }

    apr_strfsize((unsigned long)(KBYTE *(float) (kbytes - last_update_kbytes)
                                 / (float) elapsed), bps);

    rv = sd_notifyf(0, "READY=1\n"
                    "STATUS=Total requests: %lu; Current requests/sec: %.3g; "
                    "Current traffic: %sB/sec\n", access,
                    ((float)access - last_update_access) / (float) elapsed, bps);
    if (rv < 0) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s, APLOGNO(00000)
                     "sd_notifyf returned an error %d", rv);
    }

    last_update_access = access;
    last_update_kbytes = kbytes;
    last_update_time = now;

    return DECLINED;
}

static void systemd_register_hooks(apr_pool_t *p)
{
    /* We know the PID in this hook ... */
    ap_hook_pre_mpm(systemd_pre_mpm, NULL, NULL, APR_HOOK_LAST);
    /* Used to update httpd's status line using sd_notifyf */
    ap_hook_monitor(systemd_monitor, NULL, NULL, APR_HOOK_MIDDLE);
}

module AP_MODULE_DECLARE_DATA systemd_module =
{
    STANDARD20_MODULE_STUFF,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    systemd_register_hooks,
};
