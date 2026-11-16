; RUN: llc < %s -wasm-enable-eh -wasm-use-legacy-eh=false -exception-model=wasm -mattr=+exception-handling -verify-machineinstrs | FileCheck %s --implicit-check-not="Invalid depth"
; RUN: llc < %s -O0 -wasm-enable-eh -wasm-use-legacy-eh=false -exception-model=wasm -mattr=+exception-handling -verify-machineinstrs | FileCheck %s --implicit-check-not="Invalid depth"

; CFGSort must place an EH pad before the EH pad that is its next unwind
; destination (a catchswitch's 'unwind label'), whatever their order in the IR.
; A catchpad's try_table catches only __cpp_exception, so an exception it does
; not catch has to reach its unwind destination's try_table next: that
; try_table has to enclose the catchpad's. CFGStackify nests the try_table of
; a later EH pad around an earlier one's, so the catchpad has to come first.
;
; In each function below the unwind destination comes first in the IR, and
; CFGSort used to keep that order. The checks bind each try_table to the EH pad
; it targets (its end_block label) and check the nesting.

target triple = "wasm32-unknown-unknown"

declare void @foo()
declare void @bar()
declare i32 @__gxx_wasm_personality_v0(...)
declare ptr @llvm.wasm.get.exception(token)

; %try has two EH pad successors: %catch.start (the catchswitch's catchpad) and
; %cleanup.b (the catchswitch's unwind destination). %cleanup.a encloses both.
; With %cleanup.b sorted first, its try_table ended up inside %catch.start's,
; and the try_table fixCatchUnwindMismatches adds around %catch.start's (whose
; next unwind destination looked like %cleanup.a) targeted a trampoline inside
; itself: an out-of-range catch_all_ref depth, an invalid module ("catch_all_ref
; label must a subtype of (ref exn)"), or the "Branch destination should be in
; scope" assertion.

; CHECK-LABEL: test:
; CHECK:         try_table (catch_all_ref 0) # 0: down to label[[CLEANUP_A:[0-9]+]]
; CHECK:         call foo
; CHECK:         block exnref
; CHECK-NEXT:    try_table (catch_all_ref 0) # 0: down to label[[CLEANUP_B:[0-9]+]]
; CHECK-NEXT:    block i32
; CHECK-NEXT:    try_table (catch __cpp_exception 0) # 0: down to label[[CATCH:[0-9]+]]
; CHECK-NEXT:  # EH_LABEL
; CHECK-NEXT:    call bar
; CHECK:       # %catch.start
; CHECK-NEXT:    end_block # label[[CATCH]]:
; CHECK:       # %cleanup.b
; CHECK-NEXT:    end_block # label[[CLEANUP_B]]:
; CHECK:       # %after
; CHECK:         call foo
; CHECK:       # %cleanup.a
; CHECK-NEXT:    end_block # label[[CLEANUP_A]]:
; CHECK:         end_function
define i32 @test(i1 %cond) personality ptr @__gxx_wasm_personality_v0 {
entry:
  invoke void @foo()
          to label %cont unwind label %cleanup.a

cont:
  br i1 %cond, label %try, label %exit

exit:
  ret i32 0

cleanup.b:
  %clb = cleanuppad within none []
  cleanupret from %clb unwind to caller

try:
  invoke void @bar()
          to label %after unwind label %catch.dispatch

after:
  invoke void @foo()
          to label %exit unwind label %cleanup.a

catch.dispatch:
  %cs = catchswitch within none [label %catch.start] unwind label %cleanup.b

catch.start:
  %cp = catchpad within %cs [ptr null]
  %exn = call ptr @llvm.wasm.get.exception(token %cp)
  catchret from %cp to label %exit

cleanup.a:
  %cla = cleanuppad within none []
  cleanupret from %cla unwind to caller
}

; @test with the first invoke made a call: no EH pad encloses %catch.start, so
; the old order gave a valid module, but with %cleanup.b's try_table nested
; inside %catch.start's. fixCatchUnwindMismatches found no unwind destination
; there ("does not exist anymore"), and an exception %catch.start does not
; catch (another tag than __cpp_exception) went to the caller without running
; %cleanup.b.

; CHECK-LABEL: test_silent:
; CHECK:         call foo
; CHECK:         block exnref
; CHECK-NEXT:    try_table (catch_all_ref 0) # 0: down to label[[S_CLEANUP_B:[0-9]+]]
; CHECK-NEXT:    block i32
; CHECK-NEXT:    try_table (catch __cpp_exception 0) # 0: down to label[[S_CATCH:[0-9]+]]
; CHECK-NEXT:  # EH_LABEL
; CHECK-NEXT:    call bar
; CHECK:       # %catch.start
; CHECK-NEXT:    end_block # label[[S_CATCH]]:
; CHECK:       # %cleanup.b
; CHECK-NEXT:    end_block # label[[S_CLEANUP_B]]:
; CHECK:       # %cleanup.a
; CHECK:         end_function
define i32 @test_silent(i1 %cond) personality ptr @__gxx_wasm_personality_v0 {
entry:
  call void @foo()
  br label %cont

cont:
  br i1 %cond, label %try, label %exit

exit:
  ret i32 0

cleanup.b:
  %clb = cleanuppad within none []
  cleanupret from %clb unwind to caller

try:
  invoke void @bar()
          to label %after unwind label %catch.dispatch

after:
  invoke void @foo()
          to label %exit unwind label %cleanup.a

catch.dispatch:
  %cs = catchswitch within none [label %catch.start] unwind label %cleanup.b

catch.start:
  %cp = catchpad within %cs [ptr null]
  %exn = call ptr @llvm.wasm.get.exception(token %cp)
  catchret from %cp to label %exit

cleanup.a:
  %cla = cleanuppad within none []
  cleanupret from %cla unwind to caller
}

; A chain of three: %catch.a's catchswitch unwinds to %catch.b's, which
; unwinds to %cleanup.c. The IR lists them in the opposite order. The
; try_tables nest %cleanup.c around %catch.b around %catch.a.

; CHECK-LABEL: test_chain3:
; CHECK:         try_table (catch_all_ref 0) # 0: down to label[[C_CLEANUP_D:[0-9]+]]
; CHECK:         call foo
; CHECK:         block exnref
; CHECK-NEXT:    try_table (catch_all_ref 0) # 0: down to label[[C_CLEANUP_C:[0-9]+]]
; CHECK-NEXT:    block i32
; CHECK-NEXT:    try_table (catch __cpp_exception 0) # 0: down to label[[C_CATCH_B:[0-9]+]]
; CHECK-NEXT:    block i32
; CHECK-NEXT:    try_table (catch __cpp_exception 0) # 0: down to label[[C_CATCH_A:[0-9]+]]
; CHECK-NEXT:  # EH_LABEL
; CHECK-NEXT:    call bar
; CHECK:       # %catch.a.start
; CHECK-NEXT:    end_block # label[[C_CATCH_A]]:
; CHECK:       # %catch.b.start
; CHECK-NEXT:    end_block # label[[C_CATCH_B]]:
; CHECK:       # %cleanup.c
; CHECK-NEXT:    end_block # label[[C_CLEANUP_C]]:
; CHECK:       # %after
; CHECK:         call foo
; CHECK:       # %cleanup.d
; CHECK-NEXT:    end_block # label[[C_CLEANUP_D]]:
; CHECK:         end_function
define i32 @test_chain3(i1 %cond) personality ptr @__gxx_wasm_personality_v0 {
entry:
  invoke void @foo()
          to label %cont unwind label %cleanup.d

cont:
  br i1 %cond, label %try, label %exit

exit:
  ret i32 0

cleanup.c:
  %clc = cleanuppad within none []
  cleanupret from %clc unwind to caller

catch.b.start:
  %cpb = catchpad within %csb [ptr null]
  %exnb = call ptr @llvm.wasm.get.exception(token %cpb)
  catchret from %cpb to label %exit

catch.a.start:
  %cpa = catchpad within %csa [ptr null]
  %exna = call ptr @llvm.wasm.get.exception(token %cpa)
  catchret from %cpa to label %exit

try:
  invoke void @bar()
          to label %after unwind label %catch.a.dispatch

after:
  invoke void @foo()
          to label %exit unwind label %cleanup.d

catch.a.dispatch:
  %csa = catchswitch within none [label %catch.a.start] unwind label %catch.b.dispatch

catch.b.dispatch:
  %csb = catchswitch within none [label %catch.b.start] unwind label %cleanup.c

cleanup.d:
  %cld = cleanuppad within none []
  cleanupret from %cld unwind to caller
}
