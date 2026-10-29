/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  See LICENSES/README.md for more information.
 */

void COutput::Flush()
{
  Message *msg;
  while (m_dataPort.ReceiveOutMessage(&msg))
  {
    if (msg->signal == COutputDataProtocol::NEWFRAME)
    {
      CPayloadWrap<CVaapiDecodedPicture> *payload;
      payload = dynamic_cast<CPayloadWrap<CVaapiDecodedPicture>*>(msg->payloadObj.get());
      if (payload)
      {
        CVaapiDecodedPicture pic = *(payload->GetPlayload());
        m_config.videoSurfaces->ClearRender(pic.videoSurface);
      }
    }
    else if (msg->signal == COutputDataProtocol::RETURNPIC)
    {
      CVaapiRenderPicture *pic;
      pic = *((CVaapiRenderPicture**)msg->data);
      QueueReturnPicture(pic);
    }
    msg->Release();
  }

  while (m_dataPort.ReceiveInMessage(&msg))
  {
    if (msg->signal == COutputDataProtocol::PICTURE)
    {
      CVaapiRenderPicture *pic;
      pic = *((CVaapiRenderPicture**)msg->data);
      pic->Release();
    }
    msg->Release();
  }

  for (unsigned int i = 0; i < m_bufferPool->decodedPics.size(); i++)
  {
    m_config.videoSurfaces->ClearRender(m_bufferPool->decodedPics[i].videoSurface);
  }
  m_bufferPool->decodedPics.clear();

  for (unsigned int i = 0; i < m_bufferPool->processedPics.size(); i++)
  {
    ReleaseProcessedPicture(m_bufferPool->processedPics[i]);
  }
  m_bufferPool->processedPics.clear();

  if (m_pp)
    m_pp->Flush();
}
