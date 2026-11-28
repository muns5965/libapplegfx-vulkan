target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-macosx15.6.0"
define void @i64ops(ptr addrspace(1) %o, i32 %lid, i32 %gid) {
entry:
  %l64 = zext i32 %lid to i64
  %g64 = zext i32 %gid to i64
  %gb = mul i64 %g64, 64
  %i = add i64 %gb, %l64
  %a = mul i64 %i, -7046029254386353131
  %as = lshr i64 %a, 29
  %ax = xor i64 %a, %as
  %bm = mul i64 %ax, -4658895280553007687
  %b = add i64 %bm, 12345
  %bt = trunc i64 %b to i32
  %bn = and i32 %bt, 63
  %amt = zext i32 %bn to i64
  %r_add = add i64 %a, %b
  %r_sub = sub i64 %a, %b
  %r_mul = mul i64 %a, %b
  %r_shl = shl i64 %a, %amt
  %r_lshr = lshr i64 %a, %amt
  %r_ashr = ashr i64 %a, %amt
  %r_and = and i64 %a, %b
  %r_or = or i64 %a, %b
  %r_xor = xor i64 %a, %b
  %r_not = xor i64 %a, -1
  %r_neg = sub i64 0, %a
  %c_eq = icmp eq i64 %a, %b
  %r_eq = zext i1 %c_eq to i64
  %c_ne = icmp ne i64 %a, %b
  %r_ne = zext i1 %c_ne to i64
  %c_ult = icmp ult i64 %a, %b
  %r_ult = zext i1 %c_ult to i64
  %c_ule = icmp ule i64 %a, %b
  %r_ule = zext i1 %c_ule to i64
  %c_ugt = icmp ugt i64 %a, %b
  %r_ugt = zext i1 %c_ugt to i64
  %c_uge = icmp uge i64 %a, %b
  %r_uge = zext i1 %c_uge to i64
  %c_slt = icmp slt i64 %a, %b
  %r_slt = zext i1 %c_slt to i64
  %c_sle = icmp sle i64 %a, %b
  %r_sle = zext i1 %c_sle to i64
  %c_sgt = icmp sgt i64 %a, %b
  %r_sgt = zext i1 %c_sgt to i64
  %c_sge = icmp sge i64 %a, %b
  %r_sge = zext i1 %c_sge to i64
  %cs = icmp ult i64 %a, %b
  %r_sel = select i1 %cs, i64 %a, i64 %b
  %r_sext = sext i32 %bt to i64
  %at = trunc i64 %a to i32
  %r_zext = zext i32 %at to i64
  br label %loop
loop:
  %k = phi i32 [ 0, %entry ], [ %k1, %loop ]
  %acc = phi i64 [ %b, %entry ], [ %acc1, %loop ]
  %acc3 = mul i64 %acc, 3
  %acc1 = add i64 %acc3, %a
  %k1 = add i32 %k, 1
  %kc = icmp ult i32 %k1, 4
  br i1 %kc, label %loop, label %done
done:
  %ob = mul i64 %i, 25
  %oi0 = add i64 %ob, 0
  %p0 = getelementptr i64, ptr addrspace(1) %o, i64 %oi0
  store i64 %r_add, ptr addrspace(1) %p0
  %oi1 = add i64 %ob, 1
  %p1 = getelementptr i64, ptr addrspace(1) %o, i64 %oi1
  store i64 %r_sub, ptr addrspace(1) %p1
  %oi2 = add i64 %ob, 2
  %p2 = getelementptr i64, ptr addrspace(1) %o, i64 %oi2
  store i64 %r_mul, ptr addrspace(1) %p2
  %oi3 = add i64 %ob, 3
  %p3 = getelementptr i64, ptr addrspace(1) %o, i64 %oi3
  store i64 %r_shl, ptr addrspace(1) %p3
  %oi4 = add i64 %ob, 4
  %p4 = getelementptr i64, ptr addrspace(1) %o, i64 %oi4
  store i64 %r_lshr, ptr addrspace(1) %p4
  %oi5 = add i64 %ob, 5
  %p5 = getelementptr i64, ptr addrspace(1) %o, i64 %oi5
  store i64 %r_ashr, ptr addrspace(1) %p5
  %oi6 = add i64 %ob, 6
  %p6 = getelementptr i64, ptr addrspace(1) %o, i64 %oi6
  store i64 %r_and, ptr addrspace(1) %p6
  %oi7 = add i64 %ob, 7
  %p7 = getelementptr i64, ptr addrspace(1) %o, i64 %oi7
  store i64 %r_or, ptr addrspace(1) %p7
  %oi8 = add i64 %ob, 8
  %p8 = getelementptr i64, ptr addrspace(1) %o, i64 %oi8
  store i64 %r_xor, ptr addrspace(1) %p8
  %oi9 = add i64 %ob, 9
  %p9 = getelementptr i64, ptr addrspace(1) %o, i64 %oi9
  store i64 %r_not, ptr addrspace(1) %p9
  %oi10 = add i64 %ob, 10
  %p10 = getelementptr i64, ptr addrspace(1) %o, i64 %oi10
  store i64 %r_neg, ptr addrspace(1) %p10
  %oi11 = add i64 %ob, 11
  %p11 = getelementptr i64, ptr addrspace(1) %o, i64 %oi11
  store i64 %r_eq, ptr addrspace(1) %p11
  %oi12 = add i64 %ob, 12
  %p12 = getelementptr i64, ptr addrspace(1) %o, i64 %oi12
  store i64 %r_ne, ptr addrspace(1) %p12
  %oi13 = add i64 %ob, 13
  %p13 = getelementptr i64, ptr addrspace(1) %o, i64 %oi13
  store i64 %r_ult, ptr addrspace(1) %p13
  %oi14 = add i64 %ob, 14
  %p14 = getelementptr i64, ptr addrspace(1) %o, i64 %oi14
  store i64 %r_ule, ptr addrspace(1) %p14
  %oi15 = add i64 %ob, 15
  %p15 = getelementptr i64, ptr addrspace(1) %o, i64 %oi15
  store i64 %r_ugt, ptr addrspace(1) %p15
  %oi16 = add i64 %ob, 16
  %p16 = getelementptr i64, ptr addrspace(1) %o, i64 %oi16
  store i64 %r_uge, ptr addrspace(1) %p16
  %oi17 = add i64 %ob, 17
  %p17 = getelementptr i64, ptr addrspace(1) %o, i64 %oi17
  store i64 %r_slt, ptr addrspace(1) %p17
  %oi18 = add i64 %ob, 18
  %p18 = getelementptr i64, ptr addrspace(1) %o, i64 %oi18
  store i64 %r_sle, ptr addrspace(1) %p18
  %oi19 = add i64 %ob, 19
  %p19 = getelementptr i64, ptr addrspace(1) %o, i64 %oi19
  store i64 %r_sgt, ptr addrspace(1) %p19
  %oi20 = add i64 %ob, 20
  %p20 = getelementptr i64, ptr addrspace(1) %o, i64 %oi20
  store i64 %r_sge, ptr addrspace(1) %p20
  %oi21 = add i64 %ob, 21
  %p21 = getelementptr i64, ptr addrspace(1) %o, i64 %oi21
  store i64 %r_sel, ptr addrspace(1) %p21
  %oi22 = add i64 %ob, 22
  %p22 = getelementptr i64, ptr addrspace(1) %o, i64 %oi22
  store i64 %r_sext, ptr addrspace(1) %p22
  %oi23 = add i64 %ob, 23
  %p23 = getelementptr i64, ptr addrspace(1) %o, i64 %oi23
  store i64 %r_zext, ptr addrspace(1) %p23
  %oi24 = add i64 %ob, 24
  %p24 = getelementptr i64, ptr addrspace(1) %o, i64 %oi24
  store i64 %acc1, ptr addrspace(1) %p24
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
!20 = !{ptr @i64ops, !3, !19}
!10 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 8, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"ulong", !"air.arg_name", !"o"}
!12 = !{i32 1, !"air.thread_index_in_threadgroup", !"air.arg_type_name", !"uint", !"air.arg_name", !"lid"}
!13 = !{i32 2, !"air.threadgroup_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"gid"}
!19 = !{!10, !12, !13}
