target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx15.6.0"
declare void @air.wg.barrier(i32, i32)
define void @tg_two(ptr addrspace(1) %o, ptr addrspace(3) %a, ptr addrspace(3) %b, i32 %lid, i32 %gid) {
entry:
  %l64 = zext i32 %lid to i64
  %g64 = zext i32 %gid to i64
  %pa = getelementptr i32, ptr addrspace(3) %a, i64 %l64
  %pb = getelementptr i32, ptr addrspace(3) %b, i64 %l64
  %va = add i32 %lid, 1
  %vb = mul i32 %lid, 10
  store i32 %va, ptr addrspace(3) %pa
  store i32 %vb, ptr addrspace(3) %pb
  call void @air.wg.barrier(i32 2, i32 1)
  %n1 = add i32 %lid, 1
  %m1 = and i32 %n1, 63
  %m164 = zext i32 %m1 to i64
  %n2 = add i32 %lid, 2
  %m2 = and i32 %n2, 63
  %m264 = zext i32 %m2 to i64
  %ra = getelementptr i32, ptr addrspace(3) %a, i64 %m164
  %rb = getelementptr i32, ptr addrspace(3) %b, i64 %m264
  %xa = load i32, ptr addrspace(3) %ra
  %xb = load i32, ptr addrspace(3) %rb
  %s = add i32 %xa, %xb
  %gb32 = mul i32 %gid, 64
  %oi32 = add i32 %gb32, %lid
  %oi = zext i32 %oi32 to i64
  %op = getelementptr i32, ptr addrspace(1) %o, i64 %oi
  store i32 %s, ptr addrspace(1) %op
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
!20 = !{ptr @tg_two, !3, !19}
!10 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"o"}
!11 = !{i32 1, !"air.threadgroup_buffer", !"air.location_index", i32 5, i32 1, !"air.address_space", i32 3, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"a"}
!14 = !{i32 2, !"air.threadgroup_buffer", !"air.location_index", i32 2, i32 1, !"air.address_space", i32 3, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"b"}
!12 = !{i32 3, !"air.thread_index_in_threadgroup", !"air.arg_type_name", !"uint", !"air.arg_name", !"lid"}
!13 = !{i32 4, !"air.threadgroup_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"gid"}
!19 = !{!10, !11, !14, !12, !13}
