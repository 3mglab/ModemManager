/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef MM_SMS_ALTAIR_LTE_H
#define MM_SMS_ALTAIR_LTE_H

#include "mm-base-modem.h"
#include "mm-base-sms.h"

#define MM_TYPE_SMS_ALTAIR_LTE (mm_sms_altair_lte_get_type ())
G_DECLARE_FINAL_TYPE (MMSmsAltairLte, mm_sms_altair_lte, MM, SMS_ALTAIR_LTE, MMBaseSms)

MMBaseSms *mm_sms_altair_lte_new (MMBaseModem *modem,
                                  gboolean is_3gpp,
                                  MMSmsStorage default_storage);

#endif
