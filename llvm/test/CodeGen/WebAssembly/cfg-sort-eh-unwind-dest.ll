; RUN: llc < %s -wasm-enable-eh -wasm-use-legacy-eh=false -exception-model=wasm -mattr=+exception-handling -verify-machineinstrs | FileCheck %s
; RUN: llc < %s -O0 -wasm-enable-eh -wasm-use-legacy-eh=false -exception-model=wasm -mattr=+exception-handling -verify-machineinstrs | FileCheck %s

; %try has two EH pad successors: %catch.start (the catchswitch's catchpad) and
; %cleanup.b (the catchswitch's unwind destination). An exception that
; %catch.start does not catch goes to %cleanup.b, so %cleanup.b's try_table has
; to enclose %catch.start's, and CFGSort has to place %catch.start first.
; %cleanup.b comes first in the IR. When CFGSort kept that order, %cleanup.b's
; try_table ended up inside %catch.start's, and the nested try_table that
; fixCatchUnwindMismatches adds around %catch.start's try_table (because
; %cleanup.a encloses it) targeted a trampoline inside itself: an out-of-range
; catch_all_ref depth, or the "Branch destination should be in scope"
; assertion.

target triple = "wasm32-unknown-unknown"

declare void @foo()
declare void @bar()
declare i32 @__gxx_wasm_personality_v0(...)
declare ptr @llvm.wasm.get.exception(token)

; CHECK-LABEL: test:
; CHECK:        call foo
; CHECK:        block exnref
; CHECK-NEXT:   try_table (catch_all_ref 0)
; CHECK-NEXT:   block i32
; CHECK-NEXT:   try_table (catch __cpp_exception 0)
; CHECK:        call bar
; CHECK:        end_try_table
; CHECK:        end_block
; CHECK:        end_try_table
; CHECK:        end_block
; CHECK-NOT:    Invalid depth
; CHECK:        end_function
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
