; RUN: opt -S -passes=loop-opt-tutorial -debug-only=loop-opt-tutorial < %s 2>&1 | FileCheck %s
; REQUIRES: asserts

@B = common global [1024 x i32] zeroinitializer, align 16

; CHECK: Entering LoopOptTutorialPass::run
define void @dep_free(i32* noalias %arg) {
bb:
  br label %bb5

bb5:                                              ; preds = %bb14, %bb
  %indvars.iv2 = phi i64 [ %indvars.iv.next3, %bb14 ], [ 0, %bb ]
  %.01 = phi i32 [ 0, %bb ], [ %tmp15, %bb14 ]
  %exitcond4 = icmp ne i64 %indvars.iv2, 100
  br i1 %exitcond4, label %bb7, label %bb17

bb7:                                              ; preds = %bb5
  %tmp = add nsw i32 %.01, -3
  %tmp8 = add nuw nsw i64 %indvars.iv2, 3
  %tmp9 = trunc i64 %tmp8 to i32
  %tmp10 = mul nsw i32 %tmp, %tmp9
  %tmp11 = trunc i64 %indvars.iv2 to i32
  %tmp12 = srem i32 %tmp10, %tmp11
  %tmp13 = getelementptr inbounds i32, i32* %arg, i64 %indvars.iv2
  store i32 %tmp12, i32* %tmp13, align 4
  br label %bb14

bb14:                                             ; preds = %bb7
  %indvars.iv.next3 = add nuw nsw i64 %indvars.iv2, 1
  %tmp15 = add nuw nsw i32 %.01, 1
  br label %bb5

bb17:                                             ; preds = %bb5
  ret void
}
