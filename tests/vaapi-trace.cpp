// SPDX-License-Identifier: LGPL-2.1-or-later
// Device-free validation of the opt-in VA-API JSONL trace writer.
#include "../filters/vaapi/crystalhd-trace.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>

using crystalhd_vaapi_trace::Event;
using crystalhd_vaapi_trace::Fields;
using crystalhd_vaapi_trace::Trace;

static void Require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}

static size_t FileBytes(FILE *file) {
  struct stat info = {};
  Require(fstat(fileno(file), &info) == 0, "inspect trace output");
  return static_cast<size_t>(info.st_size);
}

int main() {
  FILE *disabled = tmpfile();
  Require(disabled != nullptr, "create disabled trace sink");
  unsetenv("CRYSTALHD_VAAPI_TRACE");
  {
    Trace trace(fileno(disabled));
    trace.Emit(Event::SubmitBind);
  }
  setenv("CRYSTALHD_VAAPI_TRACE", "0", 1);
  {
    Trace trace(fileno(disabled));
    trace.Emit(Event::SubmitBind);
  }
  Require(FileBytes(disabled) == 0,
          "unset and non-1 trace settings must remain silent");
  fclose(disabled);

  setenv("CRYSTALHD_VAAPI_TRACE", "1", 1);
  FILE *enabled = tmpfile();
  Require(enabled != nullptr, "create enabled trace sink");
  Trace trace(fileno(enabled));
  Fields identity;
  identity.context = 7;
  identity.generation = 3;
  identity.surface = 11;
  identity.token = 400000;
  identity.decode_identity = 19;
  identity.owner = 11;
  identity.submission_ordinal = 4;

  trace.Emit(Event::SubmitBind, identity);
  trace.Emit(Event::InputSent, identity);
  trace.Emit(Event::PrefeedSent, identity);
  trace.Emit(Event::OutputDequeued, identity);
  trace.Emit(Event::Materialized, identity);
  trace.Emit(Event::BindingEnd, identity);
  identity.operation = 10;
  trace.Emit(Event::SyncEnter, identity);
  identity.duration_ns = 250;
  trace.Emit(Event::SyncExit, identity);
  identity.operation = 11;
  identity.duration_ns = 0;
  trace.Emit(Event::ExportEnter, identity);
  identity.duration_ns = 500;
  trace.Emit(Event::ExportExit, identity);
  identity.operation = 12;
  identity.duration_ns = 0;
  trace.Emit(Event::VppCapture, identity);
  trace.Emit(Event::VppCommit, identity);
  trace.Emit(Event::ContextDestroy, identity);
  trace.Emit(Event::SurfaceDestroy, identity);

  constexpr unsigned int kThreads = 4;
  constexpr unsigned int kEventsPerThread = 16;
  std::vector<std::thread> workers;
  for (unsigned int thread = 0; thread < kThreads; ++thread) {
    workers.emplace_back([&, thread] {
      Fields fields = identity;
      fields.operation = 100 + thread;
      fields.duration_ns = 0;
      for (unsigned int event = 0; event < kEventsPerThread; ++event)
        trace.Emit(Event::OutputDequeued, fields);
    });
  }
  for (auto &worker : workers)
    worker.join();

  Require(FileBytes(enabled) != 0, "enabled trace must produce output");
  Require(fseek(enabled, 0, SEEK_SET) == 0, "rewind enabled trace");
  char buffer[4096];
  for (;;) {
    const size_t bytes = fread(buffer, 1, sizeof(buffer), enabled);
    if (bytes != 0)
      Require(fwrite(buffer, 1, bytes, stdout) == bytes,
              "forward trace fixture");
    if (bytes != sizeof(buffer)) {
      Require(feof(enabled) != 0, "read enabled trace");
      break;
    }
  }
  fclose(enabled);
  unsetenv("CRYSTALHD_VAAPI_TRACE");
  return 0;
}
