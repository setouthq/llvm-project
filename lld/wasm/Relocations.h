//===- Relocations.h -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_WASM_RELOCATIONS_H
#define LLD_WASM_RELOCATIONS_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include <cstdint>

namespace lld::wasm {

class InputChunk;
class Symbol;

// A TLS relocation (R_WASM_MEMORY_ADDR_TLS_SLEB{,64}) against a defined symbol
// that is not TLS.  scanRelocations collects them and reportNonTLSRelocations
// reports them, one error per symbol.
struct NonTLSRelocation {
  const InputChunk *chunk;
  const Symbol *sym;
  uint8_t type;
};

void scanRelocations(InputChunk *chunk,
                     llvm::SmallVectorImpl<NonTLSRelocation> &nonTLS);
void reportNonTLSRelocations(llvm::ArrayRef<NonTLSRelocation> nonTLS);

} // namespace lld::wasm

#endif
