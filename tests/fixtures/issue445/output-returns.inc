/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  See LICENSES/README.md for more information.
 */

void COutput::ReleaseProcessedPicture(CVaapiProcessedPicture &pic)
{
  if (!pic.source)
  {
    return;
  }
  pic.source->ClearRef(pic);
  pic.source = nullptr;
}

void COutput::QueueReturnPicture(CVaapiRenderPicture *pic)
{
  m_bufferPool->QueueReturnPicture(pic);
  ProcessSyncPicture();
}

void COutput::ProcessSyncPicture()
{
  CVaapiRenderPicture *pic;

  pic = m_bufferPool->ProcessSyncPicture();

  if (pic)
  {
    ProcessReturnPicture(pic);
  }
}

void COutput::ProcessReturnPicture(CVaapiRenderPicture *pic)
{
  if (pic->avFrame)
    av_frame_unref(pic->avFrame);

  ProcessReturnProcPicture(pic->procPic.id);
  pic->valid = false;
}

void COutput::ProcessReturnProcPicture(int id)
{
  for (auto it=m_bufferPool->processedPicsAway.begin(); it!=m_bufferPool->processedPicsAway.end(); ++it)
  {
    if (it->id == id)
    {
      ReleaseProcessedPicture(*it);
      m_bufferPool->processedPicsAway.erase(it);
      break;
    }
  }
}
