// SPDX-License-Identifier: GPL-2.0-or-later
// Execute pinned LGPL Kodi lifecycle bodies, with Actor/storage/GPU boundaries
// represented by an identity ledger. This does not launch Kodi or exercise VA.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
using namespace std::chrono_literals;
using CCriticalSection = std::mutex;

static void Require(bool condition, const char* reason)
{
  if (!condition)
    throw std::runtime_error(reason);
}

struct Ledger
{
  std::map<std::string, unsigned> counts;
  std::vector<std::string> events;
  unsigned nextMessage = 0;
  void Record(const std::string& kind, unsigned id)
  {
    const auto key = kind + ':' + std::to_string(id);
    Require(++counts[key] == 1, "an object or message was retired twice");
    events.push_back(key);
  }
  unsigned Count(const std::string& kind, unsigned id) const
  {
    const auto it = counts.find(kind + ':' + std::to_string(id));
    return it == counts.end() ? 0 : it->second;
  }
};

constexpr int DVD_CODEC_CTRL_DRAIN = 0x20000000;
constexpr int LOGERROR = 1;
enum { VAAPI_OPEN, VAAPI_ERROR };
struct CDVDVideoCodec { enum { VC_NONE, VC_PICTURE, VC_EOF }; };
struct COutputControlProtocol { enum { TIMEOUT, FLUSH, ACC, ERROR, PRECLEANUP, STATS }; };
struct COutputDataProtocol { enum { NEWFRAME, PICTURE, RETURNPIC, RETURNPROCPIC, OTHER }; };
struct CLog
{
  static inline std::vector<std::string> errors;
  template<class... Args> static void Log(int, const char* message, Args...)
  {
    errors.emplace_back(message);
  }
};

#include "buffer-stats.inc"
#include "output-states.inc"

using StatValues = std::array<uint64_t, 8>;
static StatValues Values(const CVaapiBufferStats& stats)
{
  return {stats.decodedPics, stats.processedPics, stats.renderPics, stats.latency,
          static_cast<uint64_t>(stats.codecFlags), stats.canSkipDeint,
          static_cast<uint64_t>(stats.processCmd), stats.isVpp};
}

struct CVaapiProcessedPicture;
struct Source
{
  Ledger& ledger;
  void ClearRef(CVaapiProcessedPicture& picture);
};
struct CVaapiDecodedPicture { unsigned videoSurface; };
struct CVaapiProcessedPicture { int id; Source* source; };
void Source::ClearRef(CVaapiProcessedPicture& picture)
{
  ledger.Record("processed-ref", picture.id);
}
struct CVaapiRenderPicture
{
  unsigned id;
  CVaapiProcessedPicture procPic;
  bool valid = true;
  bool ready = false;
  void* avFrame = nullptr;
  unsigned references = 1;
  Ledger& ledger;
  std::function<void(CVaapiRenderPicture*)> returnPicture;
  void Release()
  {
    Require(references == 1, "render reference released twice");
    references = 0;
    ledger.Record("render-release", id);
    returnPicture(this);
  }
};
static void av_frame_unref(void*)
{
  throw std::runtime_error("fixture unexpectedly requires an AVFrame/GPU boundary");
}

struct Payload { virtual ~Payload() = default; };
template<class T> struct CPayloadWrap : Payload
{
  T value;
  explicit CPayloadWrap(T item) : value(item) {}
  T* GetPlayload() { return &value; } // Kodi API spelling.
};
struct WrongPayload : Payload {};
struct Message
{
  Ledger& ledger;
  unsigned id;
  int signal;
  CVaapiRenderPicture* picture = nullptr;
  int processedId = 0;
  void* data = nullptr;
  std::unique_ptr<Payload> payloadObj;
  std::function<void(int)> onReply;
  Message(Ledger& value, int code) : ledger(value), id(++value.nextMessage), signal(code) {}
  void Release() { ledger.Record("message-release", id); }
  void Reply(int code)
  {
    ledger.events.push_back("reply:" + std::to_string(code));
    Require(bool(onReply), "unexpected message reply");
    onReply(code);
  }
};
struct Protocol { std::string portName; };
struct DataPort : Protocol
{
  Ledger& ledger;
  std::deque<Message*> incoming, outgoing;
  std::vector<std::unique_ptr<Message>> messages;
  explicit DataPort(Ledger& value) : ledger(value) { portName = "data"; }
  Message* New(int signal)
  {
    messages.push_back(std::make_unique<Message>(ledger, signal));
    return messages.back().get();
  }
  Message* Picture(int signal, CVaapiRenderPicture* picture, bool incomingMessage)
  {
    auto* msg = New(signal);
    msg->picture = picture;
    msg->data = &msg->picture;
    (incomingMessage ? incoming : outgoing).push_back(msg);
    return msg;
  }
  static bool Receive(std::deque<Message*>& queue, Message** message)
  {
    if (queue.empty()) return false;
    *message = queue.front();
    queue.pop_front();
    return true;
  }
  bool ReceiveOutMessage(Message** message) { return Receive(outgoing, message); }
  bool ReceiveInMessage(Message** message) { return Receive(incoming, message); }
  void SendInMessage(int signal, CVaapiRenderPicture** value, size_t bytes)
  {
    Require(signal == COutputDataProtocol::PICTURE && bytes == sizeof(*value),
            "production PICTURE payload changed");
    Picture(signal, *value, true);
  }
};

struct Pool
{
  Ledger& ledger;
  std::deque<CVaapiDecodedPicture> decodedPics;
  std::deque<CVaapiProcessedPicture> processedPics, processedPicsAway;
  std::deque<CVaapiRenderPicture*> returned;
  explicit Pool(Ledger& value) : ledger(value) {}
  void QueueReturnPicture(CVaapiRenderPicture* picture)
  {
    ledger.Record("return-queued", picture->id);
    returned.push_back(picture);
  }
  CVaapiRenderPicture* ProcessSyncPicture()
  {
    // GPU completion is a declared boundary: the test controls readiness,
    // while the extracted production methods perform actual reference removal.
    auto it = std::find_if(returned.begin(), returned.end(), [](auto* pic) { return pic->ready; });
    if (it == returned.end()) return nullptr;
    auto* result = *it;
    returned.erase(it);
    ledger.Record("return-ready", result->id);
    return result;
  }
};
struct VideoSurfaces
{
  Ledger& ledger;
  void ClearRender(unsigned surface) { ledger.Record("surface-clear", surface); }
};
struct Postproc
{
  Ledger& ledger;
  unsigned flushes = 0;
  void Flush() { ledger.events.push_back("postproc-flush"); ++flushes; }
};

struct COutput;
enum class ReplyMode { Accepted, Error, Timeout, LostReply };
struct ControlPort : Protocol
{
  Ledger& ledger;
  COutput& output;
  ReplyMode mode = ReplyMode::Accepted;
  unsigned calls = 0, acknowledgements = 0;
  std::function<void()> atAcknowledgement;
  std::vector<std::unique_ptr<Message>> messages;
  std::deque<Message*> notifications;
  ControlPort(Ledger& value, COutput& owner) : ledger(value), output(owner) { portName = "control"; }
  Message* New(int signal)
  {
    messages.push_back(std::make_unique<Message>(ledger, signal));
    return messages.back().get();
  }
  void SendInMessage(int signal) { notifications.push_back(New(signal)); }
  bool SendOutMessageSync(int signal, Message** reply, std::chrono::milliseconds timeout);
  void RetireNotifications()
  {
    for (auto* message : notifications) message->Release();
    notifications.clear();
  }
};

struct COutput
{
  Ledger& ledger;
  Pool pool;
  Pool* m_bufferPool = &pool;
  VideoSurfaces surfaces;
  struct { CVaapiBufferStats* stats; VideoSurfaces* videoSurfaces; } m_config;
  DataPort m_dataPort;
  ControlPort m_controlPort;
  Postproc postproc;
  Postproc* m_pp = &postproc;
  int m_state = O_TOP_CONFIGURED_OUTPUT;
  std::chrono::milliseconds m_extTimeout{99};
  std::map<unsigned, std::unique_ptr<CVaapiRenderPicture>> pictures;
  std::map<unsigned, CVaapiRenderPicture*> renderByProcessed;
  COutput(Ledger& value, CVaapiBufferStats& stats)
    : ledger(value), pool(value), surfaces{value}, m_config{&stats, &surfaces},
      m_dataPort(value), m_controlPort(value, *this), postproc{value} {}
  CVaapiRenderPicture* AddPicture(unsigned id, int processed, Source& source)
  {
    auto item = std::make_unique<CVaapiRenderPicture>(CVaapiRenderPicture{
        id, {processed, &source}, true, false, nullptr, 1, ledger, {}});
    item->returnPicture = [&](auto* pic) { m_dataPort.Picture(COutputDataProtocol::RETURNPIC, pic, false); };
    auto* result = item.get();
    pictures.emplace(id, std::move(item));
    renderByProcessed.emplace(processed, result);
    return result;
  }
  CVaapiRenderPicture* ProcessPicture(const CVaapiProcessedPicture& picture)
  {
    auto* result = renderByProcessed.at(picture.id);
    pool.processedPicsAway.push_back(picture);
    return result;
  }
  void ReleaseBufferPool(bool) { throw std::runtime_error("PRECLEANUP is outside this fixture"); }
  void StateMachine(int signal, Protocol* port, Message* msg);
  void Flush();
  void ReleaseProcessedPicture(CVaapiProcessedPicture& pic);
  void QueueReturnPicture(CVaapiRenderPicture* pic);
  void ProcessSyncPicture();
  void ProcessReturnPicture(CVaapiRenderPicture* pic);
  void ProcessReturnProcPicture(int id);
  void PumpReturns()
  {
    Message* message;
    while (m_dataPort.ReceiveOutMessage(&message))
    {
      Require(message->signal == COutputDataProtocol::RETURNPIC,
              "old decode input survived the flush");
      StateMachine(message->signal, &m_dataPort, message);
      message->Release();
    }
    m_controlPort.RetireNotifications();
  }
};

#include "output-dispatch.inc"
#include "output-configured.inc"
#ifdef KODI_ORIGINAL_OUTPUT
#include "original-output-branch.inc"
#else
#include "patched-output-branch.inc"
#endif
    default:
      Require(state != O_TOP, "unsupported state/signal reached TOP");
      break;
    }
  }
}
#include "output-flush.inc"
#include "output-returns.inc"

bool ControlPort::SendOutMessageSync(int signal, Message** reply, std::chrono::milliseconds timeout)
{
  Require(signal == COutputControlProtocol::FLUSH && timeout == 2s,
          "decoder FLUSH signal or deadline changed");
  ++calls;
  if (mode == ReplyMode::Timeout) return false;
  if (mode == ReplyMode::Error)
  {
    *reply = New(COutputControlProtocol::ERROR);
    return true;
  }
  auto* request = New(signal);
  unsigned replies = 0;
  request->onReply = [&](int code) {
    Require(code == COutputControlProtocol::ACC, "configured FLUSH did not return ACC");
    Require(++replies == 1, "configured FLUSH acknowledged twice");
    ++acknowledgements;
    if (atAcknowledgement) atAcknowledgement();
  };
  output.StateMachine(signal, this, request);
  request->Release(); // Actor's receive-loop boundary, not part of StateMachine.
  Require(replies == 1, "configured FLUSH did not acknowledge exactly once");
  Require(output.m_state == O_TOP_CONFIGURED_IDLE, "configured FLUSH did not settle in IDLE");
  if (mode == ReplyMode::LostReply) return false;
  *reply = New(COutputControlProtocol::ACC);
  return true;
}

struct CDecoder
{
  std::mutex m_DecoderSection;
  CVaapiBufferStats m_bufferStats;
  COutput m_vaapiOutput;
  CVaapiRenderPicture* m_presentPicture = nullptr;
  bool m_vaapiConfigured = true;
  int m_DisplayState = VAAPI_OPEN;
  int m_getBufferError = 37;
  int m_codecControl = DVD_CODEC_CTRL_DRAIN;
  explicit CDecoder(Ledger& ledger) : m_vaapiOutput(ledger, m_bufferStats) {}
  void Reset();
  int Take(Message* msg)
  {
#include "decoder-picture.inc"
    throw std::runtime_error("unexpected consumer message");
  }
  int DrainInitial()
  {
    uint16_t decoded, processed, render;
    bool vpp;
#include "original-drain-initial.inc"
    return CDVDVideoCodec::VC_NONE;
  }
};
#include "decoder-reset.inc"

static void SeedStats(CVaapiBufferStats& stats)
{
  for (int i = 0; i != 3; ++i) stats.IncDecoded();
  for (int i = 0; i != 2; ++i) stats.IncProcessed();
  stats.IncRender();
  stats.SetParams(777, 19);
  stats.SetCanSkipDeint(true);
  stats.SetCmd(23);
  stats.SetVpp(true);
}

static void AllMessagesRetired(const COutput& output)
{
  for (const auto& message : output.m_dataPort.messages)
    Require(output.ledger.Count("message-release", message->id) == 1,
            "a data message was not retired exactly once");
  for (const auto& message : output.m_controlPort.messages)
    Require(output.ledger.Count("message-release", message->id) == 1,
            "a control message/reply was not retired exactly once");
}

static void FlushResetAndNewEpoch()
{
  Ledger ledger;
  CDecoder decoder(ledger);
  auto& output = decoder.m_vaapiOutput;
  Source source{ledger};
  auto* returned = output.AddPicture(201, 301, source);
  returned->references = 0; // Return was already queued before this capture.
  auto* queued = output.AddPicture(202, 302, source);
  auto* present = output.AddPicture(203, 305, source);
  auto* held = output.AddPicture(204, 306, source);
  for (auto* pic : {returned, queued, present, held}) output.pool.processedPicsAway.push_back(pic->procPic);
  decoder.m_presentPicture = present;
  output.m_dataPort.Picture(COutputDataProtocol::RETURNPIC, returned, false);
  auto* input = output.m_dataPort.New(COutputDataProtocol::NEWFRAME);
  input->payloadObj = std::make_unique<CPayloadWrap<CVaapiDecodedPicture>>(CVaapiDecodedPicture{101});
  output.m_dataPort.outgoing.push_back(input);
  auto* invalid = output.m_dataPort.New(COutputDataProtocol::NEWFRAME);
  invalid->payloadObj = std::make_unique<WrongPayload>();
  output.m_dataPort.outgoing.push_back(invalid);
  output.m_dataPort.Picture(COutputDataProtocol::PICTURE, queued, true);
  output.m_dataPort.incoming.push_back(output.m_dataPort.New(COutputDataProtocol::OTHER));
  output.pool.decodedPics = {{102}, {103}};
  output.pool.processedPics = {{303, &source}, {304, nullptr}};
  SeedStats(decoder.m_bufferStats);
  const auto before = Values(decoder.m_bufferStats);
  output.m_controlPort.atAcknowledgement = [&] {
    Require(Values(decoder.m_bufferStats) == before, "statistics reset before FLUSH acknowledgement");
    Require(output.pool.decodedPics.empty() && output.pool.processedPics.empty() &&
                output.m_dataPort.incoming.empty() && output.postproc.flushes == 1,
            "FLUSH acknowledged before queued cleanup/postproc flush");
    Require(ledger.Count("surface-clear", 101) == 1 && ledger.Count("surface-clear", 102) == 1 &&
                ledger.Count("surface-clear", 103) == 1 && ledger.Count("processed-ref", 303) == 1,
            "FLUSH missed a queued decoded/processed owner");
    Require(ledger.Count("render-release", 202) == 1 && ledger.Count("render-release", 203) == 1 &&
                held->references == 1 && ledger.Count("processed-ref", 306) == 0,
            "queued/present retirement or delayed external ownership changed");
  };
  decoder.Reset();
  Require(Values(decoder.m_bufferStats) == StatValues{} && decoder.m_getBufferError == 0 &&
              decoder.m_DisplayState == VAAPI_OPEN && decoder.m_presentPicture == nullptr,
          "successful Reset did not reset actual statistics/error/present state");
  Require(output.pool.processedPicsAway.size() == 4,
          "Flush fabricated retirement of GPU/external held owners");
  output.m_controlPort.atAcknowledgement = {};
  output.PumpReturns(); // PICTURE Release can enqueue RETURNPIC after Flush's outbound pass.
  Require(output.pool.returned.size() == 3 && output.pool.processedPicsAway.size() == 4,
          "deferred queued returns were lost or released before GPU readiness");
  for (auto* pic : {returned, queued, present})
  {
    pic->ready = true;
    output.ProcessSyncPicture();
    Require(!pic->valid && ledger.Count("processed-ref", pic->procPic.id) == 1,
            "ready return did not retire its actual processed owner");
  }
  Require(held->references == 1 && output.pool.processedPicsAway.size() == 1,
          "external renderer owner did not survive until its explicit return");

  // Start a new output epoch while the old renderer still owns its picture.
  auto* fresh = output.AddPicture(601, 401, source);
  output.pool.processedPics.push_back(fresh->procPic);
  decoder.m_bufferStats.IncProcessed();
  output.m_state = O_TOP_CONFIGURED_OUTPUT;
  output.StateMachine(COutputControlProtocol::TIMEOUT, nullptr, nullptr);
  Require(output.m_dataPort.incoming.size() == 1 && decoder.DrainInitial() != CDVDVideoCodec::VC_EOF,
          "new epoch lost its queued picture or became EOF early");
  held->Release();
  held->ready = true;
  output.PumpReturns();
  Require(decoder.m_bufferStats.renderPics == 1 && ledger.Count("processed-ref", 306) == 1,
          "old renderer return changed the new epoch's render accounting");
  Message* message;
  Require(output.m_dataPort.ReceiveInMessage(&message) &&
              decoder.Take(message) == CDVDVideoCodec::VC_PICTURE &&
              decoder.m_presentPicture == fresh && decoder.DrainInitial() == CDVDVideoCodec::VC_EOF,
          "new epoch consumed a stale picture or failed actual consumer accounting");
  decoder.Reset();
  fresh->ready = true;
  output.PumpReturns();
  output.ProcessSyncPicture();
  Require(output.pool.processedPicsAway.empty() && output.pool.returned.empty() &&
              output.m_dataPort.incoming.empty() && output.m_dataPort.outgoing.empty(),
          "completed epochs retained a picture/return owner");
  for (unsigned id : {301, 302, 303, 305, 306, 401})
    Require(ledger.Count("processed-ref", id) == 1, "an expected processed owner was not retired");
  Require(ledger.Count("processed-ref", 304) == 0, "null processed source gained a fake owner");
  AllMessagesRetired(output);
}

static void ResetOutcomes()
{
  for (auto mode : {ReplyMode::Error, ReplyMode::Timeout, ReplyMode::LostReply})
  {
    Ledger ledger;
    CDecoder decoder(ledger);
    auto& output = decoder.m_vaapiOutput;
    output.m_controlPort.mode = mode;
    Source source{ledger};
    auto* present = output.AddPicture(1, 2, source);
    output.pool.processedPicsAway.push_back(present->procPic);
    decoder.m_presentPicture = present;
    SeedStats(decoder.m_bufferStats);
    const auto before = Values(decoder.m_bufferStats);
    CLog::errors.clear();
    decoder.Reset();
    Require(decoder.m_DisplayState == VAAPI_ERROR && decoder.m_getBufferError == 37 &&
                Values(decoder.m_bufferStats) == before && decoder.m_presentPicture == nullptr &&
                ledger.Count("render-release", 1) == 1,
            "failed Reset erased statistics/error state or lost the present picture");
    Require(CLog::errors.size() == 1 && CLog::errors[0].find(
                mode == ReplyMode::Error ? "flush returned error" : "flush timed out") != std::string::npos,
            "failed Reset lost its error/timeout disposition");
    Require(output.postproc.flushes == (mode == ReplyMode::LostReply ? 1u : 0u),
            "timeout/error fixture fabricated or lost dispatched cleanup");
    present->ready = true;
    output.PumpReturns();
    output.ProcessSyncPicture();
    AllMessagesRetired(output);
  }
  Ledger ledger;
  CDecoder decoder(ledger);
  auto& output = decoder.m_vaapiOutput;
  Source source{ledger};
  auto* present = output.AddPicture(3, 4, source);
  output.pool.processedPicsAway.push_back(present->procPic);
  decoder.m_presentPicture = present;
  decoder.m_vaapiConfigured = false;
  SeedStats(decoder.m_bufferStats);
  const auto before = Values(decoder.m_bufferStats);
  decoder.Reset();
  Require(output.m_controlPort.calls == 0 && output.postproc.flushes == 0 &&
              Values(decoder.m_bufferStats) == before && decoder.m_getBufferError == 37 &&
              decoder.m_presentPicture == nullptr && ledger.Count("render-release", 3) == 1,
          "unconfigured Reset lost its early return/present-release contract");
  present->ready = true;
  output.PumpReturns();
  AllMessagesRetired(output);
}

static void ConfiguredStatesAndRepeat()
{
  for (int state : {O_TOP_CONFIGURED, O_TOP_CONFIGURED_IDLE, O_TOP_CONFIGURED_WORK,
                    O_TOP_CONFIGURED_STEP1, O_TOP_CONFIGURED_STEP2, O_TOP_CONFIGURED_OUTPUT})
  {
    Ledger ledger;
    CDecoder decoder(ledger);
    auto& output = decoder.m_vaapiOutput;
    output.m_state = state;
    decoder.Reset();
    decoder.Reset();
    Require(output.m_controlPort.calls == 2 && output.m_controlPort.acknowledgements == 2 &&
                output.postproc.flushes == 2 && output.m_state == O_TOP_CONFIGURED_IDLE &&
                Values(decoder.m_bufferStats) == StatValues{},
            "configured parent dispatch/repeated empty Reset changed");
    AllMessagesRetired(output);
  }
}

int main()
{
  try
  {
    FlushResetAndNewEpoch();
    ResetOutcomes();
    ConfiguredStatesAndRepeat();
    std::cout << "PASS: actual Flush/FLUSH dispatch/Reset/stats/return/PICTURE bodies; "
                 "queued and delayed owners, ACC ordering, error/timeout, fresh epoch, six states\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
