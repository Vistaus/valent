// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2021 Andy Holmes <andrew.g.r.holmes@gmail.com>

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define VALENT_CORE_INSIDE

#include "valent-core-enums.h"

#include "valent-application-plugin.h"
#include "valent-certificate.h"
#include "valent-channel.h"
#include "valent-channel-service.h"
#include "valent-component.h"
#include "valent-data.h"
#include "valent-debug.h"
#include "valent-device.h"
#include "valent-device-manager.h"
#include "valent-device-plugin.h"
#include "valent-device-transfer.h"
#include "valent-global.h"
#include "valent-macros.h"
#include "valent-object.h"
#include "valent-packet.h"
#include "valent-transfer.h"
#include "valent-version.h"

#undef VALENT_CORE_INSIDE

G_END_DECLS

