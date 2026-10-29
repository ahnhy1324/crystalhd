// SPDX-License-Identifier: GPL-2.0-or-later
// Compile the extracted Kodi branch unchanged into a small protocol scaffold.
// Included Kodi source excerpts retain their LGPL-2.1-or-later notices.
// This is a source-unit schedule test, not a Kodi or VA-driver integration run.
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using namespace std::chrono_literals;

static void Require(bool okay, const char* reason)
{
  if (!okay)
    throw std::runtime_error(reason);
}

struct CDVDVideoCodec { enum { VC_NONE = 0, VC_PICTURE = 4, VC_EOF = 8 }; };
constexpr int DVD_CODEC_CTRL_DRAIN = 0x20000000;
struct COutputControlProtocol { enum { TIMEOUT = 1 }; };
struct COutputDataProtocol { enum { PICTURE = 0 }; };
enum State { O_TOP_CONFIGURED_OUTPUT, O_TOP_CONFIGURED_IDLE };

struct Snapshot
{
  int decoded, processed, render;
  bool empty() const { return decoded == 0 && processed == 0 && render == 0; }
};

struct Stats
{
  std::mutex mutex;
  int decoded = 0, processed = 0, render = 0, remaining = 0;
  unsigned increments = 0, retirements = 0, takes = 0;
  bool premature_zero = false;
  std::vector<Snapshot> history;

  void Record()
  {
    Require(decoded >= 0 && processed >= 0 && render >= 0,
            "a buffer counter underflowed");
    const Snapshot state{decoded, processed, render};
    premature_zero |= remaining != 0 && state.empty();
    history.push_back(state);
  }
  void IncRender()
  {
    std::lock_guard<std::mutex> lock(mutex);
    ++render;
    ++increments;
    Record();
  }
  void DecProcessed()
  {
    std::lock_guard<std::mutex> lock(mutex);
    --processed;
    ++retirements;
    Record();
  }
  void DecRender()
  {
    std::lock_guard<std::mutex> lock(mutex);
    --remaining;
    --render;
    ++takes;
    Record();
  }
  void Get(uint16_t& d, uint16_t& p, uint16_t& r, bool& vpp)
  {
    std::lock_guard<std::mutex> lock(mutex);
    d = decoded;
    p = processed;
    r = render;
    vpp = false;
  }
  Snapshot Read()
  {
    std::lock_guard<std::mutex> lock(mutex);
    return {decoded, processed, render};
  }
};

struct CVaapiProcessedPicture { unsigned id; bool valid; };
struct CVaapiRenderPicture { unsigned id; };
struct Pool { std::deque<CVaapiProcessedPicture> processedPics; };

struct DataPort
{
  Stats& stats;
  std::mutex mutex;
  std::condition_variable condition;
  std::deque<CVaapiRenderPicture*> queue;
  bool controlled = false, canceled = false;
  unsigned before = 0, release_before = 0, sent = 0, release_after = 0;

  explicit DataPort(Stats& value) : stats(value) {}
  void SendInMessage(int signal, CVaapiRenderPicture** payload, size_t size)
  {
    Require(signal == COutputDataProtocol::PICTURE && payload && *payload &&
                size == sizeof(*payload), "the published picture payload changed");
    std::unique_lock<std::mutex> lock(mutex);
    const unsigned number = ++before;
    condition.notify_all();
    if (controlled)
      Require(condition.wait_for(lock, 2s, [&] {
        return canceled || release_before >= number;
      }) && !canceled, "consumer did not inspect the pre-publication counters");
    queue.push_back(*payload);
    ++sent;
    condition.notify_all();
    // Both versions stop at the same point: the real PICTURE is visible but
    // SendInMessage has not returned to producer housekeeping yet.
    if (controlled)
      Require(condition.wait_for(lock, 2s, [&] {
        return canceled || release_after >= number;
      }) && !canceled, "consumer did not finish its next drain observation");
  }
  void Before(unsigned number)
  {
    std::unique_lock<std::mutex> lock(mutex);
    Require(condition.wait_for(lock, 2s, [&] { return before >= number; }),
            "producer did not reach the publication boundary");
  }
  void Publish(unsigned number)
  {
    std::lock_guard<std::mutex> lock(mutex);
    release_before = number;
    condition.notify_all();
  }
  unsigned Take()
  {
    std::unique_lock<std::mutex> lock(mutex);
    Require(condition.wait_for(lock, 2s, [&] { return !queue.empty(); }),
            "consumer did not receive a real published picture");
    auto* picture = queue.front();
    queue.pop_front();
    stats.DecRender();
    return picture->id;
  }
  void Resume(unsigned number)
  {
    std::lock_guard<std::mutex> lock(mutex);
    release_after = number;
    condition.notify_all();
  }
  void Cancel()
  {
    std::lock_guard<std::mutex> lock(mutex);
    canceled = true;
    condition.notify_all();
  }
};

struct Output
{
  Stats stats;
  Pool pool;
  Pool* m_bufferPool = &pool;
  struct { Stats* stats; } m_config{&stats};
  Stats& m_bufferStats = stats;
  DataPort m_dataPort{stats};
  State m_state = O_TOP_CONFIGURED_OUTPUT;
  std::chrono::milliseconds m_extTimeout{99};
  int m_codecControl = DVD_CODEC_CTRL_DRAIN;
  std::vector<CVaapiRenderPicture> pictures;
  unsigned process_calls = 0;

  Output(unsigned count, int extra_decoded = 0, int null_id = -1)
  {
    stats.decoded = extra_decoded;
    stats.processed = count;
    stats.remaining = extra_decoded;
    pictures.reserve(count);
    for (unsigned i = 0; i < count; ++i)
    {
      pool.processedPics.push_back({i, static_cast<int>(i) != null_id});
      stats.remaining += static_cast<int>(i) != null_id;
      pictures.push_back({i});
    }
  }
  CVaapiRenderPicture* ProcessPicture(const CVaapiProcessedPicture& picture)
  {
    ++process_calls;
    return picture.valid ? &pictures.at(picture.id) : nullptr;
  }
  void Original(int signal, void* port)
  {
    switch (m_state)
    {
#include "original-output-branch.inc"
      default: throw std::runtime_error("invalid output state");
    }
  }
  void Patched(int signal, void* port)
  {
    switch (m_state)
    {
#include "patched-output-branch.inc"
      default: throw std::runtime_error("invalid output state");
    }
  }
  int DrainInitial()
  {
    uint16_t decoded, processed, render;
    bool vpp;
#include "original-drain-initial.inc"
    return CDVDVideoCodec::VC_NONE; // The actual Kodi loop would continue.
  }
};

using Branch = void (Output::*)(int, void*);

static std::vector<Snapshot> ControlledHandoff(Branch branch, unsigned count,
                                                int extra_decoded = 0)
{
  Output output(count, extra_decoded);
  output.m_dataPort.controlled = true;
  std::exception_ptr error;
  std::thread producer([&] {
    try
    {
      for (unsigned i = 0; i < count; ++i)
      {
        output.m_state = O_TOP_CONFIGURED_OUTPUT;
        (output.*branch)(COutputControlProtocol::TIMEOUT, nullptr);
      }
    }
    catch (...) { error = std::current_exception(); }
  });
  struct Join
  {
    DataPort& port;
    std::thread& thread;
    ~Join() { port.Cancel(); if (thread.joinable()) thread.join(); }
  } join{output.m_dataPort, producer};
  std::vector<Snapshot> taken;
  for (unsigned i = 1; i <= count; ++i)
  {
    output.m_dataPort.Before(i);
    const auto before = output.stats.Read();
    Require(before.render == 1 && !before.empty() &&
                output.DrainInitial() != CDVDVideoCodec::VC_EOF,
            "an unpublished picture lost its render counter or caused early EOF");
    output.m_dataPort.Publish(i);
    Require(output.m_dataPort.Take() == i - 1,
            "PICTURE identity or output order changed");
    taken.push_back(output.stats.Read());
    const bool last = i == count && extra_decoded == 0;
    const bool candidate = branch == &Output::Patched;
    Require((output.DrainInitial() == CDVDVideoCodec::VC_EOF) == (candidate && last),
            "controlled drain did not expose the expected old/new publication ordering");
    output.m_dataPort.Resume(i);
  }
  producer.join();
  if (error) std::rethrow_exception(error);
  Require(output.stats.retirements == count && output.stats.increments == count &&
              output.stats.takes == count && !output.stats.premature_zero &&
              output.stats.Read().processed == 0 && output.stats.Read().render == 0 &&
              output.stats.Read().decoded == extra_decoded &&
              output.m_state == O_TOP_CONFIGURED_IDLE && output.m_extTimeout == 0ms,
          "housekeeping did not preserve counters, state, or timeout after delivery");
  return taken;
}

static void OrdinaryAndNull(Branch branch)
{
  for (int null_id : {-1, 1})
  {
    Output output(3, 0, null_id);
    for (unsigned i = 0; i != 3; ++i)
    {
      output.m_state = O_TOP_CONFIGURED_OUTPUT;
      (output.*branch)(COutputControlProtocol::TIMEOUT, nullptr);
    }
    const unsigned count = null_id < 0 ? 3 : 2;
    Require(output.stats.retirements == 3 && output.stats.increments == count &&
                output.process_calls == 3 && output.m_dataPort.sent == count &&
                output.stats.Read().processed == 0 && output.stats.Read().render == int(count),
            "normal/null output changed its retirement or publication count");
    for (unsigned i = 0; i != 3; ++i)
    {
      if (int(i) == null_id) continue;
      Require(output.DrainInitial() != CDVDVideoCodec::VC_EOF &&
                  output.m_dataPort.Take() == i,
              "queued pictures were skipped, duplicated, or declared drained early");
    }
    Require(output.stats.Read().empty() && !output.stats.premature_zero &&
                output.DrainInitial() == CDVDVideoCodec::VC_EOF,
            "normal queued drain did not finish after all real pictures");
    output.m_state = O_TOP_CONFIGURED_OUTPUT;
    (output.*branch)(COutputControlProtocol::TIMEOUT, nullptr);
    Require(output.stats.retirements == 3 && output.m_dataPort.sent == count,
            "empty processed queue repeated retirement or publication");
  }
  Output failed(1, 0, 0);
  (failed.*branch)(COutputControlProtocol::TIMEOUT, nullptr);
  Require(failed.stats.retirements == 1 && failed.stats.increments == 0 &&
              failed.m_dataPort.sent == 0 && failed.stats.Read().empty(),
          "null ProcessPicture must retire once without synthesizing output");
}

int main(int argc, char** argv)
{
  try
  {
    const bool require_old_correct = argc == 2 &&
        std::strcmp(argv[1], "--require-original-correct") == 0;
    Require(argc == 1 || require_old_correct, "unsupported test argument");
    const auto old_one = ControlledHandoff(&Output::Original, 1);
    const auto new_one = ControlledHandoff(&Output::Patched, 1);
    Require(old_one[0].processed == 1 && new_one[0].empty(),
            "the controlled last-picture counterexample was not reproduced");
    std::cout << "last picture at same post-publication barrier: original processed=1, "
                 "candidate processed=0/render=0\n";
    if (require_old_correct)
      Require(old_one[0].empty(), "original branch leaves a stale processed count after last take");
    const auto old_two = ControlledHandoff(&Output::Original, 2);
    const auto new_two = ControlledHandoff(&Output::Patched, 2);
    Require(old_two[0].processed == 2 && old_two[1].processed == 1 &&
                new_two[0].processed == 1 && new_two[1].empty(),
            "two-picture/Bob delivery lost its remaining output accounting");
    const auto old_partial = ControlledHandoff(&Output::Original, 1, 1);
    const auto new_partial = ControlledHandoff(&Output::Patched, 1, 1);
    Require(old_partial[0].decoded == 1 && old_partial[0].processed == 1 &&
                new_partial[0].decoded == 1 && new_partial[0].processed == 0,
            "an unfinished decoded picture was treated as EOF");
    OrdinaryAndNull(&Output::Original);
    OrdinaryAndNull(&Output::Patched);
    std::cout << "PASS: actual extracted branches, two-thread last/two/partial handoffs, "
                 "pre-publication counter fence, queued identity/counts, null and empty queue\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
