target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx15.6.0"
%"struct.metal::_atomic" = type { i32 }
@tg = internal unnamed_addr addrspace(3) global [8 x i32] undef, align 4
@tga = internal unnamed_addr addrspace(3) global [4 x %"struct.metal::_atomic"] undef, align 4
declare void @air.wg.barrier(i32, i32)
declare i32 @air.atomic.local.load.i32(ptr addrspace(3), i32, i32, i1)
declare void @air.atomic.local.store.i32(ptr addrspace(3), i32, i32, i32, i1)
declare i32 @air.atomic.local.add.u.i32(ptr addrspace(3), i32, i32, i32, i32, i1)
declare i32 @air.atomic.local.or.u.i32(ptr addrspace(3), i32, i32, i32, i32, i1)
declare i32 @air.atomic.local.xor.u.i32(ptr addrspace(3), i32, i32, i32, i32, i1)
declare i32 @air.atomic.local.max.u.i32(ptr addrspace(3), i32, i32, i32, i32, i1)
declare i32 @air.atomic.local.min.s.i32(ptr addrspace(3), i32, i32, i32, i32, i1)
declare i32 @air.atomic.local.and.u.i32(ptr addrspace(3), i32, i32, i32, i32, i1)
declare i32 @air.atomic.local.xchg.i32(ptr addrspace(3), i32, i32, i32, i32, i1)
define void @atomic_tg(ptr addrspace(1) %o, i32 %lid, i32 %gid) {
entry:
  %lid1 = add i32 %lid, 1
  %lid7 = add i32 %lid, 7
  %lidm20 = sub i32 %lid, 20
  %bit = and i32 %lid, 31
  %one = shl i32 1, %bit
  %w0 = getelementptr inbounds [8 x i32], ptr addrspace(3) @tg, i64 0, i64 0
  %w1 = getelementptr inbounds [8 x i32], ptr addrspace(3) @tg, i64 0, i64 1
  %w2 = getelementptr inbounds [8 x i32], ptr addrspace(3) @tg, i64 0, i64 2
  %w3 = getelementptr inbounds [8 x i32], ptr addrspace(3) @tg, i64 0, i64 3
  %w4 = getelementptr inbounds [8 x i32], ptr addrspace(3) @tg, i64 0, i64 4
  %w5 = getelementptr inbounds [8 x i32], ptr addrspace(3) @tg, i64 0, i64 5
  %w6 = getelementptr inbounds [8 x i32], ptr addrspace(3) @tg, i64 0, i64 6
  %w7 = getelementptr inbounds [8 x i32], ptr addrspace(3) @tg, i64 0, i64 7
  %a0 = getelementptr inbounds [4 x %"struct.metal::_atomic"], ptr addrspace(3) @tga, i64 0, i64 0, i32 0
  %a1 = getelementptr inbounds [4 x %"struct.metal::_atomic"], ptr addrspace(3) @tga, i64 0, i64 1, i32 0
  %a2 = getelementptr inbounds [4 x %"struct.metal::_atomic"], ptr addrspace(3) @tga, i64 0, i64 2, i32 0
  %a3 = getelementptr inbounds [4 x %"struct.metal::_atomic"], ptr addrspace(3) @tga, i64 0, i64 3, i32 0
  ; initial values: w0 = 0 (sum), w1 = 0 (or), w2 = 0 (xor), w3 = 0 (umax), w4 = 0x7fffffff (smin), w5 = 0xffffffff (and), w6 = 0 (ticket), w7 = 0;  a0 = 0 (sum), a1 = 0 (or), a2 = 5 (xchg), a3 = 0
  call void @air.atomic.local.store.i32(ptr addrspace(3) %w0, i32 0, i32 0, i32 2, i1 true)
  call void @air.atomic.local.store.i32(ptr addrspace(3) %w1, i32 0, i32 0, i32 2, i1 true)
  call void @air.atomic.local.store.i32(ptr addrspace(3) %w2, i32 0, i32 0, i32 2, i1 true)
  call void @air.atomic.local.store.i32(ptr addrspace(3) %w3, i32 0, i32 0, i32 2, i1 true)
  call void @air.atomic.local.store.i32(ptr addrspace(3) %w4, i32 2147483647, i32 0, i32 2, i1 true)
  call void @air.atomic.local.store.i32(ptr addrspace(3) %w5, i32 -1, i32 0, i32 2, i1 true)
  call void @air.atomic.local.store.i32(ptr addrspace(3) %w6, i32 0, i32 0, i32 2, i1 true)
  call void @air.atomic.local.store.i32(ptr addrspace(3) %w7, i32 0, i32 0, i32 2, i1 true)
  call void @air.atomic.local.store.i32(ptr addrspace(3) %a0, i32 0, i32 0, i32 2, i1 true)
  call void @air.atomic.local.store.i32(ptr addrspace(3) %a1, i32 0, i32 0, i32 2, i1 true)
  call void @air.atomic.local.store.i32(ptr addrspace(3) %a2, i32 5, i32 0, i32 2, i1 true)
  call void @air.atomic.local.store.i32(ptr addrspace(3) %a3, i32 0, i32 0, i32 2, i1 true)
  call void @air.wg.barrier(i32 2, i32 1)
  %r0 = call i32 @air.atomic.local.add.u.i32(ptr addrspace(3) %w0, i32 %lid1, i32 0, i32 2, i32 0, i1 true)
  %r1 = call i32 @air.atomic.local.or.u.i32(ptr addrspace(3) %w1, i32 %one, i32 0, i32 2, i32 0, i1 true)
  %r2 = call i32 @air.atomic.local.xor.u.i32(ptr addrspace(3) %w2, i32 %lid1, i32 0, i32 2, i32 0, i1 true)
  %r3 = call i32 @air.atomic.local.max.u.i32(ptr addrspace(3) %w3, i32 %lid7, i32 0, i32 2, i32 0, i1 true)
  %r4 = call i32 @air.atomic.local.min.s.i32(ptr addrspace(3) %w4, i32 %lidm20, i32 0, i32 2, i32 0, i1 true)
  %r5 = call i32 @air.atomic.local.and.u.i32(ptr addrspace(3) %w5, i32 -1, i32 0, i32 2, i32 0, i1 true)
  %tk = call i32 @air.atomic.local.add.u.i32(ptr addrspace(3) %w6, i32 1, i32 0, i32 2, i32 0, i1 true)
  %q0 = call i32 @air.atomic.local.add.u.i32(ptr addrspace(3) %a0, i32 2, i32 0, i32 2, i32 0, i1 true)
  %q1 = call i32 @air.atomic.local.or.u.i32(ptr addrspace(3) %a1, i32 %one, i32 0, i32 2, i32 0, i1 true)
  %q2 = call i32 @air.atomic.local.xchg.i32(ptr addrspace(3) %a2, i32 %lid1, i32 0, i32 2, i32 0, i1 true)
  call void @air.wg.barrier(i32 2, i32 1)
  %g64 = zext i32 %gid to i64
  %gb = mul i64 %g64, 16
  %l64 = zext i32 %lid to i64
  %v0 = call i32 @air.atomic.local.load.i32(ptr addrspace(3) %w0, i32 0, i32 2, i1 true)
  %ob0 = add i64 %gb, 0
  %op0 = getelementptr i32, ptr addrspace(1) %o, i64 %ob0
  store i32 %v0, ptr addrspace(1) %op0
  %v1 = call i32 @air.atomic.local.load.i32(ptr addrspace(3) %w1, i32 0, i32 2, i1 true)
  %ob1 = add i64 %gb, 1
  %op1 = getelementptr i32, ptr addrspace(1) %o, i64 %ob1
  store i32 %v1, ptr addrspace(1) %op1
  %v2 = call i32 @air.atomic.local.load.i32(ptr addrspace(3) %w2, i32 0, i32 2, i1 true)
  %ob2 = add i64 %gb, 2
  %op2 = getelementptr i32, ptr addrspace(1) %o, i64 %ob2
  store i32 %v2, ptr addrspace(1) %op2
  %v3 = call i32 @air.atomic.local.load.i32(ptr addrspace(3) %w3, i32 0, i32 2, i1 true)
  %ob3 = add i64 %gb, 3
  %op3 = getelementptr i32, ptr addrspace(1) %o, i64 %ob3
  store i32 %v3, ptr addrspace(1) %op3
  %v4 = call i32 @air.atomic.local.load.i32(ptr addrspace(3) %w4, i32 0, i32 2, i1 true)
  %ob4 = add i64 %gb, 4
  %op4 = getelementptr i32, ptr addrspace(1) %o, i64 %ob4
  store i32 %v4, ptr addrspace(1) %op4
  %v5 = call i32 @air.atomic.local.load.i32(ptr addrspace(3) %w5, i32 0, i32 2, i1 true)
  %ob5 = add i64 %gb, 5
  %op5 = getelementptr i32, ptr addrspace(1) %o, i64 %ob5
  store i32 %v5, ptr addrspace(1) %op5
  %v6 = call i32 @air.atomic.local.load.i32(ptr addrspace(3) %w6, i32 0, i32 2, i1 true)
  %ob6 = add i64 %gb, 6
  %op6 = getelementptr i32, ptr addrspace(1) %o, i64 %ob6
  store i32 %v6, ptr addrspace(1) %op6
  %v7 = call i32 @air.atomic.local.load.i32(ptr addrspace(3) %w7, i32 0, i32 2, i1 true)
  %ob7 = add i64 %gb, 7
  %op7 = getelementptr i32, ptr addrspace(1) %o, i64 %ob7
  store i32 %v7, ptr addrspace(1) %op7
  %u0 = call i32 @air.atomic.local.load.i32(ptr addrspace(3) %a0, i32 0, i32 2, i1 true)
  %oc0 = add i64 %gb, 8
  %oq0 = getelementptr i32, ptr addrspace(1) %o, i64 %oc0
  store i32 %u0, ptr addrspace(1) %oq0
  %u1 = call i32 @air.atomic.local.load.i32(ptr addrspace(3) %a1, i32 0, i32 2, i1 true)
  %oc1 = add i64 %gb, 9
  %oq1 = getelementptr i32, ptr addrspace(1) %o, i64 %oc1
  store i32 %u1, ptr addrspace(1) %oq1
  %u2 = call i32 @air.atomic.local.load.i32(ptr addrspace(3) %a2, i32 0, i32 2, i1 true)
  %oc2 = add i64 %gb, 10
  %oq2 = getelementptr i32, ptr addrspace(1) %o, i64 %oc2
  store i32 %u2, ptr addrspace(1) %oq2
  %u3 = call i32 @air.atomic.local.load.i32(ptr addrspace(3) %a3, i32 0, i32 2, i1 true)
  %oc3 = add i64 %gb, 11
  %oq3 = getelementptr i32, ptr addrspace(1) %o, i64 %oc3
  store i32 %u3, ptr addrspace(1) %oq3
  %gt = mul i64 %g64, 64
  %tb = add i64 %gt, %l64
  %tc = add i64 %tb, 64
  %tp = getelementptr i32, ptr addrspace(1) %o, i64 %tc
  store i32 %tk, ptr addrspace(1) %tp
  ret void
}
!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 7, i32 0}
!2 = !{!"Metal", i32 3, i32 2, i32 0}
!3 = !{}
!air.kernel = !{!20}
!20 = !{ptr @atomic_tg, !3, !19}
!10 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"o"}
!12 = !{i32 1, !"air.thread_index_in_threadgroup", !"air.arg_type_name", !"uint", !"air.arg_name", !"lid"}
!13 = !{i32 2, !"air.threadgroup_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"gid"}
!19 = !{!10, !12, !13}
