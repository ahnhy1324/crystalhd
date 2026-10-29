/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  See LICENSES/README.md for more information.
 */

    case O_TOP_CONFIGURED:
      if (port == &m_controlPort)
      {
        switch (signal)
        {
        case COutputControlProtocol::FLUSH:
          Flush();
          msg->Reply(COutputControlProtocol::ACC);
          m_state = O_TOP_CONFIGURED_IDLE;
          return;
        case COutputControlProtocol::PRECLEANUP:
          Flush();
          ReleaseBufferPool(true);
          msg->Reply(COutputControlProtocol::ACC);
          m_state = O_TOP_UNCONFIGURED;
          m_extTimeout = 10s;
          return;
        default:
          break;
        }
      }
      else if (port == &m_dataPort)
      {
        switch (signal)
        {
        case COutputDataProtocol::NEWFRAME:
          CPayloadWrap<CVaapiDecodedPicture> *payload;
          payload = dynamic_cast<CPayloadWrap<CVaapiDecodedPicture>*>(msg->payloadObj.get());
          if (payload)
          {
            m_bufferPool->decodedPics.push_back(*(payload->GetPlayload()));
            m_extTimeout = 0ms;
          }
          return;
        case COutputDataProtocol::RETURNPIC:
          CVaapiRenderPicture *pic;
          pic = *((CVaapiRenderPicture**)msg->data);
          QueueReturnPicture(pic);
          m_controlPort.SendInMessage(COutputControlProtocol::STATS);
          m_extTimeout = 0ms;
          return;
        case COutputDataProtocol::RETURNPROCPIC:
          int id;
          id = *((int*)msg->data);
          ProcessReturnProcPicture(id);
          m_extTimeout = 0ms;
          return;
        default:
          break;
        }
      }
      break;
