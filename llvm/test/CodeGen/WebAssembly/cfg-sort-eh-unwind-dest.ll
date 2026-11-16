; RUN: llc < %s -wasm-enable-eh -wasm-use-legacy-eh=false -exception-model=wasm -mattr=+exception-handling -verify-machineinstrs | FileCheck %s --implicit-check-not="Invalid depth"
; RUN: llc < %s -O0 -wasm-enable-eh -wasm-use-legacy-eh=false -exception-model=wasm -mattr=+exception-handling -verify-machineinstrs | FileCheck %s --implicit-check-not="Invalid depth"

; CFGSort must place an EH pad before the EH pad that is its next unwind
; destination (a catchswitch's 'unwind label'). A catchpad's try_table catches
; only __cpp_exception, so an exception it does not catch has to reach its
; unwind destination's try_table next: that try_table has to enclose the
; catchpad's. CFGStackify nests the try_table of a later EH pad around an
; earlier one's, so the catchpad has to come first.
;
; In the first three functions the unwind destination comes first in the IR,
; and CFGSort used to keep that order. In the last three a loop defers the
; catchpad until the loop ends, while the unwind destination becomes ready on
; another path. Ordering the two only when both are ready at once does not help
; there, so the order is a readiness constraint. The checks bind each try_table
; to the EH pad it targets (its end_block label) and check the nesting.

target triple = "wasm32-unknown-unknown"

declare void @foo()
declare void @bar()
declare void @baz()
declare void @f(i32)
declare i1 @cond(i32) nounwind
declare void @mark(i32) nounwind
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
; CHECK:         call bar
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
; CHECK:         call bar
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
; CHECK:         call bar
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

; A catch_unwind whose body has a call before a loop and one in it, inside a
; drop scope that the Ok path also unwinds to:
;
;   let _g = Guard;                                    // %cleanup.y
;   match catch_unwind(|| { a(); for .. { b() } }) {   // %catch.start
;       Ok(()) => c(),                                 // unwinds to %cleanup.y
;       Err(e) => ..,
;   }
;
; %catch.start is not dominated by the loop header, so CFGSort defers it to the
; Ready queue until the loop ends. %okarm then makes %cleanup.y ready on the
; Preferred queue, which is drained first, so %cleanup.y used to be placed
; before %catch.start. %cleanup.outer encloses both, so this gave the same
; out-of-range catch_all_ref depth as @test. The call in %okarm sits inside
; %catch.start's try_table and reaches %cleanup.y through a trampoline.

; CHECK-LABEL: test_defer_outer:
; CHECK:         try_table (catch_all_ref 0) # 0: down to label[[DO_OUTER:[0-9]+]]
; CHECK:         call foo
; CHECK:         try_table (catch_all_ref 0) # 0: down to label[[DO_Y:[0-9]+]]
; CHECK-NEXT:    block exnref
; CHECK-NEXT:    block i32
; CHECK-NEXT:    try_table (catch __cpp_exception 0) # 0: down to label[[DO_CATCH:[0-9]+]]
; CHECK:         call foo
; CHECK:         loop # label{{[0-9]+}}:
; CHECK:         call bar
; CHECK:         end_loop
; CHECK:         try_table (catch_all_ref 2) # 2: down to label[[DO_TRAMP:[0-9]+]]
; CHECK:         call baz
; CHECK:       # %catch.start
; CHECK-NEXT:    end_block # label[[DO_CATCH]]:
; CHECK:         end_block # label[[DO_TRAMP]]:
; CHECK:         throw_ref
; CHECK:       # %cleanup.y
; CHECK-NEXT:    end_block # label[[DO_Y]]:
; CHECK:       # %cleanup.outer
; CHECK-NEXT:    end_block # label[[DO_OUTER]]:
; CHECK:         end_function
define void @test_defer_outer(i32 %n) personality ptr @__gxx_wasm_personality_v0 {
entry:
  invoke void @foo()
          to label %a unwind label %cleanup.outer

a:
  invoke void @foo()
          to label %loop unwind label %catch.dispatch

loop:
  %i = phi i32 [ 0, %a ], [ %i.next, %latch ]
  invoke void @bar()
          to label %latch unwind label %catch.dispatch

latch:
  %i.next = add i32 %i, 1
  %done = icmp eq i32 %i.next, %n
  br i1 %done, label %okarm, label %loop

okarm:
  invoke void @baz()
          to label %join unwind label %cleanup.y

catch.dispatch:
  %cs = catchswitch within none [label %catch.start] unwind label %cleanup.y

catch.start:
  %cp = catchpad within %cs [ptr null]
  %exn = call ptr @llvm.wasm.get.exception(token %cp)
  catchret from %cp to label %join

cleanup.y:
  %cly = cleanuppad within none []
  call void @mark(i32 1) [ "funclet"(token %cly) ]
  cleanupret from %cly unwind label %cleanup.outer

join:
  invoke void @foo()
          to label %exit unwind label %cleanup.outer

exit:
  ret void

cleanup.outer:
  %clo = cleanuppad within none []
  call void @mark(i32 2) [ "funclet"(token %clo) ]
  cleanupret from %clo unwind to caller
}

; @test_defer_outer without %cleanup.outer: the old order was valid, but with
; %cleanup.y's try_table nested inside %catch.start's, so an exception
; %catch.start does not catch, thrown by the calls before or in the loop,
; skipped %cleanup.y.

; CHECK-LABEL: test_defer_silent:
; CHECK:         call foo
; CHECK:         try_table (catch_all_ref 0) # 0: down to label[[DS_Y:[0-9]+]]
; CHECK-NEXT:    block exnref
; CHECK-NEXT:    block i32
; CHECK-NEXT:    try_table (catch __cpp_exception 0) # 0: down to label[[DS_CATCH:[0-9]+]]
; CHECK:         call foo
; CHECK:         loop # label{{[0-9]+}}:
; CHECK:         call bar
; CHECK:         end_loop
; CHECK:         try_table (catch_all_ref 2) # 2: down to label[[DS_TRAMP:[0-9]+]]
; CHECK:         call baz
; CHECK:       # %catch.start
; CHECK-NEXT:    end_block # label[[DS_CATCH]]:
; CHECK:         end_block # label[[DS_TRAMP]]:
; CHECK:         throw_ref
; CHECK:       # %cleanup.y
; CHECK-NEXT:    end_block # label[[DS_Y]]:
; CHECK:         end_function
define void @test_defer_silent(i32 %n) personality ptr @__gxx_wasm_personality_v0 {
entry:
  call void @foo()
  br label %a

a:
  invoke void @foo()
          to label %loop unwind label %catch.dispatch

loop:
  %i = phi i32 [ 0, %a ], [ %i.next, %latch ]
  invoke void @bar()
          to label %latch unwind label %catch.dispatch

latch:
  %i.next = add i32 %i, 1
  %done = icmp eq i32 %i.next, %n
  br i1 %done, label %okarm, label %loop

okarm:
  invoke void @baz()
          to label %join unwind label %cleanup.y

catch.dispatch:
  %cs = catchswitch within none [label %catch.start] unwind label %cleanup.y

catch.start:
  %cp = catchpad within %cs [ptr null]
  %exn = call ptr @llvm.wasm.get.exception(token %cp)
  catchret from %cp to label %join

cleanup.y:
  %cly = cleanuppad within none []
  call void @mark(i32 1) [ "funclet"(token %cly) ]
  cleanupret from %cly unwind to caller

join:
  call void @foo()
  ret void
}

; Three nested catches: %c3 in %c2 in %c1. %c3's body has a call before a loop
; and one in it, so the loop defers %c3; %ok3, after %c3's normal exit, unwinds
; to %c2. When %ok3 is placed, %c2 and %c1 become ready on the Preferred queue
; while %c3 waits on Ready. Breaking that tie by unwind order alone would place
; %c2 before %c3 (invalid at -O0); breaking it by block number happened to
; work at -O0 and failed at -O2. The try_tables must nest %c1 around %c2 around
; %c3.

; CHECK-LABEL: test_nest3:
; CHECK:         try_table (catch __cpp_exception 0) # 0: down to label[[N_C1:[0-9]+]]
; CHECK:         try_table (catch __cpp_exception 0) # 0: down to label[[N_C2:[0-9]+]]
; CHECK:         try_table (catch __cpp_exception 0) # 0: down to label[[N_C3:[0-9]+]]
; CHECK:         call f
; CHECK:         loop # label{{[0-9]+}}:
; CHECK:         end_loop
; CHECK:         try_table (catch_all_ref {{[0-9]+}}) # {{[0-9]+}}: down to label[[N_TRAMP:[0-9]+]]
; CHECK:         call f
; CHECK:       # %c3
; CHECK-NEXT:    end_block # label[[N_C3]]:
; CHECK:         end_block # label[[N_TRAMP]]:
; CHECK:         throw_ref
; CHECK:       # %c2
; CHECK-NEXT:    end_block # label[[N_C2]]:
; CHECK:       # %c1
; CHECK-NEXT:    end_block # label[[N_C1]]:
; CHECK:         end_function
define void @test_nest3() personality ptr @__gxx_wasm_personality_v0 {
entry:
  br label %pre

d1:
  %cs1 = catchswitch within none [label %c1] unwind to caller

c1:
  %p1 = catchpad within %cs1 [ptr null]
  %e1 = call ptr @llvm.wasm.get.exception(token %p1)
  call void @mark(i32 1) [ "funclet"(token %p1) ]
  catchret from %p1 to label %err1

d2:
  %cs2 = catchswitch within none [label %c2] unwind label %d1

c2:
  %p2 = catchpad within %cs2 [ptr null]
  %e2 = call ptr @llvm.wasm.get.exception(token %p2)
  call void @mark(i32 2) [ "funclet"(token %p2) ]
  catchret from %p2 to label %err2

d3:
  %cs3 = catchswitch within none [label %c3] unwind label %d2

c3:
  %p3 = catchpad within %cs3 [ptr null]
  %e3 = call ptr @llvm.wasm.get.exception(token %p3)
  call void @mark(i32 3) [ "funclet"(token %p3) ]
  catchret from %p3 to label %err3

pre:
  invoke void @f(i32 1)
          to label %loop unwind label %d3

loop:
  invoke void @f(i32 2)
          to label %latch unwind label %d3

latch:
  %c = call i1 @cond(i32 1)
  br i1 %c, label %loop, label %ok3

ok3:
  invoke void @f(i32 3)
          to label %join3 unwind label %d2

err3:
  br label %join3

join3:
  br label %join2

err2:
  br label %join2

join2:
  br label %join1

err1:
  br label %join1

join1:
  ret void
}
