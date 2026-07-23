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


int sys::ExecuteAndWait(StringRef Program, ArrayRef<StringRef> Args,
                        std::optional<ArrayRef<StringRef>> Env,
                        ArrayRef<std::optional<StringRef>> Redirects,
                        unsigned SecondsToWait, unsigned MemoryLimit,
                        std::string *ErrMsg, bool *ExecutionFailed,
                        std::optional<ProcessStatistics> *ProcStat,
                        BitVector *AffinityMask) {
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
// Process spawning is implemented over the C library rather than direct
// devenv:subprocess WIT bindings: the setouthq wasi-libc fork's subprocess
// sysroot flavor (branch wasip2-subprocess-0.4.3) provides real
// posix_spawnp/waitpid over devenv:subprocess@0.4.3, so LLVM needs no
// vendored bindings, no committed component-type object, and no second
// copy of the spawn ABI. Building with LLVM_WASI_ENABLE_SUBPROCESS
// requires linking against that sysroot flavor.
#include <cerrno>
#include <cstring>
#include <spawn.h>
#include <unistd.h>
#if defined(_WASI_EMULATED_SIGNAL)
// wasi-libc's <sys/wait.h> declares waitid(siginfo_t *) when
// _WASI_EMULATED_SIGNAL is defined, but on WASI <signal.h> only defines
// siginfo_t under __wasilibc_unmodified_upstream, so that combination
// does not compile against the current setouthq wasi-libc pin
// (wasip2-subprocess-0.4.3 @ f0b54b87). Nothing here uses emulated
// signals; drop the macro around the include until the libc guard is
// fixed upstream-of-here.
#pragma push_macro("_WASI_EMULATED_SIGNAL")
#undef _WASI_EMULATED_SIGNAL
#include <sys/wait.h>
#pragma pop_macro("_WASI_EMULATED_SIGNAL")
#else
#include <sys/wait.h>
#endif

extern char **environ;
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
  // Spawn through wasi-libc's posix_spawnp, which the setouthq wasi-libc
  // subprocess sysroot flavor implements over devenv:subprocess@0.4.3.
  // The session host resolves bare command names (registered commands)
  // and workspace paths; there is no fork/exec underneath.
  (void)MemoryLimit;
  (void)AffinityMask;
  if (DetachProcess) {
    if (ErrMsg)
      *ErrMsg = "detached processes are not supported on WASI";
    return false;
  }
  if (!Redirects.empty()) {
    assert(Redirects.size() == 3);
    for (const std::optional<StringRef> &R : Redirects) {
      if (R) {
        if (ErrMsg)
          *ErrMsg = "file redirects are not supported on WASI; children "
                    "inherit this process's stdio";
        return false;
      }
    }
  }

  std::string ProgramStorage = Program.str();
  SmallVector<std::string, 16> ArgStorage;
  SmallVector<char *, 16> Argv;
  for (StringRef A : Args)
    ArgStorage.push_back(A.str());
  for (std::string &A : ArgStorage)
    Argv.push_back(A.data());
  Argv.push_back(nullptr);

  SmallVector<std::string, 16> EnvStorage;
  SmallVector<char *, 16> Envp;
  char **EnvpPtr = environ;
  if (Env) {
    for (StringRef E : *Env)
      EnvStorage.push_back(E.str());
    for (std::string &E : EnvStorage)
      Envp.push_back(E.data());
    Envp.push_back(nullptr);
    EnvpPtr = Envp.data();
  }

  pid_t Pid = 0;
  int Err = ::posix_spawnp(&Pid, ProgramStorage.c_str(), /*file_actions=*/nullptr,
                           /*attrp=*/nullptr, Argv.data(), EnvpPtr);
  if (Err != 0) {
    if (ErrMsg)
      *ErrMsg = std::string("posix_spawnp failed: ") + std::strerror(Err);
    PI.ReturnCode = -1;
    return false;
  }

  PI.Pid = Pid;
  PI.Process = Pid;
  PI.ReturnCode = 0;
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
  int Status = 0;
  // SecondsToWait==0 means "poll, do not block" in this API; wasi-libc's
  // waitpid supports WNOHANG by polling the child's try-wait. Timeouts
  // cannot be enforced without signals; treat any other value as a
  // blocking wait (the clang driver's job execution passes no timeout).
  int Flags = (SecondsToWait && *SecondsToWait == 0) ? WNOHANG : 0;
  pid_t Waited = ::waitpid(static_cast<pid_t>(PI.Pid), &Status, Flags);
  if (Waited < 0) {
    if (ErrMsg)
      *ErrMsg = std::string("waitpid failed: ") + std::strerror(errno);
    Result.ReturnCode = -1;
    return Result;
  }
  if (Waited == 0) {
    // WNOHANG and the child is still running.
    Result.Pid = 0;
    Result.Process = 0;
    Result.ReturnCode = 0;
    return Result;
  }
  if (WIFEXITED(Status))
    Result.ReturnCode = WEXITSTATUS(Status);
  else if (WIFSIGNALED(Status))
    Result.ReturnCode = -2; // The convention for "killed by a signal".
  else
    Result.ReturnCode = -1;
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
