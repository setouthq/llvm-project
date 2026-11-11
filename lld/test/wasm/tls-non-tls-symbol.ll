; A TLS relocation (R_WASM_MEMORY_ADDR_TLS_SLEB, _SLEB64) against a symbol
; that is not TLS is an error in every link, single-threaded ones included.
; The relocation resolves to the symbol's offset from __tls_base, and a non-TLS
; symbol has none: a single-threaded link used to take the symbol's address as
; the offset, so the access landed at __tls_base plus that address. The case is
; an object compiled with thread-local storage (-pthread: atomics and
; bulk-memory) that uses a thread_local variable defined by an object compiled
; without it, where the compiler lowers thread_local to an ordinary global (C's
; errno from a libc built without threads is one). The objects come from llc,
; with and without atomics, because the assembler marks every symbol an object
; itself references with @TLSREL as TLS (relocation-bad-tls.s).

; RUN: split-file %s %t
; RUN: llc --mtriple=wasm32-unknown-unknown -mattr=+atomics,+bulk-memory -filetype=obj %t/use.ll -o %t/use.o
; RUN: llc --mtriple=wasm32-unknown-unknown -mattr=-atomics -filetype=obj %t/def.ll -o %t/def.o
; RUN: llc --mtriple=wasm32-unknown-unknown -mattr=+atomics,+bulk-memory -filetype=obj %t/def.ll -o %t/def-tls.o
; RUN: llc --mtriple=wasm32-unknown-unknown -mattr=+atomics,+bulk-memory -filetype=obj %t/def-plain.ll -o %t/def-plain.o
; RUN: rm -f %t/libdef.a
; RUN: llvm-ar rcs %t/libdef.a %t/def.o

;; A single-threaded link refuses it, naming the object, the symbol, where it
;; is defined, the cause and the fix: as an executable and as a PIE.
; RUN: not wasm-ld --no-entry --export=get_counter %t/use.o %t/def.o -o %t/single.wasm 2>&1 \
; RUN:   | FileCheck %s --check-prefixes=ERR,SINGLE
; RUN: not wasm-ld -pie --no-entry --export=get_counter %t/use.o %t/def.o -o %t/pie.wasm 2>&1 \
; RUN:   | FileCheck %s --check-prefixes=ERR,SINGLE

; ERR:         error: {{.*}}use.o: relocation R_WASM_MEMORY_ADDR_TLS_SLEB cannot be used against non-TLS symbol `counter`
; ERR-NEXT:    >>> defined in {{.*}}def.o
; ERR-NEXT:    >>> {{.*}}use.o reaches `counter` as a thread-local (compiled with -pthread, or with atomics and bulk-memory), but {{.*}}def.o defines it as an ordinary global (compiled without them, or not declared thread_local)
; SINGLE-NEXT: >>> this link is single-threaded (no --shared-memory or --cooperative-threading): compile {{.*}}use.o without -pthread
; SINGLE-NOT:  error:

;; The definition comes from an archive member, as errno comes from libc.a.
; RUN: not wasm-ld --no-entry --export=get_counter %t/use.o %t/libdef.a -o %t/archive.wasm 2>&1 \
; RUN:   | FileCheck %s --check-prefix=ARCHIVE

; ARCHIVE:      error: {{.*}}use.o: relocation R_WASM_MEMORY_ADDR_TLS_SLEB cannot be used against non-TLS symbol `counter`
; ARCHIVE-NEXT: >>> defined in {{.*}}libdef.a(def.o)

;; A multi-threaded link refused it before; the message now names the
;; definition and the fix as well.
; RUN: not wasm-ld --shared-memory --no-entry --export=get_counter %t/use.o %t/def-plain.o -o %t/shared.wasm 2>&1 \
; RUN:   | FileCheck %s --check-prefix=MULTI

; MULTI:      error: {{.*}}use.o: relocation R_WASM_MEMORY_ADDR_TLS_SLEB cannot be used against non-TLS symbol `counter`
; MULTI-NEXT: >>> defined in {{.*}}def-plain.o
; MULTI-NEXT: >>> {{.*}}use.o reaches `counter` as a thread-local (compiled with -pthread, or with atomics and bulk-memory), but {{.*}}def-plain.o defines it as an ordinary global (compiled without them, or not declared thread_local)
; MULTI-NEXT: >>> compile {{.*}}def-plain.o with the same threading flags as {{.*}}use.o, with `counter` declared thread_local
; MULTI:      error: {{.*}}use.o: relocation R_WASM_MEMORY_ADDR_TLS_SLEB cannot be used against `counter` in non-TLS section: .data

;; wasm64: R_WASM_MEMORY_ADDR_TLS_SLEB64.
; RUN: llc --mtriple=wasm64-unknown-unknown -mattr=+atomics,+bulk-memory -filetype=obj %t/use.ll -o %t/use64.o
; RUN: llc --mtriple=wasm64-unknown-unknown -mattr=-atomics -filetype=obj %t/def.ll -o %t/def64.o
; RUN: not wasm-ld -mwasm64 --no-entry --export=get_counter %t/use64.o %t/def64.o -o %t/single64.wasm 2>&1 \
; RUN:   | FileCheck %s --check-prefix=WASM64

; WASM64:      error: {{.*}}use64.o: relocation R_WASM_MEMORY_ADDR_TLS_SLEB64 cannot be used against non-TLS symbol `counter`
; WASM64-NEXT: >>> defined in {{.*}}def64.o

;; The same object against a TLS definition links, with and without shared
;; memory: the check is on the symbol, not on the link.
; RUN: wasm-ld --no-entry --export=get_counter %t/use.o %t/def-tls.o -o %t/tls.wasm
; RUN: wasm-ld --shared-memory --no-entry --export=get_counter %t/use.o %t/def-tls.o -o %t/tls-shared.wasm

;; A function nothing keeps alive is not scanned: its relocation never reaches
;; the output.
; RUN: wasm-ld --no-entry %t/use.o %t/def.o -o %t/gc.wasm

;--- use.ll
@counter = external thread_local global i32

declare ptr @llvm.threadlocal.address.p0(ptr)

define i32 @get_counter() {
  %p = call ptr @llvm.threadlocal.address.p0(ptr @counter)
  %v = load i32, ptr %p
  ret i32 %v
}

;--- def.ll
@counter = thread_local global i32 7

;--- def-plain.ll
@counter = global i32 7
