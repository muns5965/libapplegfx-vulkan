target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx15.6.0"
declare i32 @air.atomic.global.load.i32(ptr addrspace(1), i32, i32, i1)
declare void @air.atomic.global.store.i32(ptr addrspace(1), i32, i32, i32, i1)
declare i32 @air.atomic.global.xchg.i32(ptr addrspace(1), i32, i32, i32, i32, i1)
declare i32 @air.atomic.global.add.u.i32(ptr addrspace(1), i32, i32, i32, i32, i1)
declare i32 @air.atomic.global.add.s.i32(ptr addrspace(1), i32, i32, i32, i32, i1)
declare i32 @air.atomic.global.sub.u.i32(ptr addrspace(1), i32, i32, i32, i32, i1)
declare i32 @air.atomic.global.and.u.i32(ptr addrspace(1), i32, i32, i32, i32, i1)
declare i32 @air.atomic.global.or.u.i32(ptr addrspace(1), i32, i32, i32, i32, i1)
declare i32 @air.atomic.global.xor.u.i32(ptr addrspace(1), i32, i32, i32, i32, i1)
declare i32 @air.atomic.global.min.u.i32(ptr addrspace(1), i32, i32, i32, i32, i1)
declare i32 @air.atomic.global.max.u.i32(ptr addrspace(1), i32, i32, i32, i32, i1)
declare i32 @air.atomic.global.min.s.i32(ptr addrspace(1), i32, i32, i32, i32, i1)
declare i32 @air.atomic.global.max.s.i32(ptr addrspace(1), i32, i32, i32, i32, i1)
define void @atomic_rmw(ptr addrspace(1) %c, ptr addrspace(1) %o, i32 %tid) {
entry:
  %t1 = add i32 %tid, 1
  %tid7 = add i32 %tid, 7
  %tidm100 = sub i32 %tid, 100
  %bit = and i32 %tid, 31
  %one = shl i32 1, %bit
  %nbit = xor i32 %one, -1
  %t64 = zext i32 %tid to i64
  %p0 = getelementptr i32, ptr addrspace(1) %c, i64 0
  %p1 = getelementptr i32, ptr addrspace(1) %c, i64 1
  %p2 = getelementptr i32, ptr addrspace(1) %c, i64 2
  %p3 = getelementptr i32, ptr addrspace(1) %c, i64 3
  %p4 = getelementptr i32, ptr addrspace(1) %c, i64 4
  %p5 = getelementptr i32, ptr addrspace(1) %c, i64 5
  %p6 = getelementptr i32, ptr addrspace(1) %c, i64 6
  %p7 = getelementptr i32, ptr addrspace(1) %c, i64 7
  %p8 = getelementptr i32, ptr addrspace(1) %c, i64 8
  %p9 = getelementptr i32, ptr addrspace(1) %c, i64 9
  %p10 = getelementptr i32, ptr addrspace(1) %c, i64 10
  %p11 = getelementptr i32, ptr addrspace(1) %c, i64 11
  %p12 = getelementptr i32, ptr addrspace(1) %c, i64 12
  %p13 = getelementptr i32, ptr addrspace(1) %c, i64 13
  %p14 = getelementptr i32, ptr addrspace(1) %c, i64 14
  %r0 = call i32 @air.atomic.global.add.u.i32(ptr addrspace(1) %p0, i32 %t1, i32 0, i32 2, i32 0, i1 true)
  %r1 = call i32 @air.atomic.global.sub.u.i32(ptr addrspace(1) %p1, i32 1, i32 0, i32 2, i32 0, i1 true)
  %r2 = call i32 @air.atomic.global.and.u.i32(ptr addrspace(1) %p2, i32 %nbit, i32 0, i32 2, i32 0, i1 true)
  %r3 = call i32 @air.atomic.global.or.u.i32(ptr addrspace(1) %p3, i32 %one, i32 0, i32 2, i32 0, i1 true)
  %r4 = call i32 @air.atomic.global.xor.u.i32(ptr addrspace(1) %p4, i32 %t1, i32 0, i32 2, i32 0, i1 true)
  %r5 = call i32 @air.atomic.global.min.u.i32(ptr addrspace(1) %p5, i32 %tid7, i32 0, i32 2, i32 0, i1 true)
  %r6 = call i32 @air.atomic.global.max.u.i32(ptr addrspace(1) %p6, i32 %tid7, i32 0, i32 2, i32 0, i1 true)
  %r7 = call i32 @air.atomic.global.min.s.i32(ptr addrspace(1) %p7, i32 %tidm100, i32 0, i32 2, i32 0, i1 true)
  %r8 = call i32 @air.atomic.global.max.s.i32(ptr addrspace(1) %p8, i32 %tidm100, i32 0, i32 2, i32 0, i1 true)
  ; exchange: the old value goes to o[tid]
  %r9 = call i32 @air.atomic.global.xchg.i32(ptr addrspace(1) %p9, i32 %t1, i32 0, i32 2, i32 0, i1 true)
  %o_x = getelementptr i32, ptr addrspace(1) %o, i64 %t64
  store i32 %r9, ptr addrspace(1) %o_x
  ; add.s of a negative value: c[10] += tid - 100 (signed, wraps as two's complement)
  %r10 = call i32 @air.atomic.global.add.s.i32(ptr addrspace(1) %p10, i32 %tidm100, i32 0, i32 2, i32 0, i1 true)
  ; atomic store of a constant, then every thread loads c[11] back into o[256 + tid]
  call void @air.atomic.global.store.i32(ptr addrspace(1) %p11, i32 4660, i32 0, i32 2, i1 true)
  %r11 = call i32 @air.atomic.global.load.i32(ptr addrspace(1) %p11, i32 0, i32 2, i1 true)
  %t64b = add i64 %t64, 256
  %o_l = getelementptr i32, ptr addrspace(1) %o, i64 %t64b
  store i32 %r11, ptr addrspace(1) %o_l
  ; ticket: c[14] += 1 returns a distinct old value per thread -> o[512 + tid]
  %r14 = call i32 @air.atomic.global.add.u.i32(ptr addrspace(1) %p14, i32 1, i32 0, i32 2, i32 0, i1 true)
  %t64c = add i64 %t64, 512
  %o_t = getelementptr i32, ptr addrspace(1) %o, i64 %t64c
  store i32 %r14, ptr addrspace(1) %o_t
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
!20 = !{ptr @atomic_rmw, !3, !19}
!10 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"atomic_uint", !"air.arg_name", !"c"}
!11 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"o"}
!12 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
!19 = !{!10, !11, !12}
