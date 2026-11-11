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
#include "llvm/ADT/MapVector.h"

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

// Whether the compiler lowered thread-local storage or atomics away in this
// object, because it was compiled without atomics or bulk-memory.  It marks
// such an object as disallowing shared memory.
static bool strippedThreadLocals(const ObjFile *file) {
  return llvm::any_of(file->getWasmObj()->getTargetFeatures(),
                      [](const WasmFeatureEntry &f) {
                        return f.Prefix == WASM_FEATURE_PREFIX_DISALLOWED &&
                               f.Name == "shared-mem";
                      });
}

// A TLS relocation (R_WASM_MEMORY_ADDR_TLS_SLEB{,64}) resolves to the offset
// of a TLS symbol from __tls_base, and the code adds __tls_base to it at run
// time.  A symbol that is not TLS has no such offset: the relocation would get
// the symbol's address and the access would land at __tls_base plus that
// address.  That holds in single-threaded links too, where __tls_base is the
// start of the .tdata segment (or of the data when there is none), so this is
// an error in every final link.  The usual cause is code compiled with
// thread-local storage (-pthread, or atomics and bulk-memory) using a
// thread_local variable defined by an object compiled without it, where the
// compiler lowers thread_local to an ordinary global: C's errno, declared
// `extern _Thread_local int errno` and defined by a libc built without
// threads, is one.  LTO is another: a module that defines no functions has no
// target features of its own, so unless --shared-memory adds atomics
// (createConfig in LTO.cpp) LTO compiles it without them and its thread_local
// variables become ordinary globals.
void reportNonTLSRelocations(ArrayRef<NonTLSRelocation> nonTLS) {
  MapVector<const Symbol *, SmallVector<const NonTLSRelocation *, 0>> bySym;
  for (const NonTLSRelocation &r : nonTLS)
    bySym[r.sym].push_back(&r);

  for (auto &[sym, refs] : bySym) {
    std::string msg = "relocation ";
    msg += relocTypeToString(refs.front()->type);
    msg += " cannot be used against non-TLS symbol `" + toString(*sym) +
           "`\n>>> defined in " + toString(sym->getFile());

    // Up to three places that reference it, then how many references remain.
    SmallVector<const InputChunk *, 3> shown;
    size_t rest = 0;
    for (const NonTLSRelocation *r : refs) {
      if (llvm::is_contained(shown, r->chunk))
        continue;
      if (shown.size() == 3) {
        ++rest;
        continue;
      }
      shown.push_back(r->chunk);
      msg += "\n>>> referenced by " + toString(r->chunk);
    }
    if (rest)
      msg += "\n>>> referenced " + std::to_string(rest) + " more time" +
             (rest == 1 ? "" : "s");

    msg += "\n>>> the references are thread-local; the definition is not";
    const auto *def = dyn_cast_or_null<ObjFile>(sym->getFile());
    bool stripped = def && strippedThreadLocals(def);
    if (stripped && def->fromLTO && def->functions.empty() &&
        !ctx.arg.isMultithreaded()) {
      msg += "\n>>> LTO dropped thread_local: it compiles a module with no "
             "functions without atomics";
      msg += "\n>>> use full LTO, or link with --shared-memory (clang's "
             "-pthread)";
    } else {
      msg += stripped ? "\n>>> the definition was compiled without atomics or "
                        "bulk-memory, which drops thread_local"
                      : "\n>>> the definition does not declare it thread_local";
      msg += ctx.arg.isMultithreaded()
                 ? "\n>>> define it as thread_local, with the same threading "
                   "flags as the references"
                 : "\n>>> this link is single-threaded: compile the references "
                   "without -pthread";
    }
    error(msg);
  }
}

void scanRelocations(InputChunk *chunk,
                     SmallVectorImpl<NonTLSRelocation> &nonTLS) {
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
        // Reported after the scan, one error per symbol.  A single-threaded
        // relocatable link keeps the relocation, and the final link checks it
        // against the definition it picks.
        if (!ctx.arg.relocatable || ctx.arg.isMultithreaded())
          nonTLS.push_back({chunk, sym, reloc.Type});
      } else if (ctx.arg.isMultithreaded()) {
        // In single-threaded builds TLS is lowered away and TLS data can be
        // merged with normal data and allowing TLS relocation in non-TLS
        // segments.
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
