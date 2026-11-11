//===- Relocations.cpp ----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Relocations.h"

#include "InputChunks.h"
#include "OutputSegment.h"
#include "SymbolTable.h"
#include "SyntheticSections.h"

using namespace llvm;
using namespace llvm::wasm;

namespace lld::wasm {

static bool requiresGOTAccess(const Symbol *sym) {
  if (sym->isShared())
    return true;
  if (!ctx.isPic &&
      ctx.arg.unresolvedSymbols != UnresolvedPolicy::ImportDynamic)
    return false;
  if (sym->isHidden() || sym->isLocal())
    return false;
  // With `-Bsymbolic` (or when building an executable) as don't need to use
  // the GOT for symbols that are defined within the current module.
  if (sym->isDefined() && (!ctx.arg.shared || ctx.arg.bsymbolic))
    return false;
  return true;
}

static bool allowUndefined(const Symbol *sym) {
  // Symbols that are explicitly imported are always allowed to be undefined at
  // link time.
  if (sym->isImported())
    return true;
  if (isa<UndefinedFunction>(sym) && ctx.arg.importUndefined)
    return true;

  return ctx.arg.allowUndefinedSymbols.contains(sym->getName());
}

static void reportUndefined(ObjFile *file, Symbol *sym) {
  if (!allowUndefined(sym)) {
    switch (ctx.arg.unresolvedSymbols) {
    case UnresolvedPolicy::ReportError:
      error(toString(file) + ": undefined symbol: " + toString(*sym));
      break;
    case UnresolvedPolicy::Warn:
      warn(toString(file) + ": undefined symbol: " + toString(*sym));
      break;
    case UnresolvedPolicy::Ignore:
      LLVM_DEBUG(dbgs() << "ignoring undefined symbol: " + toString(*sym) +
                               "\n");
      break;
    case UnresolvedPolicy::ImportDynamic:
      break;
    }

    if (auto *f = dyn_cast<UndefinedFunction>(sym)) {
      if (!f->stubFunction &&
          ctx.arg.unresolvedSymbols != UnresolvedPolicy::ImportDynamic &&
          !ctx.arg.importUndefined) {
        f->stubFunction = symtab->createUndefinedStub(*f->getSignature());
        f->stubFunction->markLive();
        // Mark the function itself as a stub which prevents it from being
        // assigned a table entry.
        f->isStub = true;
      }
    }
  }
}

static void addGOTEntry(Symbol *sym) {
  if (requiresGOTAccess(sym))
    out.importSec->addGOTEntry(sym);
  else
    out.globalSec->addInternalGOTEntry(sym);
}

// A TLS relocation (R_WASM_MEMORY_ADDR_TLS_SLEB{,64}) resolves to the offset
// of a TLS symbol from __tls_base, and the code adds __tls_base to it at run
// time.  A symbol that is not TLS has no such offset: the relocation would get
// the symbol's address and the access would land at __tls_base plus that
// address.  That holds in single-threaded links too, where __tls_base is the
// start of the .tdata segment (or of the data when there is none), so this is
// an error in every link.  The usual cause is an object compiled with
// thread-local storage (-pthread, or the atomics and bulk-memory features)
// using a thread_local variable from an object compiled without it, where the
// compiler lowers thread_local to an ordinary global: C's errno, declared
// `extern _Thread_local int errno` and defined by a libc built without
// threads, is one.
static void reportNonTLSSymbol(const ObjFile *file, const WasmRelocation &reloc,
                               const Symbol *sym) {
  std::string use = toString(file);
  std::string def = toString(sym->getFile());
  std::string name = toString(*sym);
  std::string why = use + " reaches `" + name +
                    "` as a thread-local (compiled with -pthread, or with "
                    "atomics and bulk-memory), but " +
                    def +
                    " defines it as an ordinary global (compiled without "
                    "them, or not declared thread_local)";
  std::string fix = ctx.arg.isMultithreaded()
                        ? "compile " + def +
                              " with the same threading flags as " + use +
                              ", with `" + name + "` declared thread_local"
                        : "this link is single-threaded (no --shared-memory or "
                          "--cooperative-threading): compile " +
                              use + " without -pthread";
  error(use + ": relocation " + relocTypeToString(reloc.Type) +
        " cannot be used against non-TLS symbol `" + name +
        "`\n>>> defined in " + def + "\n>>> " + why + "\n>>> " + fix);
}

void scanRelocations(InputChunk *chunk) {
  if (!chunk->live)
    return;
  ObjFile *file = chunk->file;
  ArrayRef<WasmSignature> types = file->getWasmObj()->types();
  for (const WasmRelocation &reloc : chunk->getRelocations()) {
    if (reloc.Type == R_WASM_TYPE_INDEX_LEB) {
      // Mark target type as live
      file->typeMap[reloc.Index] =
          out.typeSec->registerType(types[reloc.Index]);
      file->typeIsUsed[reloc.Index] = true;
      continue;
    }

    // Other relocation types all have a corresponding symbol
    Symbol *sym = file->getSymbols()[reloc.Index];

    switch (reloc.Type) {
    case R_WASM_TABLE_INDEX_I32:
    case R_WASM_TABLE_INDEX_I64:
    case R_WASM_TABLE_INDEX_SLEB:
    case R_WASM_TABLE_INDEX_SLEB64:
    case R_WASM_TABLE_INDEX_REL_SLEB:
    case R_WASM_TABLE_INDEX_REL_SLEB64:
      if (requiresGOTAccess(sym))
        break;
      out.elemSec->addEntry(cast<FunctionSymbol>(sym));
      break;
    case R_WASM_GLOBAL_INDEX_LEB:
    case R_WASM_GLOBAL_INDEX_I32:
      if (!isa<GlobalSymbol>(sym))
        addGOTEntry(sym);
      break;
    case R_WASM_MEMORY_ADDR_TLS_SLEB:
    case R_WASM_MEMORY_ADDR_TLS_SLEB64:
      if (!sym->isDefined()) {
        error(toString(file) + ": relocation " + relocTypeToString(reloc.Type) +
              " cannot be used against an undefined symbol `" + toString(*sym) +
              "`");
      } else if (!sym->isTLS()) {
        reportNonTLSSymbol(file, reloc, sym);
      }
      // In single-threaded builds TLS is lowered away and TLS data can be
      // merged with normal data and allowing TLS relocation in non-TLS
      // segments.
      if (ctx.arg.isMultithreaded()) {
        if (auto *D = dyn_cast<DefinedData>(sym)) {
          if (!D->segment->outputSeg->isTLS()) {
            error(toString(file) + ": relocation " +
                  relocTypeToString(reloc.Type) + " cannot be used against `" +
                  toString(*sym) +
                  "` in non-TLS section: " + D->segment->outputSeg->name);
          }
        }
      }
      break;
    }

    if (ctx.isPic || sym->isShared() ||
        (sym->isUndefined() &&
         ctx.arg.unresolvedSymbols == UnresolvedPolicy::ImportDynamic)) {
      switch (reloc.Type) {
      case R_WASM_TABLE_INDEX_SLEB:
      case R_WASM_TABLE_INDEX_SLEB64:
      case R_WASM_MEMORY_ADDR_SLEB:
      case R_WASM_MEMORY_ADDR_LEB:
      case R_WASM_MEMORY_ADDR_SLEB64:
      case R_WASM_MEMORY_ADDR_LEB64:
        // Certain relocation types can't be used when building PIC output,
        // since they would require absolute symbol addresses at link time.
        error(toString(file) + ": relocation " + relocTypeToString(reloc.Type) +
              " cannot be used against symbol `" + toString(*sym) +
              "`; recompile with -fPIC");
        break;
      case R_WASM_TABLE_INDEX_I32:
      case R_WASM_TABLE_INDEX_I64:
      case R_WASM_MEMORY_ADDR_I32:
      case R_WASM_MEMORY_ADDR_I64:
        // These relocation types are only present in the data section and
        // will be converted into code by `generateRelocationCode`.  This
        // code requires the symbols to have GOT entries.
        if (requiresGOTAccess(sym))
          addGOTEntry(sym);
        break;
      }
    }

    if (!ctx.arg.relocatable && sym->isUndefined()) {
      switch (reloc.Type) {
      case R_WASM_TABLE_INDEX_REL_SLEB:
      case R_WASM_TABLE_INDEX_REL_SLEB64:
      case R_WASM_MEMORY_ADDR_REL_SLEB:
      case R_WASM_MEMORY_ADDR_REL_SLEB64:
        // These relocation types are for symbols that exists relative to
        // `__memory_base` or `__table_base` and as such only make sense for
        // defined symbols.
        error(toString(file) + ": relocation " + relocTypeToString(reloc.Type) +
              " is not supported against an undefined symbol `" +
              toString(*sym) + "`");
        break;
      }

      if (!sym->isWeak()) {
        // Report undefined symbols
        reportUndefined(file, sym);
      }
    }
  }
}

} // namespace lld::wasm
