target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx15.6.0"
@tg64 = internal unnamed_addr addrspace(3) global [1 x i64] undef, align 8
declare void @air.wg.barrier(i32, i32)
declare void @air.atomic.local.store.i64(ptr addrspace(3), i64, i32, i32, i1)
declare void @air.atomic.local.max.u.i64(ptr addrspace(3), i64, i32, i32, i32, i1)
define void @atomic_i64_tg(ptr addrspace(1) %c, i32 %tid) {
entry:
  %w = getelementptr inbounds [1 x i64], ptr addrspace(3) @tg64, i64 0, i64 0
  call void @air.atomic.local.store.i64(ptr addrspace(3) %w, i64 0, i32 0, i32 2, i1 true)
  call void @air.wg.barrier(i32 2, i32 1)
  %t = zext i32 %tid to i64
  %m = mul i64 %t, 4294967297
  call void @air.atomic.local.max.u.i64(ptr addrspace(3) %w, i64 %m, i32 0, i32 2, i32 0, i1 true)
  call void @air.wg.barrier(i32 2, i32 1)
  %v = load i64, ptr addrspace(3) %w, align 8
  %p0 = getelementptr i64, ptr addrspace(1) %c, i64 0
  store i64 %v, ptr addrspace(1) %p0
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
!20 = !{ptr @atomic_i64_tg, !3, !19}
!10 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"atomic_uint", !"air.arg_name", !"c"}
!12 = !{i32 1, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
!19 = !{!10, !12}
