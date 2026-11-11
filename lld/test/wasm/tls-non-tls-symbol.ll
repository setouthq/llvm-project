; A TLS relocation (R_WASM_MEMORY_ADDR_TLS_SLEB, _SLEB64) against a defined
; symbol that is not TLS is an error in every final link, single-threaded ones
; included. The relocation resolves to the symbol's offset from __tls_base, and
; a non-TLS symbol has none: a single-threaded link used to take the symbol's
; address as the offset, so the access landed at __tls_base plus that address.
; The case is code compiled with thread-local storage (-pthread: atomics and
; bulk-memory) that uses a thread_local variable defined by an object compiled
; without it, where the compiler lowers thread_local to an ordinary global (C's
; errno from a libc built without threads is one), or by a module that ThinLTO
; compiled without atomics. The objects come from llc, with and without
; atomics, because the assembler marks every symbol an object itself references
; with @TLSREL as TLS (relocation-bad-tls.s).

; RUN: split-file %s %t
; RUN: llc --mtriple=wasm32-unknown-unknown -mattr=+atomics,+bulk-memory -filetype=obj %t/use.ll -o %t/use.o
; RUN: llc --mtriple=wasm32-unknown-unknown -mattr=+atomics,+bulk-memory -filetype=obj %t/use2.ll -o %t/use2.o
; RUN: llc --mtriple=wasm32-unknown-unknown -mattr=-atomics -filetype=obj %t/def.ll -o %t/def.o
; RUN: llc --mtriple=wasm32-unknown-unknown -mattr=+atomics,+bulk-memory -filetype=obj %t/def.ll -o %t/def-tls.o
; RUN: llc --mtriple=wasm32-unknown-unknown -mattr=+atomics,+bulk-memory -filetype=obj %t/def-plain.ll -o %t/def-plain.o
; RUN: llc --mtriple=wasm32-unknown-unknown -mattr=+atomics,+bulk-memory -filetype=obj %t/def-weak.ll -o %t/def-weak.o
; RUN: rm -f %t/libdef.a
; RUN: llvm-ar rcs %t/libdef.a %t/def.o

;; A single-threaded link refuses it, one error per symbol: where the symbol is
;; defined and referenced, the cause and the fix. As an executable, a PIE and a
;; shared library.
; RUN: not wasm-ld --no-gc-sections --no-entry %t/use.o %t/def.o -o %t/single.wasm 2>&1 \
; RUN:   | FileCheck %s --check-prefixes=ERR,SINGLE
; RUN: not wasm-ld -pie --no-gc-sections --no-entry %t/use.o %t/def.o -o %t/pie.wasm 2>&1 \
; RUN:   | FileCheck %s --check-prefixes=ERR,SINGLE
; RUN: not wasm-ld -shared --no-gc-sections %t/use.o %t/def.o -o %t/single.so 2>&1 \
; RUN:   | FileCheck %s --check-prefixes=ERR,SINGLE

; ERR:         error: relocation R_WASM_MEMORY_ADDR_TLS_SLEB cannot be used against non-TLS symbol `counter`
; ERR-NEXT:    >>> defined in {{.*}}def.o
; ERR-NEXT:    >>> referenced by {{.*}}use.o:(get_counter)
; ERR-NEXT:    >>> referenced by {{.*}}use.o:(set_counter)
; ERR-NEXT:    >>> referenced by {{.*}}use.o:(inc_counter)
; ERR-NEXT:    >>> referenced 1 more time
; ERR-NEXT:    >>> the references are thread-local; the definition is not
; ERR-NEXT:    >>> the definition was compiled without atomics or bulk-memory, which drops thread_local
; SINGLE-NEXT: >>> this link is single-threaded: compile the references without -pthread
; SINGLE-NOT:  error:

;; References from several objects make one error.
; RUN: not wasm-ld --no-entry --export=get_counter --export=get_counter2 %t/use.o %t/use2.o %t/def.o \
; RUN:   -o %t/two.wasm 2>&1 | FileCheck %s --check-prefix=TWO

; TWO:      error: relocation R_WASM_MEMORY_ADDR_TLS_SLEB cannot be used against non-TLS symbol `counter`
; TWO-NEXT: >>> defined in {{.*}}def.o
; TWO-NEXT: >>> referenced by {{.*}}use.o:(get_counter)
; TWO-NEXT: >>> referenced by {{.*}}use2.o:(get_counter2)
; TWO-NEXT: >>> the references are thread-local; the definition is not
; TWO-NOT:  error:

;; The definition comes from an archive member, as errno comes from libc.a.
;; Functions the link drops are not counted.
; RUN: not wasm-ld --no-entry --export=get_counter %t/use.o %t/libdef.a -o %t/archive.wasm 2>&1 \
; RUN:   | FileCheck %s --check-prefix=ARCHIVE

; ARCHIVE:      error: relocation R_WASM_MEMORY_ADDR_TLS_SLEB cannot be used against non-TLS symbol `counter`
; ARCHIVE-NEXT: >>> defined in {{.*}}libdef.a(def.o)
; ARCHIVE-NEXT: >>> referenced by {{.*}}use.o:(get_counter)
; ARCHIVE-NEXT: >>> the references are thread-local; the definition is not

;; A multi-threaded link refused it before; the message now says where it is
;; defined and referenced, and the fix. The definition is an ordinary global
;; compiled with atomics. The segment error no longer follows it.
; RUN: not wasm-ld --shared-memory --no-gc-sections --no-entry %t/use.o %t/def-plain.o -o %t/shared.wasm 2>&1 \
; RUN:   | FileCheck %s --check-prefix=MULTI

; MULTI:      error: relocation R_WASM_MEMORY_ADDR_TLS_SLEB cannot be used against non-TLS symbol `counter`
; MULTI-NEXT: >>> defined in {{.*}}def-plain.o
; MULTI-NEXT: >>> referenced by {{.*}}use.o:(get_counter)
; MULTI-NEXT: >>> referenced by {{.*}}use.o:(set_counter)
; MULTI-NEXT: >>> referenced by {{.*}}use.o:(inc_counter)
; MULTI-NEXT: >>> referenced 1 more time
; MULTI-NEXT: >>> the references are thread-local; the definition is not
; MULTI-NEXT: >>> the definition does not declare it thread_local
; MULTI-NEXT: >>> define it as thread_local, with the same threading flags as the references
; MULTI-NOT:  error:

;; Cooperative threading (wasm32-wasip3) keeps thread_local without atomics,
;; and such a link is multi-threaded.
; RUN: llc --mtriple=wasm32-unknown-wasip3 -filetype=obj %t/use.ll -o %t/use-coop.o
; RUN: llc --mtriple=wasm32-unknown-wasip3 -filetype=obj %t/def.ll -o %t/def-coop.o
; RUN: llc --mtriple=wasm32-unknown-wasip3 -filetype=obj %t/def-plain.ll -o %t/def-plain-coop.o
; RUN: not wasm-ld --cooperative-threading --allow-undefined --no-entry --export=get_counter %t/use-coop.o \
; RUN:   %t/def-plain-coop.o -o %t/coop.wasm 2>&1 | FileCheck %s --check-prefix=COOP
; RUN: wasm-ld --cooperative-threading --allow-undefined --no-entry --export=get_counter %t/use-coop.o \
; RUN:   %t/def-coop.o -o %t/coop-tls.wasm

; COOP:      error: relocation R_WASM_MEMORY_ADDR_TLS_SLEB cannot be used against non-TLS symbol `counter`
; COOP-NEXT: >>> defined in {{.*}}def-plain-coop.o
; COOP-NEXT: >>> referenced by {{.*}}use-coop.o:(get_counter)
; COOP-NEXT: >>> the references are thread-local; the definition is not
; COOP-NEXT: >>> the definition does not declare it thread_local
; COOP-NEXT: >>> define it as thread_local, with the same threading flags as the references
; COOP-NOT:  error:

;; wasm64: R_WASM_MEMORY_ADDR_TLS_SLEB64.
; RUN: llc --mtriple=wasm64-unknown-unknown -mattr=+atomics,+bulk-memory -filetype=obj %t/use.ll -o %t/use64.o
; RUN: llc --mtriple=wasm64-unknown-unknown -mattr=-atomics -filetype=obj %t/def.ll -o %t/def64.o
; RUN: not wasm-ld -mwasm64 --no-entry --export=get_counter %t/use64.o %t/def64.o -o %t/single64.wasm 2>&1 \
; RUN:   | FileCheck %s --check-prefix=WASM64

; WASM64:      error: relocation R_WASM_MEMORY_ADDR_TLS_SLEB64 cannot be used against non-TLS symbol `counter`
; WASM64-NEXT: >>> defined in {{.*}}def64.o

;; An undefined symbol gets only its own error.
; RUN: not wasm-ld --no-entry --export=get_counter %t/use.o -o %t/undef.wasm 2>&1 \
; RUN:   | FileCheck %s --check-prefix=UNDEF
; RUN: not wasm-ld --shared-memory --no-entry --export=get_counter %t/use.o -o %t/undef-shared.wasm 2>&1 \
; RUN:   | FileCheck %s --check-prefix=UNDEF

; UNDEF-NOT: non-TLS
; UNDEF:     relocation R_WASM_MEMORY_ADDR_TLS_SLEB cannot be used against an undefined symbol `counter`
; UNDEF-NOT: non-TLS

;; A single-threaded relocatable link keeps the relocation, and the final link
;; checks it against the definition it picks: here the -r output's own, and
;; then a strong TLS definition that wins over a weak ordinary one.
; RUN: wasm-ld -r %t/use.o %t/def.o -o %t/reloc.o
; RUN: not wasm-ld --no-entry --export=get_counter %t/reloc.o -o %t/reloc.wasm 2>&1 \
; RUN:   | FileCheck %s --check-prefix=RELOC
; RUN: wasm-ld -r %t/use.o %t/def-weak.o -o %t/reloc-weak.o
; RUN: wasm-ld --no-entry --export=get_counter %t/reloc-weak.o %t/def-tls.o -o %t/reloc-weak.wasm

; RELOC:      error: relocation R_WASM_MEMORY_ADDR_TLS_SLEB cannot be used against non-TLS symbol `counter`
; RELOC-NEXT: >>> defined in {{.*}}reloc.o
; RELOC-NEXT: >>> referenced by {{.*}}reloc.o:(get_counter)

;; ThinLTO compiles a module that defines no functions without atomics unless
;; the link has --shared-memory, so the definition loses thread_local although
;; both inputs were compiled with atomics. The message names that cause.
; RUN: opt -module-summary %t/use-lto.ll -o %t/use-thin.o
; RUN: opt -module-summary %t/def-lto.ll -o %t/def-thin.o
; RUN: not wasm-ld --no-entry --export=get_counter %t/use-thin.o %t/def-thin.o -o %t/thin.wasm 2>&1 \
; RUN:   | FileCheck %s --check-prefix=THIN

; THIN:      error: relocation R_WASM_MEMORY_ADDR_TLS_SLEB cannot be used against non-TLS symbol `counter`
; THIN-NEXT: >>> defined in {{.*}}thin.wasm.lto.def-thin.o
; THIN-NEXT: >>> referenced by {{.*}}thin.wasm.lto.use-thin.o:(get_counter)
; THIN-NEXT: >>> the references are thread-local; the definition is not
; THIN-NEXT: >>> LTO dropped thread_local: it compiles a module with no functions without atomics
; THIN-NEXT: >>> use full LTO, or link with --shared-memory (clang's -pthread)
; THIN-NOT:  error:

;; Both of those link.
; RUN: llvm-as %t/use-lto.ll -o %t/use-full.o
; RUN: llvm-as %t/def-lto.ll -o %t/def-full.o
; RUN: wasm-ld --no-entry --export=get_counter %t/use-full.o %t/def-full.o -o %t/full.wasm
; RUN: wasm-ld --shared-memory --no-entry --export=get_counter %t/use-thin.o %t/def-thin.o -o %t/thin-shared.wasm

;; The same objects against a TLS definition link: the check is on the symbol,
;; not on the link.
; RUN: wasm-ld --no-gc-sections --no-entry %t/use.o %t/def-tls.o -o %t/tls.wasm
; RUN: wasm-ld --shared-memory --no-gc-sections --no-entry %t/use.o %t/def-tls.o -o %t/tls-shared.wasm
; RUN: wasm-ld -r %t/use.o %t/def-tls.o -o %t/reloc-tls.o

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

define void @set_counter(i32 %v) {
  %p = call ptr @llvm.threadlocal.address.p0(ptr @counter)
  store i32 %v, ptr %p
  ret void
}

define void @inc_counter() {
  %p = call ptr @llvm.threadlocal.address.p0(ptr @counter)
  %v = load i32, ptr %p
  %n = add i32 %v, 1
  store i32 %n, ptr %p
  ret void
}

define void @dec_counter() {
  %p = call ptr @llvm.threadlocal.address.p0(ptr @counter)
  %v = load i32, ptr %p
  %n = sub i32 %v, 1
  store i32 %n, ptr %p
  ret void
}

;--- use2.ll
@counter = external thread_local global i32

declare ptr @llvm.threadlocal.address.p0(ptr)

define i32 @get_counter2() {
  %p = call ptr @llvm.threadlocal.address.p0(ptr @counter)
  %v = load i32, ptr %p
  ret i32 %v
}

;--- def.ll
@counter = thread_local global i32 7

;--- def-plain.ll
@counter = global i32 7

;--- def-weak.ll
@counter = weak global i32 7

;--- use-lto.ll
target datalayout = "e-m:e-p:32:32-p10:8:8-p20:8:8-i64:64-i128:128-n32:64-S128-ni:1:10:20"
target triple = "wasm32-unknown-unknown"

@counter = external thread_local global i32

declare ptr @llvm.threadlocal.address.p0(ptr)

; The store keeps ThinLTO from importing the definition as a read-only
; variable.
define i32 @get_counter() #0 {
  %p = call ptr @llvm.threadlocal.address.p0(ptr @counter)
  %v = load i32, ptr %p
  %n = add i32 %v, 1
  store i32 %n, ptr %p
  ret i32 %v
}

attributes #0 = { "target-features"="+atomics,+bulk-memory" }

;--- def-lto.ll
target datalayout = "e-m:e-p:32:32-p10:8:8-p20:8:8-i64:64-i128:128-n32:64-S128-ni:1:10:20"
target triple = "wasm32-unknown-unknown"

@counter = thread_local global i32 7
