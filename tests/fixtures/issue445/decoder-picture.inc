/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  See LICENSES/README.md for more information.
 */

      if (msg->signal == COutputDataProtocol::PICTURE)
      {
        if (m_presentPicture)
        {
          m_presentPicture->Release();
          m_presentPicture = nullptr;
        }

        m_presentPicture = *(CVaapiRenderPicture**)msg->data;
        m_bufferStats.DecRender();
        m_bufferStats.SetParams(0, m_codecControl);
        msg->Release();
        return CDVDVideoCodec::VC_PICTURE;
      }