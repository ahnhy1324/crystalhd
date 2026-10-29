/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  See LICENSES/README.md for more information.
 */

void CDecoder::Reset()
{
  std::unique_lock<CCriticalSection> lock(m_DecoderSection);

  if (m_presentPicture)
  {
    m_presentPicture->Release();
    m_presentPicture = nullptr;
  }

  if (!m_vaapiConfigured)
    return;

  Message *reply;
  if (m_vaapiOutput.m_controlPort.SendOutMessageSync(COutputControlProtocol::FLUSH, &reply, 2s))
  {
    bool success = reply->signal == COutputControlProtocol::ACC ? true : false;
    reply->Release();
    if (!success)
    {
      CLog::Log(LOGERROR, "VAAPI::{} - flush returned error", __FUNCTION__);
      m_DisplayState = VAAPI_ERROR;
    }
    else
    {
      m_bufferStats.Reset();
      m_getBufferError = 0;
    }
  }
  else
  {
    CLog::Log(LOGERROR, "VAAPI::{} - flush timed out", __FUNCTION__);
    m_DisplayState = VAAPI_ERROR;
  }
}
