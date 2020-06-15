// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/util/debug-util.h"

#include <execinfo.h>
#include <dirent.h>
#include <signal.h>
#include <sys/syscall.h>

#ifdef __linux__
#include <link.h>
#include <cxxabi.h>
#endif // __linux__

#ifdef __linux__
#include <backtrace.h>
#endif // __linux__

#include <string>
#include <iostream>
#include <regex>
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <queue>
#include <sstream>

#include <glog/logging.h>

#include "yb/gutil/linux_syscall_support.h"
#include "yb/gutil/macros.h"
#include "yb/gutil/singleton.h"
#include "yb/gutil/spinlock.h"
#include "yb/gutil/stringprintf.h"
#include "yb/gutil/strings/numbers.h"
#include "yb/gutil/strtoint.h"

#include "yb/util/enums.h"
#include "yb/util/env.h"
#include "yb/util/errno.h"
#include "yb/util/lockfree.h"
#include "yb/util/memory/memory.h"
#include "yb/util/monotime.h"
#include "yb/util/thread.h"
#include "yb/util/string_trim.h"

using namespace std::literals;

#if defined(__linux__) && !defined(NDEBUG)
constexpr bool kDefaultUseLibbacktrace = true;
#else
constexpr bool kDefaultUseLibbacktrace = false;
#endif

DEFINE_bool(use_libbacktrace, kDefaultUseLibbacktrace,
            "Whether to use the libbacktrace library for symbolizing stack traces");

#if defined(__APPLE__)
typedef sig_t sighandler_t;
#endif

// Evil hack to grab a few useful functions from glog
namespace google {

extern int GetStackTrace(void** result, int max_depth, int skip_count);

// Symbolizes a program counter.  On success, returns true and write the
// symbol name to "out".  The symbol name is demangled if possible
// (supports symbols generated by GCC 3.x or newer).  Otherwise,
// returns false.
bool Symbolize(void *pc, char *out, int out_size);

namespace glog_internal_namespace_ {
extern void DumpStackTraceToString(std::string *s);
} // namespace glog_internal_namespace_
} // namespace google

// The %p field width for printf() functions is two characters per byte.
// For some environments, add two extra bytes for the leading "0x".
static const int kPrintfPointerFieldWidth = 2 + 2 * sizeof(void*);

using std::string;

namespace yb {

// https://gcc.gnu.org/onlinedocs/libstdc++/libstdc++-html-USERS-4.3/a01696.html
enum DemangleStatus : int {
  kDemangleOk = 0,
  kDemangleMemAllocFailure = -1,
  kDemangleInvalidMangledName = -2,
  kDemangleInvalidArgument = -3
};

namespace {

YB_DEFINE_ENUM(ThreadStackState, (kNone)(kSendFailed)(kReady));

struct ThreadStackEntry : public MPSCQueueEntry<ThreadStackEntry> {
  ThreadIdForStack tid;
  StackTrace stack;
};

#if !defined(__APPLE__) && !THREAD_SANITIZER
#define USE_FUTEX 1
#else
#define USE_FUTEX 0
#endif

class CompletionFlag {
 public:
  void Signal() {
    complete_.store(1, std::memory_order_release);
#if USE_FUTEX
    sys_futex(reinterpret_cast<int32_t*>(&complete_),
              FUTEX_WAKE | FUTEX_PRIVATE_FLAG,
              INT_MAX, // wake all
              0 /* ignored */);
#endif
  }

  bool TimedWait(MonoDelta timeout) {
    if (complete()) {
      return true;
    }

    auto now = MonoTime::Now();
    auto deadline = now + timeout;
#if !USE_FUTEX
    auto wait_time = 10ms;
#endif
    while (now < deadline) {
#if USE_FUTEX
      struct timespec ts;
      (deadline - now).ToTimeSpec(&ts);
      kernel_timespec kernel_ts;
      ts.tv_sec = ts.tv_sec;
      ts.tv_nsec = ts.tv_nsec;
      sys_futex(reinterpret_cast<int32_t*>(&complete_),
                FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
                0, // wait if value is still 0
                &kernel_ts);
#else
      std::this_thread::sleep_for(wait_time);
      wait_time = std::min(wait_time * 2, 100ms);
#endif
      if (complete()) {
        return true;
      }
      now = MonoTime::Now();
    }

    return complete();
  }

  void Reset() {
    complete_.store(0, std::memory_order_release);
  }

  bool complete() const {
    return complete_.load(std::memory_order_acquire);
  }
 private:
  std::atomic<int32_t> complete_ { 0 };
};

// Global structure used to communicate between the signal handler
// and a dumping thread.
struct ThreadStackHelper {
  std::mutex mutex; // Locked by ThreadStacks, so only one could be executed in parallel.

  LockFreeStack<ThreadStackEntry> collected;
  // Reuse previously allocated memory. We expect this size to be merely small, near 152 bytes
  // per application thread.
  LockFreeStack<ThreadStackEntry> allocated;
  CompletionFlag completion_flag;

  // Could be modified only by ThreadStacks.
  CoarseTimePoint deadline;
  size_t allocated_entries = 0;

  // Incremented by each signal handler.
  std::atomic<int64_t> left_to_collect;

  std::vector<std::unique_ptr<ThreadStackEntry[]>> allocated_chunks;

  void SetNumEntries(size_t len) {
    len += 5; // We reserve several entries, because threads from previous request could still be
              // processing signal and write their results.
    if (len <= allocated_entries) {
      return;
    }

    size_t new_chunk_size = std::max<size_t>(len - allocated_entries, 0x10);
    allocated_chunks.emplace_back(new ThreadStackEntry[new_chunk_size]);
    allocated_entries += new_chunk_size;

    for (auto entry = allocated_chunks.back().get(), end = entry + new_chunk_size; entry != end;
         ++entry) {
      allocated.Push(entry);
    }
  }

  void StoreResult(
      const std::vector<ThreadIdForStack>& tids, std::vector<Result<StackTrace>>* out) {
    // We give the thread ~1s to respond. In testing, threads typically respond within
    // a few iterations of the loop, so this timeout is very conservative.
    //
    // The main reason that a thread would not respond is that it has blocked signals. For
    // example, glibc's timer_thread doesn't respond to our signal, so we always time out
    // on that one.
    if (left_to_collect.load(std::memory_order_acquire) > 0) {
      completion_flag.TimedWait(1s);
    }

    while (auto entry = collected.Pop()) {
      auto it = std::lower_bound(tids.begin(), tids.end(), entry->tid);
      if (it != tids.end() && *it == entry->tid) {
        (*out)[it - tids.begin()] = entry->stack;
      }
      allocated.Push(entry);
    }
  }

  void RecordStackTrace(const StackTrace& stack_trace) {
    auto* entry = allocated.Pop();
    if (!entry) { // Not enough allocated entries, don't write log since we are in signal handler.
      return;
    }
    entry->tid = Thread::CurrentThreadIdForStack();
    entry->stack = stack_trace;
    collected.Push(entry);

    if (left_to_collect.fetch_sub(1, std::memory_order_acq_rel) - 1 == 0) {
      completion_flag.Signal();
    }
  }
};

ThreadStackHelper thread_stack_helper;

// Signal handler for our stack trace signal.
// We expect that the signal is only sent from DumpThreadStack() -- not by a user.

void HandleStackTraceSignal(int signum) {
  int old_errno = errno;
  StackTrace stack_trace;
  stack_trace.Collect(2);

  thread_stack_helper.RecordStackTrace(stack_trace);
  errno = old_errno;
}

// The signal that we'll use to communicate with our other threads.
// This can't be in used by other libraries in the process.
int g_stack_trace_signum = SIGUSR2;

bool InitSignalHandlerUnlocked(int signum) {
  enum InitState {
    UNINITIALIZED,
    INIT_ERROR,
    INITIALIZED
  };
  static InitState state = UNINITIALIZED;

  // If we've already registered a handler, but we're being asked to
  // change our signal, unregister the old one.
  if (signum != g_stack_trace_signum && state == INITIALIZED) {
    struct sigaction old_act;
    PCHECK(sigaction(g_stack_trace_signum, nullptr, &old_act) == 0);
    if (old_act.sa_handler == &HandleStackTraceSignal) {
      signal(g_stack_trace_signum, SIG_DFL);
    }
  }

  // If we'd previously had an error, but the signal number
  // is changing, we should mark ourselves uninitialized.
  if (signum != g_stack_trace_signum) {
    g_stack_trace_signum = signum;
    state = UNINITIALIZED;
  }

  if (state == UNINITIALIZED) {
    struct sigaction old_act;
    PCHECK(sigaction(g_stack_trace_signum, nullptr, &old_act) == 0);
    if (old_act.sa_handler != SIG_DFL &&
        old_act.sa_handler != SIG_IGN) {
      state = INIT_ERROR;
      LOG(WARNING) << "signal handler for stack trace signal "
                   << g_stack_trace_signum
                   << " is already in use: "
                   << "YB will not produce thread stack traces.";
    } else {
      // No one appears to be using the signal. This is racy, but there is no
      // atomic swap capability.
      sighandler_t old_handler = signal(g_stack_trace_signum, HandleStackTraceSignal);
      if (old_handler != SIG_IGN &&
          old_handler != SIG_DFL) {
        LOG(FATAL) << "raced against another thread installing a signal handler";
      }
      state = INITIALIZED;
    }
  }
  return state == INITIALIZED;
}

const char kUnknownSymbol[] = "(unknown)";
const char kStackTraceEntryFormat[] = "    @ %*p  %s";

#ifdef __linux__

// Remove path prefixes up to what looks like the root of the YB source tree, or the source tree
// of other recognizable codebases.
const char* NormalizeSourceFilePath(const char* file_path) {
  if (file_path == nullptr) {
    return file_path;
  }

  // Remove the leading "../../../" stuff.
  while (strncmp(file_path, "../", 3) == 0) {
    file_path += 3;
  }

  // This could be called arbitrarily early or late in program execution as part of backtrace,
  // so we're not using any std::string static constants here.
#define YB_HANDLE_SOURCE_SUBPATH(subpath, prefix_len_to_remove) \
    do { \
      const char* const subpath_ptr = strstr(file_path, (subpath)); \
      if (subpath_ptr != nullptr) { \
        return subpath_ptr + (prefix_len_to_remove); \
      } \
    } while (0);

  YB_HANDLE_SOURCE_SUBPATH("/src/yb/", 5);
  YB_HANDLE_SOURCE_SUBPATH("/src/postgres/src/", 5);
  YB_HANDLE_SOURCE_SUBPATH("/src/rocksdb/", 5);
  YB_HANDLE_SOURCE_SUBPATH("/thirdparty/build/", 1);

  // These are Linuxbrew gcc's standard headers. Keep the path starting from "gcc/...".
  YB_HANDLE_SOURCE_SUBPATH("/Cellar/gcc/", 8);

  // TODO: replace postgres_build with just postgres.
  YB_HANDLE_SOURCE_SUBPATH("/postgres_build/src/", 1);

#undef YB_HANDLE_SOURCE_SUBPATH

  return file_path;
}

struct SymbolizationContext {
  StackTraceLineFormat stack_trace_line_format = StackTraceLineFormat::DEFAULT;
  string* buf = nullptr;
};

void BacktraceErrorCallback(void* data, const char* msg, int errnum) {
  bool reported = false;
  string* buf_ptr = nullptr;
  if (data) {
    auto* context = static_cast<SymbolizationContext*>(data);
    if (context->buf) {
      buf_ptr = context->buf;
      buf_ptr->append(StringPrintf("Backtrace error: %s (errnum=%d)\n", msg, errnum));
      reported = true;
    }
  }

  if (!reported) {
    // A backup mechanism for error reporting.
    fprintf(stderr, "%s called with data=%p, msg=%s, errnum=%d, buf_ptr=%p\n", __func__,
            data, msg, errnum, buf_ptr);
  }
}

class GlobalBacktraceState {
 public:
  GlobalBacktraceState() {
    bt_state_ = backtrace_create_state(
        /* filename */ nullptr,
        /* threaded = */ 0,
        BacktraceErrorCallback,
        /* data */ nullptr);

    // To complete initialization we should call backtrace, otherwise it could fail in case of
    // concurrent initialization.
    backtrace_full(bt_state_, /* skip = */ 1, DummyCallback,
                   BacktraceErrorCallback, nullptr);
  }

  backtrace_state* GetState() { return bt_state_; }

  std::mutex* mutex() { return &mutex_; }

 private:
  static int DummyCallback(void *const data, const uintptr_t pc,
                    const char* const filename, const int lineno,
                    const char* const original_function_name) {
    return 0;
  }

  struct backtrace_state* bt_state_;
  std::mutex mutex_;
};

int BacktraceFullCallback(void *const data, const uintptr_t pc,
                          const char* const filename, const int lineno,
                          const char* const original_function_name) {
  assert(data != nullptr);
  const SymbolizationContext& context = *pointer_cast<SymbolizationContext*>(data);
  string* const buf = context.buf;
  int demangle_status = 0;
  char* const demangled_function_name =
      original_function_name != nullptr ?
      abi::__cxa_demangle(original_function_name,
                          nullptr,  // output_buffer
                          nullptr,  // length
                          &demangle_status) :
      nullptr;
  const char* function_name_to_use = original_function_name;
  if (original_function_name != nullptr) {
    if (demangle_status != kDemangleOk) {
      if (demangle_status != kDemangleInvalidMangledName) {
        // -2 means the mangled name is not a valid name under the C++ ABI mangling rules.
        // This happens when the name is e.g. "main", so we don't report the error.
        StringAppendF(buf, "Error: __cxa_demangle failed for '%s' with error code %d\n",
            original_function_name, demangle_status);
      }
      // Regardless of the exact reason for demangle failure, we use the original function name
      // provided by libbacktrace.
    } else if (demangled_function_name != nullptr) {
      // If __cxa_demangle returns 0 and a non-null string, we use that instead of the original
      // function name.
      function_name_to_use = demangled_function_name;
    } else {
      StringAppendF(buf,
          "Error: __cxa_demangle returned zero status but nullptr demangled function for '%s'\n",
          original_function_name);
    }
  }

  string pretty_function_name;
  if (function_name_to_use == nullptr) {
    pretty_function_name = kUnknownSymbol;
  } else {
    // Allocating regexes on the heap so that they would never get deallocated. This is because
    // the kernel watchdog thread could still be symbolizing stack traces as global destructors
    // are being called.
    static const std::regex* kStdColonColonOneRE = new std::regex("\\bstd::__1::\\b");
    pretty_function_name = std::regex_replace(function_name_to_use, *kStdColonColonOneRE, "std::");

    static const std::regex* kStringRE = new std::regex(
        "\\bstd::basic_string<char, std::char_traits<char>, std::allocator<char> >");
    pretty_function_name = std::regex_replace(pretty_function_name, *kStringRE, "string");

    static const std::regex* kRemoveStdPrefixRE =
        new std::regex("\\bstd::(string|tuple|shared_ptr|unique_ptr)\\b");
    pretty_function_name = std::regex_replace(pretty_function_name, *kRemoveStdPrefixRE, "$1");
  }

  const bool is_symbol_only_fmt =
      context.stack_trace_line_format == StackTraceLineFormat::SYMBOL_ONLY;

  // We have not appended an end-of-line character yet. Let's see if we have file name / line number
  // information first. BTW kStackTraceEntryFormat is used both here and in glog-based
  // symbolization.
  if (filename == nullptr) {
    // libbacktrace failed to produce an address. We still need to produce a line of the form:
    // "    @     0x7f2d98f9bd87  "
    char hex_pc_buf[32];
    snprintf(hex_pc_buf, sizeof(hex_pc_buf), "0x%" PRIxPTR, pc);
    if (is_symbol_only_fmt) {
      // At least print out the hex address, otherwise we'd end up with an empty string.
      *buf += hex_pc_buf;
    } else {
      StringAppendF(buf, "    @ %18s ", hex_pc_buf);
    }
  } else {
    const string frame_without_file_line =
        is_symbol_only_fmt ? pretty_function_name
                           : StringPrintf(
                               kStackTraceEntryFormat, kPrintfPointerFieldWidth,
                               reinterpret_cast<void*>(pc), pretty_function_name.c_str());

    // Got filename and line number from libbacktrace! No need to filter the output through
    // addr2line, etc.
    switch (context.stack_trace_line_format) {
      case StackTraceLineFormat::CLION_CLICKABLE: {
        const string file_line_prefix = StringPrintf("%s:%d: ", filename, lineno);
        StringAppendF(buf, "%-100s", file_line_prefix.c_str());
        *buf += frame_without_file_line;
        break;
      }
      case StackTraceLineFormat::SHORT: {
        *buf += frame_without_file_line;
        StringAppendF(buf, " (%s:%d)", NormalizeSourceFilePath(filename), lineno);
        break;
      }
      case StackTraceLineFormat::SYMBOL_ONLY: {
        *buf += frame_without_file_line;
        StringAppendF(buf, " (%s:%d)", NormalizeSourceFilePath(filename), lineno);
        break;
      }
    }
  }

  buf->push_back('\n');

  // No need to check for nullptr, free is a no-op in that case.
  free(demangled_function_name);
  return 0;
}
#endif  // __linux__

bool IsDoubleUnderscoredAndInList(
    const char* symbol, const std::initializer_list<const char*>& list) {
  if (symbol[0] != '_' || symbol[1] != '_') {
    return false;
  }
  for (const auto* idle_function : list) {
    if (!strcmp(symbol, idle_function)) {
      return true;
    }
  }
  return false;
}

bool IsIdle(const char* symbol) {
  return IsDoubleUnderscoredAndInList(symbol,
                                      { "__GI_epoll_wait",
                                        "__pthread_cond_timedwait",
                                        "__pthread_cond_wait" });
}

bool IsWaiting(const char* symbol) {
  return IsDoubleUnderscoredAndInList(symbol,
                                      { "__GI___pthread_mutex_lock" });
}

}  // anonymous namespace

Status SetStackTraceSignal(int signum) {
  std::lock_guard<decltype(thread_stack_helper.mutex)> lock(thread_stack_helper.mutex);
  if (!InitSignalHandlerUnlocked(signum)) {
    return STATUS(InvalidArgument, "Unable to install signal handler");
  }
  return Status::OK();
}

Result<StackTrace> ThreadStack(ThreadIdForStack tid) {
  return ThreadStacks({tid}).front();
}

std::vector<Result<StackTrace>> ThreadStacks(const std::vector<ThreadIdForStack>& tids) {
  static const Status status = STATUS(
      RuntimeError, "Thread did not respond: maybe it is blocking signals");

  std::vector<Result<StackTrace>> result(tids.size(), status);
  std::lock_guard<std::mutex> execution_lock(thread_stack_helper.mutex);

  // Ensure that our signal handler is installed. We don't need any fancy GoogleOnce here
  // because of the mutex above.
  if (!InitSignalHandlerUnlocked(g_stack_trace_signum)) {
    static const Status status = STATUS(
        RuntimeError, "Unable to take thread stack: signal handler unavailable");
    std::fill_n(result.begin(), tids.size(), status);
    return result;
  }

  thread_stack_helper.left_to_collect.store(tids.size(), std::memory_order_release);
  thread_stack_helper.SetNumEntries(tids.size());
  thread_stack_helper.completion_flag.Reset();

  for (size_t i = 0; i != tids.size(); ++i) {
    // We use the raw syscall here instead of kill() to ensure that we don't accidentally
    // send a signal to some other process in the case that the thread has exited and
    // the TID been recycled.
#if defined(__linux__)
    int res = syscall(SYS_tgkill, getpid(), tids[i], g_stack_trace_signum);
#else
    int res = pthread_kill(tids[i], g_stack_trace_signum);
#endif
    if (res != 0) {
      static const Status status = STATUS(
          RuntimeError, "Unable to deliver signal: process may have exited");
      result[i] = status;
      thread_stack_helper.left_to_collect.fetch_sub(1, std::memory_order_acq_rel);
    }
  }

  thread_stack_helper.StoreResult(tids, &result);

  return result;
}

std::string DumpThreadStack(ThreadIdForStack tid) {
  auto stack_trace = ThreadStack(tid);
  if (!stack_trace.ok()) {
    return stack_trace.status().message().ToBuffer();
  }
  return stack_trace->Symbolize();
}

Status ListThreads(std::vector<pid_t> *tids) {
#if defined(__linux__)
  DIR *dir = opendir("/proc/self/task/");
  if (dir == NULL) {
    return STATUS(IOError, "failed to open task dir", Errno(errno));
  }
  struct dirent *d;
  while ((d = readdir(dir)) != NULL) {
    if (d->d_name[0] != '.') {
      uint32_t tid;
      if (!safe_strtou32(d->d_name, &tid)) {
        LOG(WARNING) << "bad tid found in procfs: " << d->d_name;
        continue;
      }
      tids->push_back(tid);
    }
  }
  closedir(dir);
#endif // defined(__linux__)
  return Status::OK();
}

std::string GetStackTrace(StackTraceLineFormat stack_trace_line_format,
                          int num_top_frames_to_skip) {
  std::string buf;
#ifdef __linux__
  if (FLAGS_use_libbacktrace) {
    SymbolizationContext context;
    context.buf = &buf;
    context.stack_trace_line_format = stack_trace_line_format;

    // Use libbacktrace on Linux because that gives us file names and line numbers.
    auto* global_backtrace_state = Singleton<GlobalBacktraceState>::get();
    struct backtrace_state* const backtrace_state = global_backtrace_state->GetState();

    // Avoid multi-threaded access to libbacktrace which causes high memory consumption.
    std::lock_guard<std::mutex> l(*global_backtrace_state->mutex());

    // TODO: https://yugabyte.atlassian.net/browse/ENG-4729

    const int backtrace_full_rv = backtrace_full(
        backtrace_state, /* skip = */ num_top_frames_to_skip + 1, BacktraceFullCallback,
        BacktraceErrorCallback, &context);
    if (backtrace_full_rv != 0) {
      StringAppendF(&buf, "Error: backtrace_full return value is %d", backtrace_full_rv);
    }
    return buf;
  }
#endif
  google::glog_internal_namespace_::DumpStackTraceToString(&buf);
  return buf;
}

std::string GetStackTraceHex() {
  char buf[1024];
  HexStackTraceToString(buf, 1024);
  return std::string(buf);
}

void HexStackTraceToString(char* buf, size_t size) {
  StackTrace trace;
  trace.Collect(1);
  trace.StringifyToHex(buf, size);
}

string GetLogFormatStackTraceHex() {
  StackTrace trace;
  trace.Collect(1);
  return trace.ToLogFormatHexString();
}

void StackTrace::Collect(int skip_frames) {
#if THREAD_SANITIZER
  num_frames_ = google::GetStackTrace(frames_, arraysize(frames_), skip_frames);
#else
  int max_frames = skip_frames + arraysize(frames_);
  void** buffer = static_cast<void**>(alloca((max_frames) * sizeof(void*)));
  num_frames_ = backtrace(buffer, max_frames);
  if (num_frames_ > skip_frames) {
    num_frames_ -= skip_frames;
    memmove(frames_, buffer + skip_frames, num_frames_ * sizeof(void*));
  } else {
    num_frames_ = 0;
  }
#endif
}

void StackTrace::StringifyToHex(char* buf, size_t size, int flags) const {
  char* dst = buf;

  // Reserve kHexEntryLength for the first iteration of the loop, 1 byte for a
  // space (which we may not need if there's just one frame), and 1 for a nul
  // terminator.
  char* limit = dst + size - kHexEntryLength - 2;
  for (int i = 0; i < num_frames_ && dst < limit; i++) {
    if (i != 0) {
      *dst++ = ' ';
    }
    // See note in Symbolize() below about why we subtract 1 from each address here.
    uintptr_t addr = reinterpret_cast<uintptr_t>(frames_[i]);
    if (!(flags & NO_FIX_CALLER_ADDRESSES)) {
      addr--;
    }
    FastHex64ToBuffer(addr, dst);
    dst += kHexEntryLength;
  }
  *dst = '\0';
}

string StackTrace::ToHexString(int flags) const {
  // Each frame requires kHexEntryLength, plus a space
  // We also need one more byte at the end for '\0'
  char buf[kMaxFrames * (kHexEntryLength + 1) + 1];
  StringifyToHex(buf, arraysize(buf), flags);
  return string(buf);
}

// If group is specified it is filled with value corresponding to this stack trace.
void SymbolizeAddress(
    const StackTraceLineFormat stack_trace_line_format,
    void* pc,
    string* buf,
    StackTraceGroup* group = nullptr
#ifdef __linux__
    , GlobalBacktraceState* global_backtrace_state = nullptr
#endif
    ) {
  // The return address 'pc' on the stack is the address of the instruction
  // following the 'call' instruction. In the case of calling a function annotated
  // 'noreturn', this address may actually be the first instruction of the next
  // function, because the function we care about ends with the 'call'.
  // So, we subtract 1 from 'pc' so that we're pointing at the 'call' instead
  // of the return address.
  //
  // For example, compiling a C program with -O2 that simply calls 'abort()' yields
  // the following disassembly:
  //     Disassembly of section .text:
  //
  //     0000000000400440 <main>:
  //       400440:   48 83 ec 08             sub    $0x8,%rsp
  //       400444:   e8 c7 ff ff ff          callq  400410 <abort@plt>
  //
  //     0000000000400449 <_start>:
  //       400449:   31 ed                   xor    %ebp,%ebp
  //       ...
  //
  // If we were to take a stack trace while inside 'abort', the return pointer
  // on the stack would be 0x400449 (the first instruction of '_start'). By subtracting
  // 1, we end up with 0x400448, which is still within 'main'.
  //
  // This also ensures that we point at the correct line number when using addr2line
  // on logged stacks.
  pc = reinterpret_cast<void*>(reinterpret_cast<size_t>(pc) - 1);
#ifdef __linux__
  if (FLAGS_use_libbacktrace) {
    if (!global_backtrace_state) {
      global_backtrace_state = Singleton<GlobalBacktraceState>::get();
    }
    struct backtrace_state* const backtrace_state = global_backtrace_state->GetState();

    // Avoid multi-threaded access to libbacktrace which causes high memory consumption.
    std::lock_guard<std::mutex> l(*global_backtrace_state->mutex());

    SymbolizationContext context;
    context.stack_trace_line_format = stack_trace_line_format;
    context.buf = buf;
    backtrace_pcinfo(backtrace_state, reinterpret_cast<uintptr_t>(pc),
                    BacktraceFullCallback, BacktraceErrorCallback, &context);
    return;
  }
#endif
  char tmp[1024];
  const char* symbol = kUnknownSymbol;

  if (google::Symbolize(pc, tmp, sizeof(tmp))) {
    symbol = tmp;
    if (group) {
      if (IsWaiting(symbol)) {
        *group = StackTraceGroup::kWaiting;
      } else if (IsIdle(symbol)) {
        *group = StackTraceGroup::kIdle;
      }
    }
  }

  StringAppendF(buf, kStackTraceEntryFormat, kPrintfPointerFieldWidth, pc, symbol);
  // We are appending the end-of-line character separately because we want to reuse the same
  // format string for libbacktrace callback and glog-based symbolization, and we have an extra
  // file name / line number component before the end-of-line in the libbacktrace case.
  buf->push_back('\n');
}

// Symbolization function borrowed from glog and modified to use libbacktrace on Linux.
string StackTrace::Symbolize(
    const StackTraceLineFormat stack_trace_line_format, StackTraceGroup* group) const {
  string buf;
#ifdef __linux__
  // Use libbacktrace for symbolization.
  auto* global_backtrace_state =
      FLAGS_use_libbacktrace ? Singleton<GlobalBacktraceState>::get() : nullptr;
#endif

  if (group) {
    *group = StackTraceGroup::kActive;
  }

  for (int i = 0; i < num_frames_; i++) {
    void* const pc = frames_[i];

    SymbolizeAddress(stack_trace_line_format, pc, &buf, group
#ifdef __linux__
        , global_backtrace_state
#endif
        );
  }

  return buf;
}

string StackTrace::ToLogFormatHexString() const {
  string buf;
  for (int i = 0; i < num_frames_; i++) {
    void* pc = frames_[i];
    StringAppendF(&buf, "    @ %*p\n", kPrintfPointerFieldWidth, pc);
  }
  return buf;
}

uint64_t StackTrace::HashCode() const {
  return util_hash::CityHash64(reinterpret_cast<const char*>(frames_),
                               sizeof(frames_[0]) * num_frames_);
}

namespace {
#ifdef __linux__
int DynamcLibraryListCallback(struct dl_phdr_info *info, size_t size, void *data) {
  if (*info->dlpi_name != '\0') {
    // We can't use LOG(...) yet because Google Logging might not be initialized.
    // It is also important to write the entire line at once so that it is less likely to be
    // interleaved with pieces of similar lines from other processes.
    std::cerr << StringPrintf(
        "Shared library '%s' loaded at address 0x%" PRIx64 "\n", info->dlpi_name, info->dlpi_addr);
  }
  return 0;
}
#endif

void PrintLoadedDynamicLibraries() {
#ifdef __linux__
  // Supported on Linux only.
  dl_iterate_phdr(DynamcLibraryListCallback, nullptr);
#endif
}

bool PrintLoadedDynamicLibrariesOnceHelper() {
  const char* list_dl_env_var = std::getenv("YB_LIST_LOADED_DYNAMIC_LIBS");
  if (list_dl_env_var != nullptr && *list_dl_env_var != '\0') {
    PrintLoadedDynamicLibraries();
  }
  return true;
}

} // anonymous namespace

string SymbolizeAddress(void *pc, const StackTraceLineFormat stack_trace_line_format) {
  string s;
  SymbolizeAddress(stack_trace_line_format, pc, &s);
  return s;
}

// ------------------------------------------------------------------------------------------------
// Tracing function calls
// ------------------------------------------------------------------------------------------------

namespace {

const char* GetEnvVar(const char* name) {
  const char* value = getenv(name);
  if (value && strlen(value) == 0) {
    // Treat empty values as undefined.
    return nullptr;
  }
  return value;
}

struct FunctionTraceConf {

  bool enabled = false;

  // Auto-blacklist functions that have done more than this number of calls per second.
  int32_t blacklist_calls_per_sec = 1000;

  std::unordered_set<std::string> fn_name_blacklist;

  FunctionTraceConf() {
    enabled = GetEnvVar("YB_FN_TRACE");
    if (!enabled) {
      return;
    }
    auto blacklist_calls_per_sec_str = GetEnvVar("YB_FN_TRACE_BLACKLIST_CALLS_PER_SEC");
    if (blacklist_calls_per_sec_str) {
      blacklist_calls_per_sec = atoi32(blacklist_calls_per_sec_str);
    }

    static const char* kBlacklistFileEnvVarName = "YB_FN_TRACE_BLACKLIST_FILE";
    auto blacklist_file_path = GetEnvVar(kBlacklistFileEnvVarName);
    if (blacklist_file_path) {
      LOG(INFO) << "Reading blacklist from " << blacklist_file_path;
      std::ifstream blacklist_file(blacklist_file_path);
      if (!blacklist_file) {
        LOG(FATAL) << "Could not read function trace blacklist file from '"
                   << blacklist_file_path << "' as specified by " << kBlacklistFileEnvVarName;
      }
      string fn_name;
      while (std::getline(blacklist_file, fn_name)) {
        fn_name = util::TrimStr(fn_name);
        if (fn_name.size()) {
          fn_name_blacklist.insert(fn_name);
        }
      }

      if (blacklist_file.bad()) {
        LOG(FATAL) << "Error reading blacklist file '" << blacklist_file_path << "': "
                   << strerror(errno);
      }
    }
  }
};

const FunctionTraceConf& GetFunctionTraceConf() {
  static const FunctionTraceConf conf;
  return conf;
}

enum class EventType : uint8_t {
  kEnter,
  kLeave
};

struct FunctionTraceEvent {
  EventType event_type;
  void* this_fn;
  void* call_site;
  int64_t thread_id;
  CoarseMonoClock::time_point time;

  FunctionTraceEvent(
      EventType type_,
      void* this_fn_,
      void* call_site_)
      : event_type(type_),
        this_fn(this_fn_),
        call_site(call_site_),
        thread_id(Thread::CurrentThreadId()),
        time(CoarseMonoClock::Now()) {
  }
};

class FunctionTracer {
 public:
  FunctionTracer() {
  }

  void RecordEvent(EventType event_type, void* this_fn, void* call_site) {
    if (blacklisted_fns_.count(this_fn)) {
      return;
    }
    const auto& conf = GetFunctionTraceConf();

    FunctionTraceEvent event(event_type, this_fn, call_site);
    auto& time_queue = func_event_times_[this_fn];
    while (!time_queue.empty() && time_queue.back() - time_queue.front() > 1000ms) {
      time_queue.pop();
    }
    time_queue.push(event.time);
    if (conf.blacklist_calls_per_sec > 0 &&
        time_queue.size() >= conf.blacklist_calls_per_sec) {
      std::cerr << "Auto-blacklisting " << Symbolize(this_fn) << ": repeated "
                << time_queue.size() << " times per sec" << std::endl;
      blacklisted_fns_.insert(this_fn);
      func_event_times_.erase(this_fn);
      return;
    }
    const string& symbol = Symbolize(event.this_fn);
    if (!conf.fn_name_blacklist.empty()) {
      // User specified a file listing functions that should not be traced. Match the symbolized
      // function name to that file.
      const char* symbol_str = symbol.c_str();
      const char* space_ptr = strchr(symbol_str, ' ');
      if (conf.fn_name_blacklist.count(space_ptr ? std::string(symbol_str, space_ptr)
                                                 : symbol)) {
        // Remember to avoid tracing this function pointer from now on.
        blacklisted_fns_.insert(event.this_fn);
        return;
      }
    }
    const char* event_type_str = event.event_type == EventType::kEnter ? "->" : "<-";
    if (!first_event_) {
      auto delay_ms = ToMilliseconds(event.time - last_event_time_);
      if (delay_ms > 1000) {
        std::ostringstream ss;
        ss << "\n" << std::string(80, '-') << "\n";
        ss << "No events for " << delay_ms << " ms (thread id: " << event.thread_id << ")";
        ss << "\n" << std::string(80, '-') << "\n";
        std::cout << ss.str() << std::endl;
      }
      last_event_time_ = event.time;
    }
    first_event_ = false;
    std::ostringstream ss;
    ss << ToMicroseconds(event.time.time_since_epoch())
       << " " << event.thread_id << " " << event_type_str << " " << symbol
       << ", called from: " << Symbolize(event.call_site);
    std::cerr << ss.str() << std::endl;
  }

 private:
  const std::string& Symbolize(void* pc) {
    {
      auto iter = symbol_cache_.find(pc);
      if (iter != symbol_cache_.end()) {
        return iter->second;
      }
    }
    string buf;
    SymbolizeAddress(StackTraceLineFormat::SYMBOL_ONLY, pc, &buf);
    if (!buf.empty() && buf.back() == '\n') {
      buf.pop_back();
    }
    return symbol_cache_.emplace(pc, buf).first->second;
  }

  std::unordered_map<void*, std::queue<CoarseMonoClock::time_point>> func_event_times_;
  std::unordered_set<void*> blacklisted_fns_;
  CoarseMonoClock::time_point last_event_time_;
  bool first_event_ = true;

  std::unordered_map<void*, std::string> symbol_cache_;
};

FunctionTracer& GetFunctionTracer() {
  static FunctionTracer function_tracer;
  return function_tracer;
}

} // anonymous namespace

// List the load addresses of dynamic libraries once on process startup if required.
const bool  __attribute__((unused)) kPrintedLoadedDynamicLibraries =
    PrintLoadedDynamicLibrariesOnceHelper();

extern "C" {

void __cyg_profile_func_enter(void*, void*) __attribute__((no_instrument_function));
void __cyg_profile_func_enter(void *this_fn, void *call_site) {
  if (GetFunctionTraceConf().enabled) {
    GetFunctionTracer().RecordEvent(EventType::kEnter, this_fn, call_site);
  }
}

void __cyg_profile_func_exit(void*, void*) __attribute__((no_instrument_function));
void __cyg_profile_func_exit (void *this_fn, void *call_site) {
  if (GetFunctionTraceConf().enabled) {
    GetFunctionTracer().RecordEvent(EventType::kLeave, this_fn, call_site);
  }
}

}  // extern "C"

std::string DemangleName(const char* mangled_name) {
  int demangle_status = 0;
  char* demangled_name =
      abi::__cxa_demangle(mangled_name, nullptr /* output_buffer */, nullptr /* length */,
                          &demangle_status);
  string ret_val = demangle_status == kDemangleOk ? demangled_name : mangled_name;
  free(demangled_name);
  return ret_val;
}

std::string SourceLocation::ToString() const {
  return Format("$0:$1", file_name, line_number);
}

}  // namespace yb
