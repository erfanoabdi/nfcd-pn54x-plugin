/*
 * Copyright (C) 2019-2021 Jolla Ltd.
 * Copyright (C) 2019-2021 Slava Monich <slava.monich@jolla.com>
 * Copyright (C) 2019 Open Mobile Platform LLC.
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

#include "pn54x_io.h"

#include <gutil_macros.h>
#include <gutil_misc.h>
#include <gutil_log.h>

#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>

static TestOpt test_opt;

static int test_fd = -1;
static int test_ioctl_ret = -1;
static int test_open_errno = ENODEV;
static int test_ioctl_errno = EINVAL;

int
pn54x_system_open(
    const char* dev)
{
    if (test_fd >= 0) {
        return dup(test_fd);
    } else {
        errno = test_open_errno;
        return -1;
    }
}

int
pn54x_system_ioctl(
    int fd,
    unsigned int cmd,
    unsigned long arg)
{
    errno = (test_ioctl_ret == 0) ? 0 : test_ioctl_errno;
    return test_ioctl_ret;
}

static
void
test_reset()
{
    test_fd = -1;
    test_ioctl_ret = -1;
    test_open_errno = ENODEV;
    test_ioctl_errno = EINVAL;
}

static
void
test_no_error(
    NciHalClient* client)
{
    g_assert_not_reached();
}

static
void
test_no_read(
    NciHalClient* client,
    const void* data,
    guint len)
{
    g_assert_not_reached();
}

static
void
test_no_write(
    NciHalClient* client,
    gboolean ok)
{
    g_assert_not_reached();
}

static
gboolean
test_unblock(
    gpointer user_data)
{
    test_quit_later((GMainLoop*) user_data);
    return G_SOURCE_REMOVE;
}

/*==========================================================================*
 * null
 *==========================================================================*/

static
void
test_null(
    void)
{
    g_assert_null(pn54x_io_new(NULL));
    g_assert(!pn54x_io_set_power(NULL, FALSE));
    pn54x_io_free(NULL);
}

/*==========================================================================*
 * open_error
 *==========================================================================*/

static
void
test_open_error(
    void)
{
    test_reset();
    g_assert_null(pn54x_io_new("test"));
}

/*==========================================================================*
 * read
 *==========================================================================*/

typedef struct test_read_input_chunk {
    GUtilData data;
    gboolean has_packet;
} TestReadInputChunk;

typedef struct test_read_config {
    const char* name;
    const TestReadInputChunk* in;
    guint in_count;
    const GUtilData* out;
    guint out_count;
} TestReadConfig;

typedef struct test_read_data {
    NciHalClient client;
    const TestReadConfig* config;
    GMainLoop* loop;
    guint nout;
    int fd;
} TestRead;

static
void
test_read_proc(
    NciHalClient* client,
    const void* data,
    guint len)
{
    TestRead* test = G_CAST(client, TestRead, client);
    const GUtilData* out = test->config->out + test->nout;
    GString* buf = g_string_new(NULL);
    guint i;

    test->nout++;

    g_string_append_printf(buf, "%u:", test->nout);
    for (i = 0; i < len; i++) {
        g_string_append_printf(buf, " %02x", ((const guint8*)data)[i]);
    }
    GDEBUG("%s", buf->str);
    g_string_free(buf, TRUE);

    g_assert_cmpint(out->size, ==, len);
    g_assert(!memcmp(out->bytes, data, len));
    g_main_loop_quit(test->loop);
}

static
void
test_read(
    gconstpointer data)
{
    int fd[2];
    TestRead test;
    Pn54xHalIo* hal;
    NciHalIo* io;
    guint i;
    const TestReadConfig* config = data;
    static const NciHalClientFunctions test_read_fn = {
        test_no_error, test_read_proc
    };

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, fd), ==, 0);
    memset(&test, 0, sizeof(test));

    test_reset();
    test_ioctl_ret = 0;
    test_fd = fd[0];
    test.fd = fd[1];
    test.client.fn = &test_read_fn;
    test.config = config;
    test.loop = g_main_loop_new(NULL, FALSE);

    hal = pn54x_io_new("test");
    g_assert(hal);
    io = &hal->hal_io;
    io->fn->start(io, &test.client);
    pn54x_io_set_power(hal, TRUE);

    for (i = 0; i < config->in_count && test.nout < config->out_count; i++) {
        const TestReadInputChunk* in = config->in + i;
        const GUtilData* data = &in->data;

        g_assert_cmpint(write(fd[1], data->bytes, data->size), ==, data->size);
        if (in->has_packet) {
            GDEBUG("Waiting for incoming packet(s)...");
            test_run(&test_opt, test.loop);
        } else {
            /* Give the child a chance to read the data and pass it back */
            g_timeout_add(100, test_unblock, test.loop);
            test_run(&test_opt, test.loop);
        }
    }

    g_assert_cmpuint(i, == ,config->in_count);
    g_assert_cmpuint(test.nout, == ,config->out_count);
    g_assert_cmpint(close(test.fd), ==, 0);
    test.fd = -1;
    io->fn->stop(io);

    g_main_loop_unref(test.loop);
    close(test_fd);
    test_reset();
    pn54x_io_free(hal);
}


static const guint8 test_read_in_basic_1[] = {
    0x60, 0x08, 0x02, 0x05, 0x05, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};
static const TestReadInputChunk test_read_in_basic[] = {
    { {TEST_ARRAY_AND_SIZE(test_read_in_basic_1)}, TRUE }
};
static const GUtilData test_read_out_basic[] = {
    { test_read_in_basic_1, 5 /* First 5 bytes of the input */ }
};

static const guint8 test_read_in_split_1[] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};
static const guint8 test_read_in_split_2[] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x60, 0x08,
};
static const guint8 test_read_in_split_3[] = {
    0x02, 0xb2,
};
static const guint8 test_read_in_split_4[] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};
static const guint8 test_read_out_split_1[] = {
    0x60, 0x08, 0x02, 0xb2, 0x00
};
static const TestReadInputChunk test_read_in_split[] = {
    { {TEST_ARRAY_AND_SIZE(test_read_in_split_1)}, FALSE },
    { {TEST_ARRAY_AND_SIZE(test_read_in_split_2)}, FALSE },
    { {TEST_ARRAY_AND_SIZE(test_read_in_split_3)}, FALSE },
    { {TEST_ARRAY_AND_SIZE(test_read_in_split_4)}, TRUE }
};
static const GUtilData test_read_out_split[] = {
    { TEST_ARRAY_AND_SIZE(test_read_out_split_1) },
};

static const guint8 test_read_in_combined_1[] = {
    0x60, 0x08, 0x02, 0xb2, 0x00, 0xff, 0xff, 0xff,
    0x61, 0x06, 0x02, 0x03, 0x00, 0xff, 0xff, 0xff,
    0xff, 0xff
};
static const TestReadInputChunk test_read_in_combined[] = {
    { {TEST_ARRAY_AND_SIZE(test_read_in_combined_1)}, TRUE }
};
static const GUtilData test_read_out_combined[] = {
    { test_read_in_combined_1, 5 /* First 5 bytes of the input */ },
    { test_read_in_combined_1 + 8, 5 /* And another 5 bytes */ }
};
static const TestReadConfig read_tests[] = {
    {
        "basic",
        TEST_ARRAY_AND_COUNT(test_read_in_basic),
        TEST_ARRAY_AND_COUNT(test_read_out_basic)
    },{
        "split",
        TEST_ARRAY_AND_COUNT(test_read_in_split),
        TEST_ARRAY_AND_COUNT(test_read_out_split)
    },{
        "combined",
        TEST_ARRAY_AND_COUNT(test_read_in_combined),
        TEST_ARRAY_AND_COUNT(test_read_out_combined)
    }
};

/*==========================================================================*
 * basic_write
 *==========================================================================*/

typedef struct test_basic_write_data {
    NciHalClient client;
    GMainLoop* loop;
    int fd;
} TestBasicWrite;

static
void
test_basic_write_ok(
    NciHalClient* client,
    gboolean ok)
{
    TestBasicWrite* test = G_CAST(client, TestBasicWrite, client);

    GDEBUG_("%d", ok);
    g_assert(ok);
    g_main_loop_quit(test->loop);
}

static
void
test_basic_write(
    void)
{
    int fd[2];
    TestBasicWrite test;
    Pn54xHalIo* hal;
    NciHalIo* io;
    static const NciHalClientFunctions test_write_fn = {
        test_no_error, test_no_read
    };
    static const guint8 rset[] = { 0x20, 0x01, 0x00 };
    static const GUtilData rset_data = { rset, sizeof(rset) };
    guint buf[sizeof(rset) + 1];

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, fd), ==, 0);
    memset(&test, 0, sizeof(test));

    test_reset();
    test_ioctl_ret = 0;
    test_fd = fd[0];
    test.fd = fd[1];
    test.client.fn = &test_write_fn;

    hal = pn54x_io_new("test");
    g_assert(hal);
    io = &hal->hal_io;
    io->fn->start(io, &test.client);

    /* Write completion will terminate the loop */
    test.loop = g_main_loop_new(NULL, FALSE);
    g_assert(io->fn->write(io, &rset_data, 1, test_basic_write_ok));
    test_run(&test_opt, test.loop);

    /* Read the data back from the other end of the pipe */
    g_assert_cmpint(read(test.fd, buf, sizeof(buf)), ==, sizeof(rset));
    g_assert(!memcmp(buf, rset, sizeof(rset)));
    g_assert_cmpint(close(test.fd), ==, 0);
    test.fd = -1;
    io->fn->stop(io);

    g_main_loop_unref(test.loop);
    close(test_fd);
    test_reset();
    pn54x_io_free(hal);
}

/*==========================================================================*
 * cancel_write
 *==========================================================================*/

static
void
test_cancel_write(
    void)
{
    int fd[2];
    TestBasicWrite test;
    Pn54xHalIo* hal;
    NciHalIo* io;
    static const NciHalClientFunctions test_write_fn = {
        test_no_error, test_no_read
    };
    static const guint8 rset[] = { 0x20, 0x01, 0x00 };
    static const GUtilData rset_data = { rset, sizeof(rset) };
    guint buf[sizeof(rset) + 1];

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, fd), ==, 0);
    memset(&test, 0, sizeof(test));

    test_reset();
    test_ioctl_ret = 0;
    test_fd = fd[0];
    test.fd = fd[1];
    test.client.fn = &test_write_fn;

    hal = pn54x_io_new("test");
    g_assert(hal);
    io = &hal->hal_io;
    io->fn->start(io, &test.client);

    test.loop = g_main_loop_new(NULL, FALSE);
    g_assert(io->fn->write(io, &rset_data, 1, test_no_write));
    io->fn->cancel_write(io);
    io->fn->cancel_write(io); /* This one has no effect */

    /* Make sure that write completion is not invoked */
    test_quit_later_n(test.loop, 2);
    test_run(&test_opt, test.loop);

    /* The data is actually still written, read it */
    g_assert_cmpint(read(test.fd, buf, sizeof(buf)), ==, sizeof(rset));
    g_assert(!memcmp(buf, rset, sizeof(rset)));
    g_assert_cmpint(close(test.fd), ==, 0);
    test.fd = -1;
    io->fn->stop(io);

    g_main_loop_unref(test.loop);
    close(test_fd);
    test_reset();
    pn54x_io_free(hal);
}

/*==========================================================================*
 * write_chunks
 *==========================================================================*/

static
void
test_write_chunks(
    void)
{
    /* NOTE: reusing TestBasicWrite and test_basic_write_ok() here */
    int fd[2];
    TestBasicWrite test;
    Pn54xHalIo* hal;
    NciHalIo* io;
    static const NciHalClientFunctions test_write_chunks_fn = {
        test_no_error, test_no_read
    };
    static const guint8 rset[] = { 0x20, 0x01, 0x00 };
    static const GUtilData data[] = {
        { rset, 1 },
        { rset + 1, sizeof(rset) - 1}
    };
    guint buf[sizeof(rset) + 1];

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, fd), ==, 0);
    memset(&test, 0, sizeof(test));

    test_reset();
    test_ioctl_ret = 0;
    test_fd = fd[0];
    test.fd = fd[1];
    test.client.fn = &test_write_chunks_fn;

    hal = pn54x_io_new("test");
    g_assert(hal);
    io = &hal->hal_io;
    io->fn->start(io, &test.client);

    /* Write completion will terminate the loop */
    test.loop = g_main_loop_new(NULL, FALSE);
    g_assert(io->fn->write(io, data, G_N_ELEMENTS(data), test_basic_write_ok));
    test_run(&test_opt, test.loop);

    /* Read the data back from the other end of the pipe */
    g_assert_cmpint(read(test.fd, buf, sizeof(buf)), ==, sizeof(rset));
    g_assert(!memcmp(buf, rset, sizeof(rset)));
    g_assert_cmpint(close(test.fd), ==, 0);
    test.fd = -1;
    io->fn->stop(io);

    g_main_loop_unref(test.loop);
    close(test_fd);
    test_reset();
    pn54x_io_free(hal);
}

/*==========================================================================*
 * Common
 *==========================================================================*/

#define TEST_(name) "/pn54x_io/" name

int main(int argc, char* argv[])
{
    guint i;

    signal(SIGPIPE, SIG_IGN);
    g_test_init(&argc, &argv, NULL);
    g_test_add_func(TEST_("null"), test_null);
    g_test_add_func(TEST_("open_error"), test_open_error);
    g_test_add_func(TEST_("basic_write"), test_basic_write);
    g_test_add_func(TEST_("cancel_write"), test_cancel_write);
    g_test_add_func(TEST_("write_chunks"), test_write_chunks);
    for (i = 0; i < G_N_ELEMENTS(read_tests); i++) {
        const TestReadConfig* test = read_tests + i;
        char* path = g_strconcat(TEST_("read/"), test->name, NULL);

        g_test_add_data_func(path, test, test_read);
        g_free(path);
    }
    test_init(&test_opt, argc, argv);
    return g_test_run();
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
