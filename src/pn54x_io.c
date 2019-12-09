/*
 * Copyright (C) 2019 Jolla Ltd.
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

#include "pn54x_io.h"
#include "pn54x_log.h"
#include "pn54x_system.h"

#include <gutil_macros.h>
#include <gutil_misc.h>

#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>

#define PN54X_MAX_PACKET_SIZE (512)
#define NCI_PACKET_HEADFER_SIZE (3)

#define PN54X_SET_PWR   _IOW(0xe9, 0x01, unsigned int)
#define PN54X_PWR_ON    (1)
#define PN54X_PWR_OFF   (0)

typedef struct pn54x_io {
    Pn54xHalIo pn54x;
    NciHalClient* client;
    GMainContext* context;
    gint refcount;
    char* dev;
    int fd;

    /* Read */
    int read_fd;
    pid_t read_pid;
    void* read_tmp_buf;
    GByteArray* read_buf;
    GIOChannel* read_channel;
    guint read_watch_id;

    /* Write */
    guint write_id;
    NciHalClientFunc write_cb;
    GByteArray* write_buf;
} Pn54xIo;

/* pn54x_hexdump_log is a sub-module, just to turn prefix off */

GLogModule pn54x_hexdump_log = {
    .name = "pn54x-hexdump",
    .parent = &GLOG_MODULE_NAME,
    .max_level = GLOG_LEVEL_MAX,
    .level = GLOG_LEVEL_INHERIT,
    .flags = GLOG_FLAG_HIDE_NAME
};

#define DIR_IN  '>'
#define DIR_OUT '<'
#define DUMP(f,args...)  gutil_log(&pn54x_hexdump_log, \
       GLOG_LEVEL_VERBOSE, f, ##args)

/*==========================================================================*
 * Implementation
 *==========================================================================*/

static
void
pn54x_hexdump(
    GLogModule* log,
    int level,
    char dir,
    const void* data,
    int len)
{
    const guint8* ptr = data;
    gboolean empty = FALSE;
    guint empty_len = 0, skip_count = 0;

    while (len > 0) {
        char buf[GUTIL_HEXDUMP_BUFSIZE];
        const gboolean was_empty = empty;
        const guint consumed = gutil_hexdump(buf, ptr, len);
        guint i;

        /* Don't print boring ff's too many times */
        for (i = 0, empty = TRUE; i < consumed; i++) {
            if (ptr[i] != 0xff) {
                empty = FALSE;
                break;
            }
        }
        len -= consumed;
        ptr += consumed;
        if (was_empty && empty && empty_len == consumed) {
            skip_count++;
        } else {
            if (skip_count) {
                gutil_log(log, level, "  %u line(s) skipped", skip_count);
                skip_count = 0;
            }
            gutil_log(log, level, "%c %s", dir, buf);
            dir = ' ';
        }
        if (empty) {
            empty_len = consumed;
        }
    }
    if (skip_count) {
        gutil_log(log, level, "  ... %u line(s) skipped", skip_count);
    }
}

static
void
pn54x_dump_data(
    char dir,
    const void* data,
    guint len)
{
    const int level = GLOG_LEVEL_VERBOSE;
    GLogModule* log = &pn54x_hexdump_log;

    if (gutil_log_enabled(log, level)) {
        pn54x_hexdump(log, level, dir, data, len);
    }
}

static
Pn54xIo*
pn54x_io_cast(
    Pn54xHalIo* io)
{
    return G_CAST(io, Pn54xIo, pn54x);
}

static
Pn54xIo*
pn54x_hal_io_cast(
    NciHalIo* hal_io)
{
    return G_CAST(hal_io, Pn54xIo, pn54x.hal_io);
}

static
gboolean
pn54x_io_open(
    Pn54xIo* self)
{
    if (self->fd >= 0) {
        return TRUE;
    } else {
        self->fd = pn54x_system_open(self->dev);
        if (self->fd >= 0) {
            GVERBOSE("Opened %s", self->dev);
            return TRUE;
        } else {
            GERR("Failed to open %s: %s", self->dev, strerror(errno));
            return FALSE;
        }
    }
}

static
void
pn54x_io_close(
    Pn54xIo* self)
{
    if (self->read_watch_id) {
        g_source_remove(self->read_watch_id);
        self->read_watch_id = 0;
    }
    if (self->read_channel) {
        g_io_channel_shutdown(self->read_channel, FALSE, NULL);
        g_io_channel_unref(self->read_channel);
        self->read_channel = NULL;
    }
    if (self->read_pid) {
        int status;

        GDEBUG("Killing child %d", self->read_pid);
        kill(self->read_pid, SIGKILL);
        waitpid(self->read_pid, &status, 0);
        self->read_pid = 0;
    }
    if (self->read_fd >= 0) {
        close(self->read_fd);
        self->read_fd = -1;
    }
    if (self->fd >= 0) {
        close(self->fd);
        self->fd = -1;
        GVERBOSE("Closed %s", self->dev);
    }
}

static
void
pn54x_io_stop(
    Pn54xIo* self)
{
    self->client = NULL;
    self->write_cb = NULL;
    g_byte_array_set_size(self->read_buf, 0);
    if (self->write_id) {
        g_source_remove(self->write_id);
        self->write_id = 0;
    }
    pn54x_io_close(self);
}

static
void
pn54x_io_finalize(
    Pn54xIo* self)
{
    pn54x_io_stop(self);
    g_byte_array_free(self->read_buf, TRUE);
    g_byte_array_free(self->write_buf, TRUE);
    g_free(self->read_tmp_buf);
    g_free(self->dev);
    g_free(self);
}

static
void
pn54x_io_unref(
    Pn54xIo* self)
{
    GASSERT(self->refcount > 0);
    if (g_atomic_int_dec_and_test(&self->refcount)) {
        pn54x_io_finalize(self);
    }
}

static
guint
pn54x_io_read_packet_size(
    const void* buf,
    gsize size)
{
    /* Octet 2 is the payload length in both control and data packets */
    if (size >= NCI_PACKET_HEADFER_SIZE) {
        const guint8* pkt = buf;
        const guint max_payload = size - NCI_PACKET_HEADFER_SIZE;

        /* Driver fills unused part of the buffer with 0xff's */
        if (pkt[0] != 0xff) {
            const guint payload_len = pkt[2];

            if (payload_len <= max_payload) {
                return NCI_PACKET_HEADFER_SIZE + payload_len;
            }
        }
    }
    return 0;
}

static
gboolean
pn54x_io_read_done(
    const void* buf,
    gsize size)
{
    /* Driver fills unused part of the buffer with 0xff's */
    const guint8* ptr = buf;
    const guint8* end = ptr + size;

    while (ptr < end) {
        if (*ptr++ != 0xff) {
            /* There's something in there */
            return FALSE;
        }
    }
    /* All 0xff's (or nothing at all) */
    return TRUE;
}

static
void
pn54x_io_read_handle(
    Pn54xIo* self,
    const void* buf,
    gsize size)
{
    NciHalClient* client = self->client;
    GByteArray* read_buf = self->read_buf;
    gsize nbytes, pktsiz;
    const guint8* ptr;

    DUMP("%c %u byte(s)", DIR_IN, (guint)size);
    pn54x_dump_data(DIR_IN, buf, size);

    if (read_buf->len) {
        /* Something left from the previous read */
        g_byte_array_append(read_buf, buf, size);
        ptr = read_buf->data;
        nbytes = read_buf->len;
    } else {
        /* We must be at the NCI packet boundary */
        ptr = buf;
        nbytes = size;
    }

    while ((pktsiz = pn54x_io_read_packet_size(ptr, nbytes)) > 0) {
        client->fn->read(client, ptr, pktsiz);
        ptr += pktsiz;
        nbytes -= pktsiz;
    }

    if (pn54x_io_read_done(ptr, nbytes)) {
        g_byte_array_set_size(read_buf, 0);
    } else if (read_buf->len) {
        /* Erase consumed data from read_buf */
        g_byte_array_remove_range(read_buf, 0, read_buf->len - nbytes);
    } else {
        /* Move unused data to read_buf */
        g_byte_array_append(read_buf, ptr, nbytes);
    }
}

static
gboolean
pn54x_io_read_chars(
    GIOChannel* channel,
    void* buf,
    gsize count,
    gsize* bytes_read)
{
    GError* error = NULL;
    GIOStatus status = g_io_channel_read_chars(channel, buf, count,
        bytes_read, &error);

    if (error) {
        GERR("Read failed: %s", error->message);
        g_error_free(error);
        return FALSE;
    } else if (status == G_IO_STATUS_EOF) {
        GDEBUG("End of stream");
        return FALSE;
    } else {
        return TRUE;
    }
}

static
gboolean
pn54x_io_read_callback(
    GIOChannel* channel,
    GIOCondition condition,
    gpointer user_data)
{
    Pn54xIo* self = user_data;
    NciHalClient* client;

    if (condition & G_IO_IN) {
        gsize bytes_read;

        if (pn54x_io_read_chars(channel, self->read_tmp_buf,
            PN54X_MAX_PACKET_SIZE, &bytes_read)) {
            pn54x_io_read_handle(self, self->read_tmp_buf, bytes_read);
            return G_SOURCE_CONTINUE;
        }
    } else {
        GERR("Read condition 0x%04X", condition);
    }

    client = self->client;
    self->read_watch_id = 0;
    client->fn->error(client);
    return G_SOURCE_REMOVE;
}

static
gboolean
pn54x_hal_io_write_complete(
    gpointer data)
{
    Pn54xIo* self = data;
    NciHalClientFunc cb = self->write_cb;

    GASSERT(self->write_id);
    self->write_id = 0;
    self->write_cb = NULL;
    cb(self->client, TRUE);
    return G_SOURCE_REMOVE;
}

/*==========================================================================*
 * NFC HAL I/O
 *==========================================================================*/

static
gboolean
pn54x_hal_io_start(
    NciHalIo* hal_io,
    NciHalClient* client)
{
    Pn54xIo* self = pn54x_hal_io_cast(hal_io);

    GASSERT(!self->read_pid);
    if (pn54x_io_open(self)) {
        int fd[2];

        if (pipe(fd) == 0) {
            /*
             * The driver is primitive, read is blocking, we can't cancel
             * the read - the only thing we can do is to perform the read
             * in a separate process and kill it when we no longer need it.
             */
            self->client = client;
            self->read_pid = fork();
            if (self->read_pid > 0) {
                close(fd[1]);
                self->read_fd = fd[0];
                self->read_channel = g_io_channel_unix_new(self->read_fd);
                if (self->read_channel) {
                    g_io_channel_set_flags(self->read_channel,
                        G_IO_FLAG_NONBLOCK, NULL);
                    g_io_channel_set_encoding(self->read_channel, NULL, NULL);
                    g_io_channel_set_buffered(self->read_channel, FALSE);
                    self->read_watch_id = g_io_add_watch(self->read_channel,
                        G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
                        pn54x_io_read_callback, self);
                    GDEBUG("Started read process %d", self->read_pid);
                    return TRUE;
                }
            } else if (self->read_pid == 0) {
                gssize size;
                close(fd[0]);
                GDEBUG("Child %d is running", getpid());
                while (((size = read(self->fd, self->read_tmp_buf,
                    PN54X_MAX_PACKET_SIZE)) > 0)) {
                    GVERBOSE("%d bytes read", (int)size);
                    if (write(fd[1], self->read_tmp_buf, size) < size) {
                        break;
                    }
                }
                /* Normally, it never exits. It gets killed by the parent. */
                GERR("Child exiting: %s", strerror(errno));
                exit(0);
            } else {
                GERR("Failed to start read process: %s", strerror(errno));
                self->read_pid = 0;
                self->client = NULL;
            }
        }
        pn54x_io_close(self);
    }
    return FALSE;
}

static
void
pn54x_hal_io_stop(
    NciHalIo* hal_io)
{
    pn54x_io_stop(pn54x_hal_io_cast(hal_io));
}

static
gboolean
pn54x_hal_io_write(
    NciHalIo* hal_io,
    const GUtilData* chunks,
    guint count,
    NciHalClientFunc callback)
{
    Pn54xIo* self = pn54x_hal_io_cast(hal_io);

    if (pn54x_io_open(self)) {
        const guint8* data = NULL;
        gssize len = 0;

        GASSERT(!self->write_cb);
        GASSERT(!self->write_id);

        if (count == 1) {
            data = chunks->bytes;
            len = chunks->size;
        } else {
            guint i;

            g_byte_array_set_size(self->write_buf, 0);
            for (i = 0; i < count; i++) {
                g_byte_array_append(self->write_buf, chunks[i].bytes,
                    chunks[i].size);
            }
            data = self->write_buf->data;
            len = self->write_buf->len;
        }

        if (write(self->fd, data, len) == len) {
            if (callback) {
                self->write_cb = callback;
                self->write_id = g_idle_add(pn54x_hal_io_write_complete, self);
            }
            return TRUE;
        }
        GERR("Error writing %s: %s", self->dev, strerror(errno));
    }
    return FALSE;
}

static
void
pn54x_hal_io_cancel_write(
    NciHalIo* hal_io)
{
    Pn54xIo* self = pn54x_hal_io_cast(hal_io);

    self->write_cb = NULL;
    if (self->write_id) {
        g_source_remove(self->write_id);
        self->write_id = 0;
    }
}

/*==========================================================================*
 * API
 *=========================================================================*/

Pn54xHalIo*
pn54x_io_new(
    const char* dev)
{
    if (G_LIKELY(dev)) {
        static const NciHalIoFunctions pn54x_hal_io_functions = {
            .start = pn54x_hal_io_start,
            .stop = pn54x_hal_io_stop,
            .write = pn54x_hal_io_write,
            .cancel_write = pn54x_hal_io_cancel_write
        };

        Pn54xIo* self = g_new0(Pn54xIo, 1);
        Pn54xHalIo* io = &self->pn54x;

        g_atomic_int_set(&self->refcount, 1);
        self->fd = -1;
        self->read_fd = -1;
        self->context = g_main_context_default();
        self->read_tmp_buf = g_malloc(PN54X_MAX_PACKET_SIZE);
        self->read_buf = g_byte_array_new();
        self->write_buf = g_byte_array_new();
        io->hal_io.fn = &pn54x_hal_io_functions;
        io->dev = self->dev = g_strdup(dev);

        /* Turn power off (and check if driver is there) */
        if (pn54x_io_set_power(io, FALSE)) {
            return io;
        }
        pn54x_io_free(io);
    }
    return NULL;
}

void
pn54x_io_free(
    Pn54xHalIo* io)
{
    if (G_LIKELY(io)) {
        pn54x_io_unref(pn54x_io_cast(io));
    }
}

gboolean
pn54x_io_set_power(
    Pn54xHalIo* io,
    gboolean on)
{
    if (G_LIKELY(io)) {
        Pn54xIo* self = pn54x_io_cast(io);

        if (pn54x_io_open(self)) {
            const unsigned long pwr = on ? PN54X_PWR_ON : PN54X_PWR_OFF;

            if (pn54x_system_ioctl(self->fd, PN54X_SET_PWR, pwr) >= 0) {
                GDEBUG("Power %s", on ? "on" : "off");
                if (!on) {
                    pn54x_io_close(self);
                }
                return TRUE;
            }
            GERR("PN54X_SET_PWR(%lu) error: %s", pwr, strerror(errno));
        }
    }
    return FALSE;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
