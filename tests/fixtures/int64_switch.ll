target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx15.6.0"
define void @i64sw(ptr addrspace(1) %o, i32 %lid, i32 %gid) {
entry:
  %l64 = zext i32 %lid to i64
  %g64 = zext i32 %gid to i64
  %gb = mul i64 %g64, 64
  %i = add i64 %gb, %l64
  %v = and i64 %i, 7
  %vh = shl i64 %v, 32
  %x0 = or i64 %v, %vh
  %c5 = icmp eq i64 %v, 5
  %x1 = select i1 %c5, i64 -9188470239253757806, i64 %x0
  %c6 = icmp eq i64 %v, 6
  %x = select i1 %c6, i64 4294967296, i64 %x1
  switch i64 %x, label %dflt [
    i64 0, label %ca
    i64 4294967297, label %cb
    i64 12884901891, label %cc
    i64 -9188470239253757806, label %cd
    i64 4294967296, label %ce
  ]
ca:
  br label %join
cb:
  br label %join
cc:
  br label %join
cd:
  br label %join
ce:
  br label %join
dflt:
  br label %join
join:
  %r = phi i64 [ 1000, %ca ], [ 2000000000000, %cb ], [ 3000, %cc ], [ 4000, %cd ], [ 5000, %ce ], [ 7, %dflt ]
  %p = getelementptr i64, ptr addrspace(1) %o, i64 %i
  store i64 %r, ptr addrspace(1) %p
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
!20 = !{ptr @i64sw, !3, !19}
!10 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 8, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"ulong", !"air.arg_name", !"o"}
!12 = !{i32 1, !"air.thread_index_in_threadgroup", !"air.arg_type_name", !"uint", !"air.arg_name", !"lid"}
!13 = !{i32 2, !"air.threadgroup_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"gid"}
!19 = !{!10, !12, !13}
