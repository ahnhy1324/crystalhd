/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  See LICENSES/README.md for more information.
 */

    case O_TOP_CONFIGURED_OUTPUT:
      if (port == NULL) // timeout
      {
        switch (signal)
        {
        case COutputControlProtocol::TIMEOUT:
          if (!m_bufferPool->processedPics.empty())
          {
            CVaapiRenderPicture *outPic;
            CVaapiProcessedPicture procPic;
            procPic = m_bufferPool->processedPics.front();
            m_bufferPool->processedPics.pop_front();
            outPic = ProcessPicture(procPic);
            if (outPic)
            {
              m_config.stats->IncRender();
              m_dataPort.SendInMessage(COutputDataProtocol::PICTURE, &outPic, sizeof(outPic));
            }
            m_config.stats->DecProcessed();
          }
          m_state = O_TOP_CONFIGURED_IDLE;
          m_extTimeout = 0ms;
          return;
        default:
          break;
        }
      }
      break;
