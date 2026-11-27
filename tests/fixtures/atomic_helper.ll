target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx15.6.0"
declare i32 @air.atomic.global.add.u.i32(ptr addrspace(1), i32, i32, i32, i32, i1)
define void @atomic_helper(ptr addrspace(1) %c, ptr addrspace(1) %o, i32 %tid) {
entry:
  %t1 = add i32 %tid, 1
  %t64 = zext i32 %tid to i64
  %p0 = getelementptr i32, ptr addrspace(1) %c, i64 0
  %p1 = getelementptr i32, ptr addrspace(1) %c, i64 1
  %p2 = getelementptr i32, ptr addrspace(1) %c, i64 2
  %ra = call i32 @bump(ptr addrspace(1) %p0, i32 1)
  %rb = call i32 @bump(ptr addrspace(1) %p1, i32 %t1)
  %rn = call i32 @bump_twice(ptr addrspace(1) %p0, i32 2)
  %rc = call i32 @bump(ptr addrspace(1) %p2, i32 1)
  %op = getelementptr i32, ptr addrspace(1) %o, i64 %t64
  store i32 %rc, ptr addrspace(1) %op
  ret void
}
define internal i32 @bump(ptr addrspace(1) %p, i32 %v) {
entry:
  %r = call i32 @air.atomic.global.add.u.i32(ptr addrspace(1) %p, i32 %v, i32 0, i32 2, i32 0, i1 true)
  ret i32 %r
}
define internal i32 @bump_twice(ptr addrspace(1) %p, i32 %v) {
entry:
  %a = call i32 @bump(ptr addrspace(1) %p, i32 %v)
  %b = call i32 @bump(ptr addrspace(1) %p, i32 %v)
  %s = add i32 %a, %b
  ret i32 %s
}
!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 7, i32 0}
!2 = !{!"Metal", i32 3, i32 2, i32 0}
!3 = !{}
!air.kernel = !{!20}
!20 = !{ptr @atomic_helper, !3, !19}
!10 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"atomic_uint", !"air.arg_name", !"c"}
!11 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"o"}
!12 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
!19 = !{!10, !11, !12}
