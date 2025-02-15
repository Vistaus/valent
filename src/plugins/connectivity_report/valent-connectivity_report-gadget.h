// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2021 Andy Holmes <andrew.g.r.holmes@gmail.com>

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define VALENT_TYPE_CONNECTIVITY_REPORT_GADGET (valent_connectivity_report_gadget_get_type())

G_DECLARE_FINAL_TYPE (ValentConnectivityReportGadget, valent_connectivity_report_gadget, VALENT, CONNECTIVITY_REPORT_GADGET, GtkWidget)

G_END_DECLS
