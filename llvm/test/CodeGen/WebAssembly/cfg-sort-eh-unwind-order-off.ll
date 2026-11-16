; RUN: llc < %s -wasm-enable-eh -wasm-use-legacy-eh=false -exception-model=wasm -mattr=+exception-handling -verify-machineinstrs | FileCheck %s --check-prefix=ON
; RUN: llc < %s -wasm-enable-eh -wasm-use-legacy-eh=false -exception-model=wasm -mattr=+exception-handling -verify-machineinstrs -wasm-ehpad-unwind-order=false | FileCheck %s --check-prefix=OFF

; -wasm-ehpad-unwind-order=false turns off the rule that CFGSort places an EH
; pad before its next unwind destination (see cfg-sort-eh-unwind-dest.ll), and
; gives the earlier block order. This function is valid either way, but only
; with the rule does %cleanup.b's try_table enclose %catch.start's, so that an
; exception %catch.start does not catch (a tag other than __cpp_exception) runs
; %cleanup.b. Without the rule %cleanup.b comes first, its try_table sits
; inside %catch.start's, and such an exception goes to the caller.

target triple = "wasm32-unknown-unknown"

declare void @foo()
declare void @bar()
declare i32 @__gxx_wasm_personality_v0(...)
declare ptr @llvm.wasm.get.exception(token)

; ON-LABEL: test_order:
; ON:         call foo
; ON:         block exnref
; ON-NEXT:    try_table (catch_all_ref 0) # 0: down to label[[ON_CLEANUP_B:[0-9]+]]
; ON-NEXT:    block i32
; ON-NEXT:    try_table (catch __cpp_exception 0) # 0: down to label[[ON_CATCH:[0-9]+]]
; ON:         call bar
; ON:       # %catch.start
; ON-NEXT:    end_block # label[[ON_CATCH]]:
; ON:       # %cleanup.b
; ON-NEXT:    end_block # label[[ON_CLEANUP_B]]:
; ON:         end_function

; OFF-LABEL: test_order:
; OFF:         call foo
; OFF:         block i32
; OFF-NEXT:    try_table (catch __cpp_exception 0) # 0: down to label[[OFF_CATCH:[0-9]+]]
; OFF:         try_table (catch_all_ref 0) # 0: down to label[[OFF_CLEANUP_B:[0-9]+]]
; OFF:         call bar
; OFF:       # %cleanup.b
; OFF-NEXT:    end_block # label[[OFF_CLEANUP_B]]:
; OFF:       # %catch.start
; OFF-NEXT:    end_block # label[[OFF_CATCH]]:
; OFF:         end_function
define i32 @test_order(i1 %cond) personality ptr @__gxx_wasm_personality_v0 {
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
