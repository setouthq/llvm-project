//===-- Program.cpp - Implement OS Program Concept --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file implements the operating system Program concept.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/Program.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#if defined(__wasi__) && defined(LLVM_WASI_ENABLE_SUBPROCESS)
#include "WASI/subprocess_host.h"
#endif
#include <cassert>
#include <cstdlib>
#include <memory>
#include <string>
#include <tuple>
#include <vector>
using namespace llvm;
using namespace sys;

//===----------------------------------------------------------------------===//
//=== WARNING: Implementation here must contain only TRULY operating system
//===          independent code.
//===----------------------------------------------------------------------===//

static bool Execute(ProcessInfo &PI, StringRef Program,
                    ArrayRef<StringRef> Args,
                    std::optional<ArrayRef<StringRef>> Env,
                    ArrayRef<std::optional<StringRef>> Redirects,
                    unsigned MemoryLimit, std::string *ErrMsg,
                    BitVector *AffinityMask, bool DetachProcess);

#if defined(__wasi__) && defined(LLVM_WASI_ENABLE_SUBPROCESS)
static int ExecuteAndWaitWithWasiSubprocess(
    StringRef Program, ArrayRef<StringRef> Args,
    std::optional<ArrayRef<StringRef>> Env,
    ArrayRef<std::optional<StringRef>> Redirects, unsigned SecondsToWait,
    unsigned MemoryLimit, std::string *ErrMsg, bool *ExecutionFailed,
    std::optional<ProcessStatistics> *ProcStat, BitVector *AffinityMask);
#endif

int sys::ExecuteAndWait(StringRef Program, ArrayRef<StringRef> Args,
                        std::optional<ArrayRef<StringRef>> Env,
                        ArrayRef<std::optional<StringRef>> Redirects,
                        unsigned SecondsToWait, unsigned MemoryLimit,
                        std::string *ErrMsg, bool *ExecutionFailed,
                        std::optional<ProcessStatistics> *ProcStat,
                        BitVector *AffinityMask) {
#if defined(__wasi__) && defined(LLVM_WASI_ENABLE_SUBPROCESS)
  return ExecuteAndWaitWithWasiSubprocess(
      Program, Args, Env, Redirects, SecondsToWait, MemoryLimit, ErrMsg,
      ExecutionFailed, ProcStat, AffinityMask);
#else
  assert(Redirects.empty() || Redirects.size() == 3);
  ProcessInfo PI;
  if (Execute(PI, Program, Args, Env, Redirects, MemoryLimit, ErrMsg,
              AffinityMask, /*DetachProcess=*/false)) {
    if (ExecutionFailed)
      *ExecutionFailed = false;
    ProcessInfo Result = Wait(
        PI, SecondsToWait == 0 ? std::nullopt : std::optional(SecondsToWait),
        ErrMsg, ProcStat);
    return Result.ReturnCode;
  }

  if (ExecutionFailed)
    *ExecutionFailed = true;

  return -1;
#endif
}

ProcessInfo sys::ExecuteNoWait(StringRef Program, ArrayRef<StringRef> Args,
                               std::optional<ArrayRef<StringRef>> Env,
                               ArrayRef<std::optional<StringRef>> Redirects,
                               unsigned MemoryLimit, std::string *ErrMsg,
                               bool *ExecutionFailed, BitVector *AffinityMask,
                               bool DetachProcess) {
  assert(Redirects.empty() || Redirects.size() == 3);
  ProcessInfo PI;
  if (ExecutionFailed)
    *ExecutionFailed = false;
  if (!Execute(PI, Program, Args, Env, Redirects, MemoryLimit, ErrMsg,
               AffinityMask, DetachProcess))
    if (ExecutionFailed)
      *ExecutionFailed = true;

  return PI;
}

bool sys::commandLineFitsWithinSystemLimits(StringRef Program,
                                            ArrayRef<const char *> Args) {
  SmallVector<StringRef, 8> StringRefArgs(Args);
  return commandLineFitsWithinSystemLimits(Program, StringRefArgs);
}

void sys::printArg(raw_ostream &OS, StringRef Arg, bool Quote) {
  const bool Escape = Arg.find_first_of(" \"\\$") != StringRef::npos;

  if (!Quote && !Escape) {
    OS << Arg;
    return;
  }

  // Quote and escape. This isn't really complete, but good enough.
  OS << '"';
  for (const auto c : Arg) {
    if (c == '"' || c == '\\' || c == '$')
      OS << '\\';
    OS << c;
  }
  OS << '"';
}

// Include the platform-specific parts of this class.
#if defined(__wasi__)
ProcessInfo::ProcessInfo() : Pid(0), Process(0), ReturnCode(0) {}

#if defined(LLVM_WASI_ENABLE_SUBPROCESS)
namespace {

static subprocess_host_string_t toSubprocessString(StringRef Value) {
  if (Value.empty())
    return subprocess_host_string_t{
        reinterpret_cast<uint8_t *>(const_cast<char *>("")), 0};
  return subprocess_host_string_t{
      reinterpret_cast<uint8_t *>(const_cast<char *>(Value.data())),
      Value.size()};
}

static std::string toStdString(subprocess_host_string_t Value) {
  return std::string(reinterpret_cast<const char *>(Value.ptr), Value.len);
}

static void setErrMsg(std::string *ErrMsg, Twine Message) {
  if (ErrMsg)
    *ErrMsg = Message.str();
}

static procid_t encodeChildHandle(devenv_subprocess_types_own_child_t Child) {
  return static_cast<procid_t>(Child.__handle + 1);
}

static devenv_subprocess_types_own_child_t decodeChildHandle(ProcessInfo PI) {
  return devenv_subprocess_types_own_child_t{static_cast<int32_t>(PI.Pid - 1)};
}

static void finishProcessInfo(ProcessInfo &PI,
                              devenv_subprocess_types_own_child_t Child) {
  PI.Pid = encodeChildHandle(Child);
  PI.Process = PI.Pid;
  PI.ReturnCode = 0;
}

struct RedirectPlan {
  bool CaptureStdout = false;
  bool CaptureStderr = false;
  std::string StdoutPath;
  std::string StderrPath;
};

static bool
configureOutputRedirect(std::optional<StringRef> Redirect,
                        devenv_subprocess_types_option_output_t &Output,
                        bool &Capture, std::string &Path) {
  if (!Redirect) {
    Output.is_some = false;
    return true;
  }

  Output.is_some = true;
  if (Redirect->empty()) {
    Output.val.tag = DEVENV_SUBPROCESS_TYPES_OUTPUT_DISCARD;
    return true;
  }

  Output.val.tag = DEVENV_SUBPROCESS_TYPES_OUTPUT_PIPE;
  Capture = true;
  Path = std::string(*Redirect);
  return true;
}

static bool redirectsRequestOutputFileCapture(
    ArrayRef<std::optional<StringRef>> Redirects) {
  if (Redirects.empty())
    return false;
  assert(Redirects.size() == 3);
  return (Redirects[1] && !Redirects[1]->empty()) ||
         (Redirects[2] && !Redirects[2]->empty());
}

static bool configureRedirects(ArrayRef<std::optional<StringRef>> Redirects,
                               devenv_subprocess_host_spawn_options_t &Options,
                               RedirectPlan &Plan, std::string *ErrMsg) {
  if (Redirects.empty()) {
    Options.stdin_.is_some = false;
    Options.stdout_.is_some = false;
    Options.stderr_.is_some = false;
    return true;
  }

  assert(Redirects.size() == 3);

  if (Redirects[0]) {
    Options.stdin_.is_some = true;
    if (Redirects[0]->empty()) {
      Options.stdin_.val.tag = DEVENV_SUBPROCESS_TYPES_STDIN_CLOSED;
    } else {
      setErrMsg(ErrMsg, "WASI subprocess spawning does not support stdin file "
                        "redirects yet");
      return false;
    }
  } else {
    Options.stdin_.is_some = false;
  }

  configureOutputRedirect(Redirects[1], Options.stdout_, Plan.CaptureStdout,
                          Plan.StdoutPath);
  configureOutputRedirect(Redirects[2], Options.stderr_, Plan.CaptureStderr,
                          Plan.StderrPath);
  return true;
}

static bool appendEnvTuple(
    StringRef Entry,
    SmallVectorImpl<subprocess_host_tuple2_string_string_t> &Environment) {
  StringRef Name;
  StringRef Value;
  std::tie(Name, Value) = Entry.split('=');
  if (Name.empty())
    return false;
  Environment.push_back(subprocess_host_tuple2_string_string_t{
      toSubprocessString(Name), toSubprocessString(Value)});
  return true;
}

static bool buildSpawnOptions(
    StringRef Program, ArrayRef<StringRef> Args,
    std::optional<ArrayRef<StringRef>> Env,
    ArrayRef<std::optional<StringRef>> Redirects,
    devenv_subprocess_host_spawn_options_t &Options,
    SmallVectorImpl<subprocess_host_string_t> &ArgumentStorage,
    SmallVectorImpl<subprocess_host_tuple2_string_string_t> &EnvironmentStorage,
    RedirectPlan &Plan, std::string *ErrMsg) {
  Options = {};
  Options.command = toSubprocessString(Program);

  ArgumentStorage.clear();
  for (StringRef Arg : Args)
    ArgumentStorage.push_back(toSubprocessString(Arg));
  Options.arguments = subprocess_host_list_string_t{ArgumentStorage.data(),
                                                    ArgumentStorage.size()};

  Options.cwd.is_some = false;

  EnvironmentStorage.clear();
  if (Env) {
    for (StringRef Entry : *Env)
      appendEnvTuple(Entry, EnvironmentStorage);
  }
  Options.environment = subprocess_host_list_tuple2_string_string_t{
      EnvironmentStorage.data(), EnvironmentStorage.size()};

  return configureRedirects(Redirects, Options, Plan, ErrMsg);
}

static bool openOutputFile(StringRef Path,
                           std::unique_ptr<raw_fd_ostream> &Stream,
                           raw_fd_ostream *&Output, std::string *ErrMsg) {
  std::error_code EC;
  Stream = std::make_unique<raw_fd_ostream>(
      Path, EC, sys::fs::CD_CreateAlways, sys::fs::FA_Write, sys::fs::OF_None);
  if (EC) {
    setErrMsg(ErrMsg, Twine("failed to open subprocess redirect '") + Path +
                          "': " + EC.message());
    return false;
  }
  Output = Stream.get();
  return true;
}

struct CapturedStream {
  bool Active = false;
  devenv_subprocess_types_own_input_stream_t Stream = {};
  devenv_subprocess_types_own_pollable_t Pollable = {};
  raw_fd_ostream *Output = nullptr;
};

static void dropCapturedStream(CapturedStream &Capture) {
  if (!Capture.Active)
    return;
  poll_pollable_drop_own(Capture.Pollable);
  streams_input_stream_drop_own(Capture.Stream);
  Capture.Active = false;
}

static std::string describeStreamError(streams_stream_error_t &Error) {
  switch (Error.tag) {
  case STREAMS_STREAM_ERROR_CLOSED:
    return "stream closed";
  case STREAMS_STREAM_ERROR_LAST_OPERATION_FAILED: {
    subprocess_host_string_t DebugString;
    io_error_method_error_to_debug_string(
        io_error_borrow_error(Error.val.last_operation_failed), &DebugString);
    std::string Result = toStdString(DebugString);
    subprocess_host_string_free(&DebugString);
    io_error_error_drop_own(Error.val.last_operation_failed);
    return Result;
  }
  }
  return "unknown stream error";
}

static bool drainReadyStream(CapturedStream &Capture, std::string *ErrMsg) {
  constexpr uint64_t ChunkSize = 64 * 1024;
  while (Capture.Active) {
    subprocess_host_list_u8_t Chunk;
    streams_stream_error_t StreamError;
    if (!streams_method_input_stream_read(
            streams_borrow_input_stream(Capture.Stream), ChunkSize, &Chunk,
            &StreamError)) {
      if (StreamError.tag == STREAMS_STREAM_ERROR_CLOSED) {
        dropCapturedStream(Capture);
        return true;
      }
      std::string Error = describeStreamError(StreamError);
      dropCapturedStream(Capture);
      setErrMsg(ErrMsg, Twine("failed reading subprocess output: ") + Error);
      return false;
    }

    if (Chunk.len == 0) {
      subprocess_host_list_u8_free(&Chunk);
      return true;
    }

    Capture.Output->write(reinterpret_cast<const char *>(Chunk.ptr), Chunk.len);
    subprocess_host_list_u8_free(&Chunk);
    if (Capture.Output->has_error()) {
      setErrMsg(ErrMsg, "failed writing subprocess output redirect");
      dropCapturedStream(Capture);
      return false;
    }
  }
  return true;
}

static bool takeCapturedStream(devenv_subprocess_types_own_child_t Child,
                               bool Stderr, raw_fd_ostream *Output,
                               CapturedStream &Capture, std::string *ErrMsg) {
  bool HasStream =
      Stderr
          ? devenv_subprocess_types_method_child_stderr(
                devenv_subprocess_types_borrow_child(Child), &Capture.Stream)
          : devenv_subprocess_types_method_child_stdout(
                devenv_subprocess_types_borrow_child(Child), &Capture.Stream);
  if (!HasStream) {
    setErrMsg(ErrMsg, Stderr ? "host did not provide subprocess stderr pipe"
                             : "host did not provide subprocess stdout pipe");
    return false;
  }

  Capture.Output = Output;
  Capture.Pollable = streams_method_input_stream_subscribe(
      streams_borrow_input_stream(Capture.Stream));
  Capture.Active = true;
  return true;
}

static int returnCodeFromExitStatus(
    const devenv_subprocess_types_exit_status_t &ExitStatus,
    std::string *ErrMsg) {
  switch (ExitStatus.tag) {
  case DEVENV_SUBPROCESS_TYPES_EXIT_STATUS_EXITED:
    return ExitStatus.val.exited.is_err ? 1 : 0;
  case DEVENV_SUBPROCESS_TYPES_EXIT_STATUS_EXITED_WITH_CODE:
    return ExitStatus.val.exited_with_code;
  case DEVENV_SUBPROCESS_TYPES_EXIT_STATUS_TERMINATED:
    setErrMsg(ErrMsg, "child terminated");
    return -2;
  }
  setErrMsg(ErrMsg, "unknown child exit status");
  return -1;
}

static bool childExitStatus(devenv_subprocess_types_own_child_t Child,
                            int &ReturnCode, std::string *ErrMsg) {
  devenv_subprocess_types_exit_status_t ExitStatus;
  if (!devenv_subprocess_types_method_child_exit_status(
          devenv_subprocess_types_borrow_child(Child), &ExitStatus))
    return false;
  ReturnCode = returnCodeFromExitStatus(ExitStatus, ErrMsg);
  devenv_subprocess_types_exit_status_free(&ExitStatus);
  return true;
}

static int waitForChildAndDrainOutput(devenv_subprocess_types_own_child_t Child,
                                      CapturedStream &Stdout,
                                      CapturedStream &Stderr,
                                      std::optional<unsigned> SecondsToWait,
                                      std::string *ErrMsg) {
  (void)SecondsToWait;
  int ReturnCode = -1;
  bool ChildExited = childExitStatus(Child, ReturnCode, ErrMsg);
  devenv_subprocess_types_own_pollable_t ChildPollable = {};
  bool HasChildPollable = !ChildExited;
  if (HasChildPollable)
    ChildPollable = devenv_subprocess_types_method_child_subscribe(
        devenv_subprocess_types_borrow_child(Child));

  while (!ChildExited || Stdout.Active || Stderr.Active) {
    SmallVector<poll_borrow_pollable_t, 3> Pollables;
    SmallVector<unsigned, 3> Kinds;

    if (Stdout.Active) {
      Pollables.push_back(poll_borrow_pollable(Stdout.Pollable));
      Kinds.push_back(0);
    }
    if (Stderr.Active) {
      Pollables.push_back(poll_borrow_pollable(Stderr.Pollable));
      Kinds.push_back(1);
    }
    if (!ChildExited) {
      Pollables.push_back(poll_borrow_pollable(ChildPollable));
      Kinds.push_back(2);
    }

    if (Pollables.empty())
      break;

    poll_list_borrow_pollable_t PollList{Pollables.data(), Pollables.size()};
    subprocess_host_list_u32_t Ready;
    poll_poll(&PollList, &Ready);

    for (size_t I = 0; I < Ready.len; ++I) {
      unsigned Kind = Kinds[Ready.ptr[I]];
      if (Kind == 0) {
        if (!drainReadyStream(Stdout, ErrMsg)) {
          subprocess_host_list_u32_free(&Ready);
          if (HasChildPollable)
            poll_pollable_drop_own(ChildPollable);
          devenv_subprocess_types_method_child_terminate(
              devenv_subprocess_types_borrow_child(Child));
          devenv_subprocess_types_child_drop_own(Child);
          return -1;
        }
      } else if (Kind == 1) {
        if (!drainReadyStream(Stderr, ErrMsg)) {
          subprocess_host_list_u32_free(&Ready);
          if (HasChildPollable)
            poll_pollable_drop_own(ChildPollable);
          devenv_subprocess_types_method_child_terminate(
              devenv_subprocess_types_borrow_child(Child));
          devenv_subprocess_types_child_drop_own(Child);
          return -1;
        }
      } else if (!ChildExited) {
        ChildExited = childExitStatus(Child, ReturnCode, ErrMsg);
      }
    }

    subprocess_host_list_u32_free(&Ready);
  }

  if (!ChildExited) {
    if (HasChildPollable)
      poll_pollable_drop_own(ChildPollable);
    setErrMsg(ErrMsg, "child completion was not observed");
    ReturnCode = -1;
  }

  dropCapturedStream(Stdout);
  dropCapturedStream(Stderr);
  if (HasChildPollable)
    poll_pollable_drop_own(ChildPollable);
  devenv_subprocess_types_child_drop_own(Child);
  return ReturnCode;
}

static bool spawnWithSubprocess(ProcessInfo &PI, StringRef Program,
                                ArrayRef<StringRef> Args,
                                std::optional<ArrayRef<StringRef>> Env,
                                ArrayRef<std::optional<StringRef>> Redirects,
                                unsigned MemoryLimit, std::string *ErrMsg,
                                BitVector *AffinityMask, bool DetachProcess,
                                RedirectPlan &Plan) {
  if (MemoryLimit != 0) {
    setErrMsg(ErrMsg,
              "WASI subprocess spawning does not support memory limits");
    return false;
  }
  if (AffinityMask) {
    setErrMsg(ErrMsg,
              "WASI subprocess spawning does not support affinity masks");
    return false;
  }
  if (DetachProcess) {
    setErrMsg(ErrMsg,
              "WASI subprocess spawning does not support detached children");
    return false;
  }

  SmallVector<subprocess_host_string_t, 32> Arguments;
  SmallVector<subprocess_host_tuple2_string_string_t, 32> Environment;
  devenv_subprocess_host_spawn_options_t Options;
  if (!buildSpawnOptions(Program, Args, Env, Redirects, Options, Arguments,
                         Environment, Plan, ErrMsg))
    return false;

  devenv_subprocess_host_own_child_t Child;
  subprocess_host_string_t Error;
  if (!devenv_subprocess_host_spawn(&Options, &Child, &Error)) {
    setErrMsg(ErrMsg, Twine("subprocess spawn failed: ") + toStdString(Error));
    subprocess_host_string_free(&Error);
    return false;
  }

  finishProcessInfo(PI, Child);
  return true;
}

} // namespace

static int ExecuteAndWaitWithWasiSubprocess(
    StringRef Program, ArrayRef<StringRef> Args,
    std::optional<ArrayRef<StringRef>> Env,
    ArrayRef<std::optional<StringRef>> Redirects, unsigned SecondsToWait,
    unsigned MemoryLimit, std::string *ErrMsg, bool *ExecutionFailed,
    std::optional<ProcessStatistics> *ProcStat, BitVector *AffinityMask) {
  assert(Redirects.empty() || Redirects.size() == 3);
  if (ProcStat)
    ProcStat->reset();

  ProcessInfo PI;
  RedirectPlan Plan;
  if (!spawnWithSubprocess(PI, Program, Args, Env, Redirects, MemoryLimit,
                           ErrMsg, AffinityMask, /*DetachProcess=*/false,
                           Plan)) {
    if (ExecutionFailed)
      *ExecutionFailed = true;
    return -1;
  }

  if (ExecutionFailed)
    *ExecutionFailed = false;

  devenv_subprocess_types_own_child_t Child = decodeChildHandle(PI);
  std::unique_ptr<raw_fd_ostream> StdoutFile;
  std::unique_ptr<raw_fd_ostream> StderrFile;
  std::unique_ptr<raw_fd_ostream> SharedFile;
  raw_fd_ostream *StdoutOutput = nullptr;
  raw_fd_ostream *StderrOutput = nullptr;

  if (Plan.CaptureStdout && Plan.CaptureStderr &&
      Plan.StdoutPath == Plan.StderrPath) {
    if (!openOutputFile(Plan.StdoutPath, SharedFile, StdoutOutput, ErrMsg)) {
      devenv_subprocess_types_method_child_terminate(
          devenv_subprocess_types_borrow_child(Child));
      devenv_subprocess_types_child_drop_own(Child);
      return -1;
    }
    StderrOutput = StdoutOutput;
  } else {
    if (Plan.CaptureStdout &&
        !openOutputFile(Plan.StdoutPath, StdoutFile, StdoutOutput, ErrMsg)) {
      devenv_subprocess_types_method_child_terminate(
          devenv_subprocess_types_borrow_child(Child));
      devenv_subprocess_types_child_drop_own(Child);
      return -1;
    }
    if (Plan.CaptureStderr &&
        !openOutputFile(Plan.StderrPath, StderrFile, StderrOutput, ErrMsg)) {
      devenv_subprocess_types_method_child_terminate(
          devenv_subprocess_types_borrow_child(Child));
      devenv_subprocess_types_child_drop_own(Child);
      return -1;
    }
  }

  CapturedStream Stdout;
  CapturedStream Stderr;
  if (Plan.CaptureStdout && !takeCapturedStream(Child, /*Stderr=*/false,
                                                StdoutOutput, Stdout, ErrMsg)) {
    devenv_subprocess_types_method_child_terminate(
        devenv_subprocess_types_borrow_child(Child));
    devenv_subprocess_types_child_drop_own(Child);
    return -1;
  }
  if (Plan.CaptureStderr && !takeCapturedStream(Child, /*Stderr=*/true,
                                                StderrOutput, Stderr, ErrMsg)) {
    dropCapturedStream(Stdout);
    devenv_subprocess_types_method_child_terminate(
        devenv_subprocess_types_borrow_child(Child));
    devenv_subprocess_types_child_drop_own(Child);
    return -1;
  }

  return waitForChildAndDrainOutput(
      Child, Stdout, Stderr,
      SecondsToWait == 0 ? std::nullopt : std::optional(SecondsToWait), ErrMsg);
}

#endif // defined(LLVM_WASI_ENABLE_SUBPROCESS)

ErrorOr<std::string> sys::findProgramByName(StringRef Name,
                                            ArrayRef<StringRef> Paths) {
  assert(!Name.empty() && "Must have a name!");
  if (Name.contains('/'))
    return std::string(Name);

  SmallVector<StringRef, 16> EnvironmentPaths;
  if (Paths.empty())
    if (const char *PathEnv = std::getenv("PATH")) {
      SplitString(PathEnv, EnvironmentPaths, ":");
      Paths = EnvironmentPaths;
    }

  for (auto Path : Paths) {
    if (Path.empty())
      continue;
    SmallString<128> FilePath(Path);
    sys::path::append(FilePath, Name);
    if (sys::fs::can_execute(FilePath.c_str()))
      return std::string(FilePath);
  }
  return errc::no_such_file_or_directory;
}

#if !defined(LLVM_WASI_ENABLE_SUBPROCESS)
static bool Execute(ProcessInfo &PI, StringRef, ArrayRef<StringRef>,
                    std::optional<ArrayRef<StringRef>>,
                    ArrayRef<std::optional<StringRef>>, unsigned,
                    std::string *ErrMsg, BitVector *, bool) {
  PI.ReturnCode = -1;
  if (ErrMsg)
    *ErrMsg = "program execution is not supported on WASI";
  return false;
}
#endif

#if defined(LLVM_WASI_ENABLE_SUBPROCESS)
static bool Execute(ProcessInfo &PI, StringRef Program,
                    ArrayRef<StringRef> Args,
                    std::optional<ArrayRef<StringRef>> Env,
                    ArrayRef<std::optional<StringRef>> Redirects,
                    unsigned MemoryLimit, std::string *ErrMsg,
                    BitVector *AffinityMask, bool DetachProcess) {
  if (redirectsRequestOutputFileCapture(Redirects)) {
    setErrMsg(ErrMsg,
              "WASI subprocess ExecuteNoWait does not support file redirects");
    return false;
  }

  RedirectPlan Plan;
  if (!spawnWithSubprocess(PI, Program, Args, Env, Redirects, MemoryLimit,
                           ErrMsg, AffinityMask, DetachProcess, Plan))
    return false;

  return true;
}
#endif

#if !defined(LLVM_WASI_ENABLE_SUBPROCESS)
ProcessInfo sys::Wait(const ProcessInfo &PI, std::optional<unsigned>,
                      std::string *ErrMsg, std::optional<ProcessStatistics> *,
                      bool) {
  ProcessInfo Result = PI;
  Result.ReturnCode = -1;
  if (ErrMsg)
    *ErrMsg = "waiting for child processes is not supported on WASI";
  return Result;
}
#endif

#if defined(LLVM_WASI_ENABLE_SUBPROCESS)
ProcessInfo sys::Wait(const ProcessInfo &PI,
                      std::optional<unsigned> SecondsToWait,
                      std::string *ErrMsg,
                      std::optional<ProcessStatistics> *ProcStat,
                      bool Polling) {
  (void)Polling;
  assert(PI.Pid && "invalid pid to wait on, process not started?");
  if (ProcStat)
    ProcStat->reset();

  ProcessInfo Result = PI;
  devenv_subprocess_types_own_child_t Child = decodeChildHandle(PI);
  int ReturnCode = -1;

  if (SecondsToWait && *SecondsToWait == 0) {
    if (!childExitStatus(Child, ReturnCode, ErrMsg)) {
      Result.Pid = 0;
      Result.Process = 0;
      return Result;
    }
  } else if (!childExitStatus(Child, ReturnCode, ErrMsg)) {
    devenv_subprocess_types_own_pollable_t Pollable =
        devenv_subprocess_types_method_child_subscribe(
            devenv_subprocess_types_borrow_child(Child));
    poll_method_pollable_block(poll_borrow_pollable(Pollable));
    poll_pollable_drop_own(Pollable);
    if (!childExitStatus(Child, ReturnCode, ErrMsg)) {
      setErrMsg(ErrMsg, "child completion was not observed");
      ReturnCode = -1;
    }
  }

  Result.ReturnCode = ReturnCode;
  devenv_subprocess_types_child_drop_own(Child);
  return Result;
}
#endif

std::error_code sys::ChangeStdinMode(fs::OpenFlags) {
  return std::error_code();
}

std::error_code sys::ChangeStdoutMode(fs::OpenFlags) {
  return std::error_code();
}

std::error_code sys::ChangeStdinToBinary() { return std::error_code(); }

std::error_code sys::ChangeStdoutToBinary() { return std::error_code(); }

std::error_code sys::writeFileWithEncoding(StringRef FileName,
                                           StringRef Contents,
                                           WindowsEncodingMethod) {
  std::error_code EC;
  raw_fd_ostream OS(FileName, EC, sys::fs::OpenFlags::OF_TextWithCRLF);
  if (EC)
    return EC;
  OS << Contents;
  if (OS.has_error())
    return make_error_code(errc::io_error);
  return EC;
}

bool sys::commandLineFitsWithinSystemLimits(StringRef, ArrayRef<StringRef>) {
  return true;
}
#elif defined(LLVM_ON_UNIX)
#include "Unix/Program.inc"
#endif
#ifdef _WIN32
#include "Windows/Program.inc"
#endif
