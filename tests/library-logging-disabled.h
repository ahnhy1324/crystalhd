// SPDX-License-Identifier: LGPL-2.1-or-later
// Test-only sink replacement; production logging defaults are unchanged.
#include "7411d.h"
#include "libcrystalhd_priv.h"
#undef DebugLog_Trace
#define DebugLog_Trace(...)
#define CRYSTALHD_TEST_DIL_LOG_DISABLED 1
