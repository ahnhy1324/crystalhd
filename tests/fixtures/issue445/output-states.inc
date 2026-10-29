/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  See LICENSES/README.md for more information.
 */

enum OUTPUT_STATES
{
  O_TOP = 0,                      // 0
  O_TOP_ERROR,                    // 1
  O_TOP_UNCONFIGURED,             // 2
  O_TOP_CONFIGURED,               // 3
  O_TOP_CONFIGURED_IDLE,          // 4
  O_TOP_CONFIGURED_WORK,          // 5
  O_TOP_CONFIGURED_STEP1,         // 6
  O_TOP_CONFIGURED_STEP2,         // 7
  O_TOP_CONFIGURED_OUTPUT,        // 8
};

int VAAPI_OUTPUT_parentStates[] = {
    -1,
    0, //TOP_ERROR
    0, //TOP_UNCONFIGURED
    0, //TOP_CONFIGURED
    3, //TOP_CONFIGURED_IDLE
    3, //TOP_CONFIGURED_WORK
    3, //TOP_CONFIGURED_STEP1
    3, //TOP_CONFIGURED_STEP2
    3, //TOP_CONFIGURED_OUTPUT
};
