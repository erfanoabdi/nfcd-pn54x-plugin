/*
 * Copyright (C) 2018-2019 Jolla Ltd.
 * Copyright (C) 2019 Open Mobile Platform LLC.
 * Copyright (C) 2019 Slava Monich <slava.monich@jolla.com>
 *
 * You may use this file under the terms of the BSD license as follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in
 *      the documentation and/or other materials provided with the
 *      distribution.
 *   3. Neither the names of the copyright holders nor the names of its
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation
 * are those of the authors and should not be interpreted as representing
 * any official policies, either expressed or implied.
 */

#include "test_common.h"

#include <gutil_log.h>

static
gboolean
test_timeout_expired(
    gpointer data)
{
    g_assert_not_reached();
    return G_SOURCE_REMOVE;
}

typedef struct test_quit_later_data{
    GMainLoop* loop;
    guint n;
} TestQuitLaterData;

static
void
test_quit_later_n_free(
    gpointer user_data)
{
    TestQuitLaterData* data = user_data;

    g_main_loop_unref(data->loop);
    g_free(data);
}

static
gboolean
test_quit_later_n_func(
    gpointer user_data)
{
    TestQuitLaterData* data = user_data;

    if (data->n > 0) {
        data->n--;
        return G_SOURCE_CONTINUE;
    } else {
        g_main_loop_quit(data->loop);
        return G_SOURCE_REMOVE;
    }
}

void
test_quit_later_n(
    GMainLoop* loop,
    guint n)
{
    TestQuitLaterData* data = g_new0(TestQuitLaterData, 1);

    data->loop = g_main_loop_ref(loop);
    data->n = n;
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, test_quit_later_n_func, data,
        test_quit_later_n_free);
}

void
test_quit_later(
    GMainLoop* loop)
{
    test_quit_later_n(loop, 0);
}

void
test_run(
    const TestOpt* opt,
    GMainLoop* loop)
{
    if (opt->flags & TEST_FLAG_DEBUG) {
        g_main_loop_run(loop);
    } else {
        const guint timeout_id = g_timeout_add_seconds(TEST_TIMEOUT_SEC,
            test_timeout_expired, NULL);
        g_main_loop_run(loop);
        g_source_remove(timeout_id);
    }
}

void
test_init(
    TestOpt* opt,
    int argc,
    char* argv[])
{
    const char* sep1;
    const char* sep2;
    int i;

    memset(opt, 0, sizeof(*opt));
    for (i=1; i<argc; i++) {
        const char* arg = argv[i];
        if (!strcmp(arg, "-d") || !strcmp(arg, "--debug")) {
            opt->flags |= TEST_FLAG_DEBUG;
        } else if (!strcmp(arg, "-v")) {
            GTestConfig* config = (GTestConfig*)g_test_config_vars;
            config->test_verbose = TRUE;
        } else {
            GWARN("Unsupported command line option %s", arg);
        }
    }

    /* Setup logging */
    sep1 = strrchr(argv[0], '/');
    sep2 = strrchr(argv[0], '\\');
    gutil_log_default.name = (sep1 && sep2) ? (MAX(sep1, sep2) + 1) :
        sep1 ? (sep1 + 1) : sep2 ? (sep2 + 1) : argv[0];
    gutil_log_default.level = g_test_verbose() ?
        GLOG_LEVEL_VERBOSE : GLOG_LEVEL_NONE;
    gutil_log_timestamp = FALSE;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
