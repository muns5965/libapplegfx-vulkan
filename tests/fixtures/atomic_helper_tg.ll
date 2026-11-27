target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx15.6.0"
@tgh = internal unnamed_addr addrspace(3) global [1 x i32] undef, align 4
declare void @air.wg.barrier(i32, i32)
declare void @air.atomic.local.store.i32(ptr addrspace(3), i32, i32, i32, i1)
declare i32 @air.atomic.local.add.u.i32(ptr addrspace(3), i32, i32, i32, i32, i1)
define void @atomic_helper_tg(ptr addrspace(1) %o, i32 %lid, i32 %gid) {
entry:
  %w = getelementptr inbounds [1 x i32], ptr addrspace(3) @tgh, i64 0, i64 0
  call void @air.atomic.local.store.i32(ptr addrspace(3) %w, i32 0, i32 0, i32 2, i1 true)
  call void @air.wg.barrier(i32 2, i32 1)
  %l1 = add i32 %lid, 1
  %ra = call i32 @tg_add(i32 1)
  %rb = call i32 @tg_add(i32 %l1)
  %rn = call i32 @tg_twice(i32 2)
  call void @air.wg.barrier(i32 2, i32 1)
  %v = load i32, ptr addrspace(3) %w, align 4
  %g64 = zext i32 %gid to i64
  %op = getelementptr i32, ptr addrspace(1) %o, i64 %g64
  store i32 %v, ptr addrspace(1) %op
  ret void
}
define internal i32 @tg_add(i32 %v) {
entry:
  %w = getelementptr inbounds [1 x i32], ptr addrspace(3) @tgh, i64 0, i64 0
  %r = call i32 @air.atomic.local.add.u.i32(ptr addrspace(3) %w, i32 %v, i32 0, i32 2, i32 0, i1 true)
  ret i32 %r
}
define internal i32 @tg_twice(i32 %v) {
entry:
  %a = call i32 @tg_add(i32 %v)
  %b = call i32 @tg_add(i32 %v)
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
!20 = !{ptr @atomic_helper_tg, !3, !19}
!10 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"o"}
!12 = !{i32 1, !"air.thread_index_in_threadgroup", !"air.arg_type_name", !"uint", !"air.arg_name", !"lid"}
!13 = !{i32 2, !"air.threadgroup_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"gid"}
!19 = !{!10, !12, !13}
