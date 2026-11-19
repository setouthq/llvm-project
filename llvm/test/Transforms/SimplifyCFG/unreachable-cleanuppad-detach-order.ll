; RUN: opt -passes=simplifycfg -S < %s | FileCheck %s

; An unreachable cleanuppad whose cleanuprets (more than four, so a small set
; would no longer keep them in insertion order) unwind to a reachable cleanup
; with a PHI. detachDeadBlocks removes each cleanupret block's PHI entry by
; swapping the last entry into its place, so the order it visits them in
; decides the surviving operand order. That order must come from the IR (the
; pad's users), never from heap addresses: before the fix the three surviving
; operands came out in a different order from run to run.

declare void @g1()
declare void @g2()
declare void @g3()
declare void @use(i32)

define void @unreachable_cleanuppad_many_rets(i32 %a, i32 %b, i32 %c, i1 %x, i1 %y) personality ptr null {
; CHECK-LABEL: define void @unreachable_cleanuppad_many_rets(
; CHECK:       live:
; CHECK-NEXT:    %p = phi i32 [ %a, %ia ], [ %b, %ib ], [ %c, %ic ]
; CHECK-NEXT:    %lp = cleanuppad within none []
; CHECK-NOT:     cleanupret from %pad
; CHECK:       exit:
; CHECK-NEXT:    ret void
entry:
  br i1 %x, label %ia, label %e2

e2:
  br i1 %y, label %ib, label %ic

ia:
  invoke void @g1()
          to label %exit unwind label %live

ib:
  invoke void @g2()
          to label %exit unwind label %live

ic:
  invoke void @g3()
          to label %exit unwind label %live

dead:
  %pad = cleanuppad within none []
  switch i32 %a, label %r1 [
    i32 1, label %r2
    i32 2, label %r3
    i32 3, label %r4
    i32 4, label %r5
    i32 5, label %r6
  ]

r1:
  cleanupret from %pad unwind label %live

r2:
  cleanupret from %pad unwind label %live

r3:
  cleanupret from %pad unwind label %live

r4:
  cleanupret from %pad unwind label %live

r5:
  cleanupret from %pad unwind label %live

r6:
  cleanupret from %pad unwind label %live

live:
  %p = phi i32 [ 1, %r1 ], [ 2, %r2 ], [ 3, %r3 ], [ 4, %r4 ], [ 5, %r5 ], [ 6, %r6 ], [ %a, %ia ], [ %b, %ib ], [ %c, %ic ]
  %lp = cleanuppad within none []
  call void @use(i32 %p) [ "funclet"(token %lp) ]
  cleanupret from %lp unwind to caller

exit:
  ret void
}

define void @unreachable_cleanuppad_eight_rets(i32 %a, i32 %b, i32 %c, i1 %x, i1 %y) personality ptr null {
; CHECK-LABEL: define void @unreachable_cleanuppad_eight_rets(
; CHECK:       live:
; CHECK-NEXT:    %p = phi i32 [ %b, %ib ], [ %c, %ic ], [ %a, %ia ]
; CHECK-NEXT:    %lp = cleanuppad within none []
; CHECK-NOT:     cleanupret from %pad
; CHECK:       exit:
; CHECK-NEXT:    ret void
entry:
  br i1 %x, label %ia, label %e2

e2:
  br i1 %y, label %ib, label %ic

ia:
  invoke void @g1()
          to label %exit unwind label %live

ib:
  invoke void @g2()
          to label %exit unwind label %live

ic:
  invoke void @g3()
          to label %exit unwind label %live

dead:
  %pad = cleanuppad within none []
  switch i32 %a, label %r1 [
    i32 1, label %r2
    i32 2, label %r3
    i32 3, label %r4
    i32 4, label %r5
    i32 5, label %r6
    i32 6, label %r7
    i32 7, label %r8
  ]

r1:
  cleanupret from %pad unwind label %live

r2:
  cleanupret from %pad unwind label %live

r3:
  cleanupret from %pad unwind label %live

r4:
  cleanupret from %pad unwind label %live

r5:
  cleanupret from %pad unwind label %live

r6:
  cleanupret from %pad unwind label %live

r7:
  cleanupret from %pad unwind label %live

r8:
  cleanupret from %pad unwind label %live

live:
  %p = phi i32 [ 1, %r1 ], [ 2, %r2 ], [ 3, %r3 ], [ 4, %r4 ], [ 5, %r5 ], [ 6, %r6 ], [ 7, %r7 ], [ 8, %r8 ], [ %a, %ia ], [ %b, %ib ], [ %c, %ic ]
  %lp = cleanuppad within none []
  call void @use(i32 %p) [ "funclet"(token %lp) ]
  cleanupret from %lp unwind to caller

exit:
  ret void
}
