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

#include "pn54x_plugin_p.h"
#include "pn54x_log.h"
#include "pn54x_io.h"

#include <nci_adapter_impl.h>

#include <nci_core.h>
#include <nci_hal.h>

typedef struct pn54x_nfc_adapter Pn54xNfcAdapter;
typedef NciAdapterClass Pn54xNfcAdapterClass;

struct pn54x_nfc_adapter {
    NciAdapter adapter;
    Pn54xHalIo* io;
    gboolean need_power;
    gboolean power_on;
    gboolean power_switch_pending;
};

G_DEFINE_TYPE(Pn54xNfcAdapter, pn54x_nfc_adapter, NCI_TYPE_ADAPTER)
#define PN54X_NFC_TYPE_ADAPTER (pn54x_nfc_adapter_get_type())
#define PN54X_NFC_ADAPTER(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), \
        PN54X_NFC_TYPE_ADAPTER, Pn54xNfcAdapter))
#define SUPER_CLASS pn54x_nfc_adapter_parent_class

/*==========================================================================*
 * Implementation
 *==========================================================================*/

static
gboolean
pn54x_nfc_adapter_can_power_off(
    Pn54xNfcAdapter* self)
{
    NciCore* nci = self->adapter.nci;

    return (nci->current_state <= NCI_RFST_IDLE);
}

static
void
pn54x_nfc_adapter_state_check(
    Pn54xNfcAdapter* self)
{
    if (self->power_on && !self->need_power &&
        pn54x_nfc_adapter_can_power_off(self)) {
        pn54x_io_set_power(self->io, FALSE);
        self->power_on = FALSE;
        if (self->power_switch_pending) {
            self->power_switch_pending = FALSE;
            nfc_adapter_power_notify(NFC_ADAPTER(self), FALSE, TRUE);
        } else {
            nfc_adapter_power_notify(NFC_ADAPTER(self), FALSE, FALSE);
        }
    }
}

/*==========================================================================*
 * Interface
 *==========================================================================*/

NfcAdapter*
pn54x_nfc_adapter_new(
    const char* dev)
{
    Pn54xHalIo* io = pn54x_io_new(dev);

    if (io) {
        Pn54xNfcAdapter* self = g_object_new(PN54X_NFC_TYPE_ADAPTER, NULL);

        self->io = io;
        nci_adapter_init_base(&self->adapter, &io->hal_io);
        return NFC_ADAPTER(self);
    }
    return NULL;
}

/*==========================================================================*
 * Methods
 *==========================================================================*/

static
void
pn54x_nfc_adapter_current_state_changed(
    NciAdapter* adapter)
{
    NCI_ADAPTER_CLASS(SUPER_CLASS)->current_state_changed(adapter);
    pn54x_nfc_adapter_state_check(PN54X_NFC_ADAPTER(adapter));
}

static
void
pn54x_nfc_adapter_next_state_changed(
    NciAdapter* adapter)
{
    Pn54xNfcAdapter* self = PN54X_NFC_ADAPTER(adapter);
    NciCore* nci = adapter->nci;

    NCI_ADAPTER_CLASS(SUPER_CLASS)->next_state_changed(adapter);
    if (nci->next_state != NCI_RFST_POLL_ACTIVE) {
        if (nci->next_state == NCI_STATE_ERROR && self->power_on) {
            GDEBUG("Resetting the chip");
            pn54x_io_set_power(self->io, FALSE);
            pn54x_io_set_power(self->io, TRUE);
        }
    }
    pn54x_nfc_adapter_state_check(self);
}

static
gboolean
pn54x_nfc_adapter_submit_power_request(
    NfcAdapter* adapter,
    gboolean on)
{
    Pn54xNfcAdapter* self = PN54X_NFC_ADAPTER(adapter);
    NciCore* nci = self->adapter.nci;

    GASSERT(!self->power_switch_pending);
    self->need_power = on;
    if (on) {
        if (self->power_on) {
            GDEBUG("Power is already on");
            nci_core_set_state(nci, NCI_RFST_IDLE);
            /* Power stays on, we are done */
            nfc_adapter_power_notify(NFC_ADAPTER(self), TRUE, TRUE);
        } else if (pn54x_io_set_power(self->io, TRUE)) {
            self->power_on = TRUE;
            nci_core_restart(nci);
            nfc_adapter_power_notify(NFC_ADAPTER(self), TRUE, TRUE);
        }
    } else {
        if (self->power_on) {
            if (pn54x_nfc_adapter_can_power_off(self)) {
                pn54x_io_set_power(self->io, FALSE);
                self->power_on = FALSE;
                nfc_adapter_power_notify(NFC_ADAPTER(self), FALSE, TRUE);
            } else {
                GDEBUG("Waiting for NCI state machine to become idle");
                nci_core_set_state(nci, NCI_RFST_IDLE);
                self->power_switch_pending =
                    (nci->current_state != NCI_RFST_IDLE &&
                    nci->next_state == NCI_RFST_IDLE);
            }
        } else {
            GDEBUG("Power is already off");
            /* Power stays off, we are done */
            nfc_adapter_power_notify(NFC_ADAPTER(self), FALSE, TRUE);
        }
    }
    return self->power_switch_pending;
}

static
void
pn54x_nfc_adapter_cancel_power_request(
    NfcAdapter* adapter)
{
    Pn54xNfcAdapter* self = PN54X_NFC_ADAPTER(adapter);

    self->need_power = self->power_on;
    self->power_switch_pending = FALSE;
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
pn54x_nfc_adapter_init(
    Pn54xNfcAdapter* self)
{
}

static
void
pn54x_nfc_adapter_finalize(
    GObject* object)
{
    Pn54xNfcAdapter* self = PN54X_NFC_ADAPTER(object);

    nci_adapter_finalize_core(&self->adapter);
    pn54x_io_free(self->io);
    G_OBJECT_CLASS(SUPER_CLASS)->finalize(object);
}

static
void
pn54x_nfc_adapter_class_init(
    Pn54xNfcAdapterClass* klass)
{
    GObjectClass* object_class = G_OBJECT_CLASS(klass);
    NfcAdapterClass* nfc_adapter_class = NFC_ADAPTER_CLASS(klass);

    klass->current_state_changed = pn54x_nfc_adapter_current_state_changed;
    klass->next_state_changed = pn54x_nfc_adapter_next_state_changed;
    nfc_adapter_class->submit_power_request =
        pn54x_nfc_adapter_submit_power_request;
    nfc_adapter_class->cancel_power_request =
        pn54x_nfc_adapter_cancel_power_request;
    object_class->finalize = pn54x_nfc_adapter_finalize;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
