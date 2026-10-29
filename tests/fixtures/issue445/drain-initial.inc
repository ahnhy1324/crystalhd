/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  See LICENSES/README.md for more information.
 */

  bool drain = (m_codecControl & DVD_CODEC_CTRL_DRAIN);

  m_bufferStats.Get(decoded, processed, render, vpp);
  // if all pics are drained, break the loop by setting VC_EOF
  if (drain && decoded <= 0 && processed <= 0 && render <= 0)
    return CDVDVideoCodec::VC_EOF;
