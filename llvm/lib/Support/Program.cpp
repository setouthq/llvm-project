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
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <cassert>
#include <cstdlib>
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

static bool Execute(ProcessInfo &PI, StringRef, ArrayRef<StringRef>,
                    std::optional<ArrayRef<StringRef>>,
                    ArrayRef<std::optional<StringRef>>, unsigned,
                    std::string *ErrMsg, BitVector *, bool) {
  PI.ReturnCode = -1;
  if (ErrMsg)
    *ErrMsg = "program execution is not supported on WASI";
  return false;
}

ProcessInfo sys::Wait(const ProcessInfo &PI, std::optional<unsigned>,
                      std::string *ErrMsg,
                      std::optional<ProcessStatistics> *, bool) {
  ProcessInfo Result = PI;
  Result.ReturnCode = -1;
  if (ErrMsg)
    *ErrMsg = "waiting for child processes is not supported on WASI";
  return Result;
}

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

bool sys::commandLineFitsWithinSystemLimits(StringRef,
                                            ArrayRef<StringRef>) {
  return true;
}
#elif defined(LLVM_ON_UNIX)
#include "Unix/Program.inc"
#endif
#ifdef _WIN32
#include "Windows/Program.inc"
#endif
