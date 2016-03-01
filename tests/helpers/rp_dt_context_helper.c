/**
 * @file rp_dt_context_helper.c
 * @author Rastislav Szabo <raszabo@cisco.com>, Lukas Macko <lmacko@cisco.com>
 * @brief RP datatree context helper functions.
 *
 * @copyright
 * Copyright 2016 Cisco Systems, Inc.
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

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <setjmp.h>
#include <cmocka.h>

#include "test_data.h"
#include "rp_data_tree.h"
#include "rp_internal.h"

void
test_rp_ctx_create(rp_ctx_t **rp_ctx_p)
{
    int rc = SR_ERR_OK;
    rp_ctx_t *ctx = NULL;

    ctx = calloc(1, sizeof(*ctx));
    assert_non_null(ctx);

    rc = ac_init(&ctx->ac_ctx);
    assert_int_equal(SR_ERR_OK, rc);

    rc = dm_init(ctx->ac_ctx, TEST_SCHEMA_SEARCH_DIR, TEST_DATA_SEARCH_DIR, &ctx->dm_ctx);
    assert_int_equal(SR_ERR_OK, rc);

    *rp_ctx_p = ctx;
}

void
test_rp_ctx_cleanup(rp_ctx_t *ctx)
{
    ac_cleanup(ctx->ac_ctx);
    dm_cleanup(ctx->dm_ctx);
    free(ctx);
}

void
test_rp_sesssion_create(rp_ctx_t *rp_ctx, sr_datastore_t datastore, rp_session_t **rp_session_p)
{
    rp_session_t *session = NULL;
    ac_ucred_t *credentials = NULL;
    int rc = SR_ERR_OK;

    credentials = calloc(1, sizeof(*credentials));

    credentials->r_username = getenv("USER");
    credentials->r_uid = getuid();
    credentials->r_gid = getgid();

    rc = rp_session_start(rp_ctx, 123456, credentials, datastore, &session);
    assert_int_equal(SR_ERR_OK, rc);

    *rp_session_p = session;
}

void
test_rp_session_cleanup(rp_ctx_t *ctx, rp_session_t *session)
{
    if (NULL != session) {
        free((void*)session->user_credentials);
        rp_session_stop(ctx, session);
    }
}
