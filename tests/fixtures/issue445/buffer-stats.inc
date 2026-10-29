/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  See LICENSES/README.md for more information.
 */

class CVaapiBufferStats
{
public:
  uint16_t decodedPics{};
  uint16_t processedPics{};
  uint16_t renderPics{};
  uint64_t latency{}; // time decoder has waited for a frame, ideally there is no latency
  int codecFlags{};
  bool canSkipDeint{};
  int processCmd{};
  bool isVpp{};

  void IncDecoded()
  {
    std::unique_lock<CCriticalSection> l(m_sec);
    decodedPics++;
  }
  void DecDecoded()
  {
    std::unique_lock<CCriticalSection> l(m_sec);
    decodedPics--;
  }
  void IncProcessed()
  {
    std::unique_lock<CCriticalSection> l(m_sec);
    processedPics++;
  }
  void DecProcessed()
  {
    std::unique_lock<CCriticalSection> l(m_sec);
    processedPics--;
  }
  void IncRender()
  {
    std::unique_lock<CCriticalSection> l(m_sec);
    renderPics++;
  }
  void DecRender()
  {
    std::unique_lock<CCriticalSection> l(m_sec);
    renderPics--;
  }
  void Reset()
  {
    std::unique_lock<CCriticalSection> l(m_sec);
    decodedPics = 0;
    processedPics = 0;
    renderPics = 0;
    latency = 0;
    codecFlags = 0;
    canSkipDeint = false;
    processCmd = 0;
    isVpp = false;
  }
  void Get(uint16_t& decoded, uint16_t& processed, uint16_t& render, bool& vpp)
  {
    std::unique_lock<CCriticalSection> l(m_sec);
    decoded = decodedPics, processed = processedPics, render = renderPics;
    vpp = isVpp;
  }
  void SetParams(uint64_t time, int flags)
  {
    std::unique_lock<CCriticalSection> l(m_sec);
    latency = time;
    codecFlags = flags;
  }
  void GetParams(uint64_t& lat, int& flags)
  {
    std::unique_lock<CCriticalSection> l(m_sec);
    lat = latency;
    flags = codecFlags;
  }
  void SetCmd(int cmd)
  {
    std::unique_lock<CCriticalSection> l(m_sec);
    processCmd = cmd;
  }
  void GetCmd(int& cmd)
  {
    std::unique_lock<CCriticalSection> l(m_sec);
    cmd = processCmd;
    processCmd = 0;
  }
  void SetCanSkipDeint(bool canSkip)
  {
    std::unique_lock<CCriticalSection> l(m_sec);
    canSkipDeint = canSkip;
  }
  bool CanSkipDeint()
  {
    std::unique_lock<CCriticalSection> l(m_sec);
    if (canSkipDeint)
      return true;
    else
      return false;
  }
  void SetVpp(bool vpp)
  {
    std::unique_lock<CCriticalSection> l(m_sec);
    isVpp = vpp;
  }

private:
  CCriticalSection m_sec;
};
