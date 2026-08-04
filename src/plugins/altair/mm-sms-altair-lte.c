/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <config.h>

#include "mm-base-modem-at.h"
#include "mm-bind.h"
#include "mm-log-object.h"
#include "mm-modem-helpers-altair-lte.h"
#include "mm-sms-altair-lte.h"
#include "mm-sms-part-3gpp.h"

struct _MMSmsAltairLte {
    MMBaseSms parent;
    MMBaseModem *modem;
};

G_DEFINE_TYPE (MMSmsAltairLte, mm_sms_altair_lte, MM_TYPE_BASE_SMS)

typedef struct {
    MMBaseModem *modem;
    GList *current;
} SmsSendContext;

static void
sms_send_context_free (SmsSendContext *ctx)
{
    g_object_unref (ctx->modem);
    g_free (ctx);
}

static gboolean
sms_send_finish (MMBaseSms *self,
                 GAsyncResult *res,
                 GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void sms_send_next_part (GTask *task);

static void
sms_send_ready (MMBaseModem *modem,
                GAsyncResult *res,
                GTask *task)
{
    SmsSendContext *ctx;
    g_autoptr(GError) error = NULL;
    const gchar *response;
    gint reference;

    response = mm_base_modem_at_command_finish (modem, res, &error);
    if (!response) {
        g_task_return_error (task, g_steal_pointer (&error));
        g_object_unref (task);
        return;
    }

    reference = mm_altair_parse_sms_submit_response (response, &error);
    if (reference < 0) {
        g_task_return_error (task, g_steal_pointer (&error));
        g_object_unref (task);
        return;
    }

    ctx = g_task_get_task_data (task);
    mm_sms_part_set_message_reference (ctx->current->data, (guint)reference);
    ctx->current = g_list_next (ctx->current);
    sms_send_next_part (task);
}

static void
sms_load_smsc_ready (MMBaseModem *modem,
                     GAsyncResult *res,
                     GTask *task)
{
    SmsSendContext *ctx;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *smsc = NULL;
    const gchar *response;
    GList *l;

    response = mm_base_modem_at_command_finish (modem, res, &error);
    if (!response) {
        g_task_return_error (task, g_steal_pointer (&error));
        g_object_unref (task);
        return;
    }

    smsc = mm_altair_parse_sms_parameter_record_smsc (response, &error);
    if (!smsc) {
        g_task_return_error (task, g_steal_pointer (&error));
        g_object_unref (task);
        return;
    }

    ctx = g_task_get_task_data (task);
    for (l = ctx->current; l; l = g_list_next (l)) {
        if (!mm_sms_part_get_smsc (l->data))
            mm_sms_part_set_smsc (l->data, smsc);
    }
    sms_send_next_part (task);
}

static void
sms_send_next_part (GTask *task)
{
    MMSmsAltairLte *self;
    SmsSendContext *ctx;
    g_autoptr(GError) error = NULL;
    g_autofree guint8 *pdu = NULL;
    g_autofree gchar *command = NULL;
    guint pdu_len = 0;
    guint tpdu_offset = 0;

    self = g_task_get_source_object (task);
    ctx = g_task_get_task_data (task);
    if (!ctx->current) {
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    /* AT+CSCA is a stub in this firmware, while the proprietary %CMGS path
     * requires a complete PDU with an explicit SMSC. Read record 1 of the
     * active SIM's EF-SMSP; P3=0 asks the modem for the whole record. */
    if (!mm_sms_part_get_smsc (ctx->current->data)) {
        mm_base_modem_at_command (ctx->modem,
                                  "+CRSM=178,28482,1,4,0", 3, FALSE,
                                  (GAsyncReadyCallback)sms_load_smsc_ready,
                                  task);
        return;
    }

    pdu = mm_sms_part_3gpp_get_submit_pdu (ctx->current->data,
                                          &pdu_len,
                                          &tpdu_offset,
                                          self,
                                          &error);
    if (!pdu) {
        g_task_return_error (task, g_steal_pointer (&error));
        g_object_unref (task);
        return;
    }

    command = mm_altair_build_sms_submit_command (pdu, pdu_len, tpdu_offset, &error);
    if (!command) {
        g_task_return_error (task, g_steal_pointer (&error));
        g_object_unref (task);
        return;
    }

    mm_base_modem_at_command (ctx->modem,
                              command,
                              MM_BASE_SMS_DEFAULT_SEND_TIMEOUT,
                              FALSE,
                              (GAsyncReadyCallback)sms_send_ready,
                              task);
}

static void
sms_send (MMBaseSms *sms,
          GAsyncReadyCallback callback,
          gpointer user_data)
{
    MMSmsAltairLte *self = MM_SMS_ALTAIR_LTE (sms);
    SmsSendContext *ctx;
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);
    ctx = g_new0 (SmsSendContext, 1);
    ctx->modem = g_object_ref (self->modem);
    ctx->current = mm_base_sms_get_parts (sms);
    g_task_set_task_data (task, ctx, (GDestroyNotify)sms_send_context_free);
    sms_send_next_part (task);
}

static gboolean
sms_delete_finish (MMBaseSms *self,
                   GAsyncResult *res,
                   GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
sms_delete (MMBaseSms *sms,
            GAsyncReadyCallback callback,
            gpointer user_data)
{
    GTask *task;

    task = g_task_new (sms, NULL, callback, user_data);
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

MMBaseSms *
mm_sms_altair_lte_new (MMBaseModem *modem,
                       gboolean is_3gpp,
                       MMSmsStorage default_storage)
{
    MMSmsAltairLte *self;

    self = g_object_new (MM_TYPE_SMS_ALTAIR_LTE,
                         MM_BIND_TO, G_OBJECT (modem),
                         MM_BASE_SMS_IS_3GPP, is_3gpp,
                         MM_BASE_SMS_DEFAULT_STORAGE, default_storage,
                         NULL);
    self->modem = g_object_ref (modem);
    return MM_BASE_SMS (self);
}

static void
dispose (GObject *object)
{
    MMSmsAltairLte *self = MM_SMS_ALTAIR_LTE (object);

    g_clear_object (&self->modem);
    G_OBJECT_CLASS (mm_sms_altair_lte_parent_class)->dispose (object);
}

static void
mm_sms_altair_lte_init (MMSmsAltairLte *self)
{
}

static void
mm_sms_altair_lte_class_init (MMSmsAltairLteClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    MMBaseSmsClass *base_sms_class = MM_BASE_SMS_CLASS (klass);

    object_class->dispose = dispose;
    base_sms_class->send = sms_send;
    base_sms_class->send_finish = sms_send_finish;
    base_sms_class->delete = sms_delete;
    base_sms_class->delete_finish = sms_delete_finish;
}
