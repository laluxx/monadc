; ModuleID = 'Ascii'
source_filename = "Ascii"

@fk = private unnamed_addr constant [6 x i8] c"LINUX\00", align 1
@fk.1 = private unnamed_addr constant [5 x i8] c"UNIX\00", align 1
@fk.2 = private unnamed_addr constant [7 x i8] c"X86-64\00", align 1
@fk.3 = private unnamed_addr constant [6 x i8] c"64BIT\00", align 1
@fk.4 = private unnamed_addr constant [14 x i8] c"LITTLE-ENDIAN\00", align 1
@fk.5 = private unnamed_addr constant [4 x i8] c"ELF\00", align 1
@fk.6 = private unnamed_addr constant [4 x i8] c"SSE\00", align 1
@fk.7 = private unnamed_addr constant [5 x i8] c"SSE2\00", align 1
@fk.8 = private unnamed_addr constant [6 x i8] c"DEBUG\00", align 1
@__features__ = internal global ptr null
@str = private unnamed_addr constant [27 x i8] c"abcdefghijklmnopqrstuvwxyz\00", align 1
@Ascii__lowercase = global ptr null
@str.9 = private unnamed_addr constant [27 x i8] c"ABCDEFGHIJKLMNOPQRSTUVWXYZ\00", align 1
@Ascii__uppercase = global ptr null
@str.10 = private unnamed_addr constant [53 x i8] c"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ\00", align 1
@Ascii__letters = global ptr null
@str.11 = private unnamed_addr constant [11 x i8] c"0123456789\00", align 1
@Ascii__digits = global ptr null
@str.12 = private unnamed_addr constant [23 x i8] c"0123456789abcdefABCDEF\00", align 1
@Ascii__hexdigits = global ptr null
@str.13 = private unnamed_addr constant [9 x i8] c"01234567\00", align 1
@Ascii__octdigits = global ptr null
@str.14 = private unnamed_addr constant [35 x i8] c"!\\\22#$%&'()*+,-./:;<=>?@[\\\\]^_`{|}~\00", align 1
@Ascii__punctuation = global ptr null
@str.15 = private unnamed_addr constant [16 x i8] c" \\t\\n\\r\\x0B\\x0C\00", align 1
@Ascii__whitespace = global ptr null
@str.16 = private unnamed_addr constant [112 x i8] c"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ!\\\22#$%&'()*+,-./:;<=>?@[\\\\]^_`{|}~ \\t\\n\\r\\x0B\\x0C\00", align 1
@Ascii__printable = global ptr null
@Ascii__nul = global i64 0
@Ascii__bel = global i64 0
@Ascii__bs = global i64 0
@Ascii__tab = global i64 0
@Ascii__lf = global i64 0
@Ascii__cr = global i64 0
@Ascii__esc = global i64 0
@Ascii__space = global i64 0
@Ascii__del = global i64 0
@Ascii__case-delta = global i64 0

declare ptr @rt_list_create()

declare ptr @rt_list_cons(ptr, ptr)

declare void @rt_list_append(ptr, ptr)

declare ptr @rt_list_car(ptr)

declare ptr @rt_list_cdr(ptr)

declare ptr @rt_list_nth(ptr, i64)

declare i64 @rt_list_length(ptr)

declare i32 @rt_list_is_empty(ptr)

declare ptr @rt_value_int(i64)

declare ptr @rt_value_float(double)

declare ptr @rt_value_char(i8)

declare ptr @rt_value_string(ptr)

declare ptr @rt_value_symbol(ptr)

declare ptr @rt_value_keyword(ptr)

declare ptr @rt_value_list(ptr)

declare ptr @rt_value_nil()

declare void @rt_print_value(ptr)

declare void @rt_print_list(ptr)

declare ptr @rt_value_ratio(i64, i64)

declare ptr @rt_ratio_add(ptr, ptr)

declare ptr @rt_ratio_sub(ptr, ptr)

declare ptr @rt_ratio_mul(ptr, ptr)

declare ptr @rt_ratio_div(ptr, ptr)

declare i64 @rt_ratio_to_int(ptr)

declare double @rt_ratio_to_float(ptr)

define void @__init_Ascii() {
entry:
  %feats = call ptr @rt_list_create()
  %kv = call ptr @rt_value_keyword(ptr @fk)
  call void @rt_list_append(ptr %feats, ptr %kv)
  %kv1 = call ptr @rt_value_keyword(ptr @fk.1)
  call void @rt_list_append(ptr %feats, ptr %kv1)
  %kv2 = call ptr @rt_value_keyword(ptr @fk.2)
  call void @rt_list_append(ptr %feats, ptr %kv2)
  %kv3 = call ptr @rt_value_keyword(ptr @fk.3)
  call void @rt_list_append(ptr %feats, ptr %kv3)
  %kv4 = call ptr @rt_value_keyword(ptr @fk.4)
  call void @rt_list_append(ptr %feats, ptr %kv4)
  %kv5 = call ptr @rt_value_keyword(ptr @fk.5)
  call void @rt_list_append(ptr %feats, ptr %kv5)
  %kv6 = call ptr @rt_value_keyword(ptr @fk.6)
  call void @rt_list_append(ptr %feats, ptr %kv6)
  %kv7 = call ptr @rt_value_keyword(ptr @fk.7)
  call void @rt_list_append(ptr %feats, ptr %kv7)
  %kv8 = call ptr @rt_value_keyword(ptr @fk.8)
  call void @rt_list_append(ptr %feats, ptr %kv8)
  store ptr %feats, ptr @__features__, align 8
  %strdup = call ptr @strdup(ptr @str)
  store ptr %strdup, ptr @Ascii__lowercase, align 8
  %strdup9 = call ptr @strdup(ptr @str.9)
  store ptr %strdup9, ptr @Ascii__uppercase, align 8
  %strdup10 = call ptr @strdup(ptr @str.10)
  store ptr %strdup10, ptr @Ascii__letters, align 8
  %strdup11 = call ptr @strdup(ptr @str.11)
  store ptr %strdup11, ptr @Ascii__digits, align 8
  %strdup12 = call ptr @strdup(ptr @str.12)
  store ptr %strdup12, ptr @Ascii__hexdigits, align 8
  %strdup13 = call ptr @strdup(ptr @str.13)
  store ptr %strdup13, ptr @Ascii__octdigits, align 8
  %strdup14 = call ptr @strdup(ptr @str.14)
  store ptr %strdup14, ptr @Ascii__punctuation, align 8
  %strdup15 = call ptr @strdup(ptr @str.15)
  store ptr %strdup15, ptr @Ascii__whitespace, align 8
  %strdup16 = call ptr @strdup(ptr @str.16)
  store ptr %strdup16, ptr @Ascii__printable, align 8
  store i64 0, ptr @Ascii__nul, align 4
  store i64 7, ptr @Ascii__bel, align 4
  store i64 8, ptr @Ascii__bs, align 4
  store i64 9, ptr @Ascii__tab, align 4
  store i64 10, ptr @Ascii__lf, align 4
  store i64 13, ptr @Ascii__cr, align 4
  store i64 27, ptr @Ascii__esc, align 4
  store i64 32, ptr @Ascii__space, align 4
  store i64 127, ptr @Ascii__del, align 4
  store i64 32, ptr @Ascii__case-delta, align 4
  ret void
}

declare ptr @strdup(ptr)

define i8 @Ascii__char-upcase(i8 %c) {
entry:
  %c1 = alloca i8, align 1
  store i8 %c, ptr %c1, align 1
  %c2 = load i8, ptr %c1, align 1
  %cmptmp = icmp sge i8 %c2, 97
  %bool = zext i1 %cmptmp to i64
  %and0 = icmp ne i64 %bool, 0
  %c3 = load i8, ptr %c1, align 1
  %cmptmp4 = icmp sle i8 %c3, 122
  %bool5 = zext i1 %cmptmp4 to i64
  %andi = icmp ne i64 %bool5, 0
  %and = and i1 %and0, %andi
  %and_ext = zext i1 %and to i64
  %ifcond = icmp ne i64 %and_ext, 0
  br i1 %ifcond, label %then, label %else

then:                                             ; preds = %entry
  %c6 = load i8, ptr %c1, align 1
  %char_to_int = zext i8 %c6 to i64
  %case-delta = load i64, ptr @Ascii__case-delta, align 4
  %subtmp = sub i64 %char_to_int, %case-delta
  %to_char = trunc i64 %subtmp to i8
  br label %ifmerge

else:                                             ; preds = %entry
  %c7 = load i8, ptr %c1, align 1
  br label %ifmerge

ifmerge:                                          ; preds = %else, %then
  %iftmp = phi i8 [ %to_char, %then ], [ %c7, %else ]
  ret i8 %iftmp
}

define i8 @Ascii__char-downcase(i8 %c) {
entry:
  %c1 = alloca i8, align 1
  store i8 %c, ptr %c1, align 1
  %c2 = load i8, ptr %c1, align 1
  %cmptmp = icmp sge i8 %c2, 65
  %bool = zext i1 %cmptmp to i64
  %and0 = icmp ne i64 %bool, 0
  %c3 = load i8, ptr %c1, align 1
  %cmptmp4 = icmp sle i8 %c3, 90
  %bool5 = zext i1 %cmptmp4 to i64
  %andi = icmp ne i64 %bool5, 0
  %and = and i1 %and0, %andi
  %and_ext = zext i1 %and to i64
  %ifcond = icmp ne i64 %and_ext, 0
  br i1 %ifcond, label %then, label %else

then:                                             ; preds = %entry
  %c6 = load i8, ptr %c1, align 1
  %char_to_int = zext i8 %c6 to i64
  %case-delta = load i64, ptr @Ascii__case-delta, align 4
  %addtmp = add i64 %char_to_int, %case-delta
  %to_char = trunc i64 %addtmp to i8
  br label %ifmerge

else:                                             ; preds = %entry
  %c7 = load i8, ptr %c1, align 1
  br label %ifmerge

ifmerge:                                          ; preds = %else, %then
  %iftmp = phi i8 [ %to_char, %then ], [ %c7, %else ]
  ret i8 %iftmp
}

define ptr @Ascii__upcase(ptr %input) {
entry:
  %input1 = alloca ptr, align 8
  store ptr %input, ptr %input1, align 8
  %input2 = load ptr, ptr %input1, align 8
  %strlen = call i64 @strlen(ptr %input2)
  %len1 = add i64 %strlen, 1
  %buf = call ptr @calloc(i64 %len1, i64 1)
  %0 = call ptr @memset(ptr %buf, i32 97, i64 %strlen)
  %strdup = call ptr @strdup(ptr %buf)
  %result = alloca ptr, align 8
  store ptr %strdup, ptr %result, align 8
  %input3 = load ptr, ptr %input1, align 8
  %strlen4 = call i64 @strlen(ptr %input3)
  %i = alloca i64, align 8
  store i64 0, ptr %i, align 4
  br label %for_cond

for_cond:                                         ; preds = %for_body, %entry
  %i5 = load i64, ptr %i, align 4
  %for_cond6 = icmp slt i64 %i5, %strlen4
  br i1 %for_cond6, label %for_body, label %for_after

for_body:                                         ; preds = %for_cond
  %result7 = load ptr, ptr %result, align 8
  %i8 = load i64, ptr %i, align 4
  %input9 = load ptr, ptr %input1, align 8
  %i10 = load i64, ptr %i, align 4
  %char_ptr = getelementptr i8, ptr %input9, i64 %i10
  %char = load i8, ptr %char_ptr, align 1
  %calltmp = call i8 @Ascii__char-upcase(i8 %char)
  %char_ptr11 = getelementptr i8, ptr %result7, i64 %i8
  store i8 %calltmp, ptr %char_ptr11, align 1
  %i_cur = load i64, ptr %i, align 4
  %i_next = add i64 %i_cur, 1
  store i64 %i_next, ptr %i, align 4
  br label %for_cond

for_after:                                        ; preds = %for_cond
  %result12 = load ptr, ptr %result, align 8
  ret ptr %result12
}

declare ptr @calloc(i64, i64)

declare ptr @memset(ptr, i32, i64)

declare i64 @strlen(ptr)

define ptr @Ascii__downcase(ptr %input) {
entry:
  %input1 = alloca ptr, align 8
  store ptr %input, ptr %input1, align 8
  %input2 = load ptr, ptr %input1, align 8
  %strlen = call i64 @strlen(ptr %input2)
  %len1 = add i64 %strlen, 1
  %buf = call ptr @calloc(i64 %len1, i64 1)
  %0 = call ptr @memset(ptr %buf, i32 97, i64 %strlen)
  %strdup = call ptr @strdup(ptr %buf)
  %result = alloca ptr, align 8
  store ptr %strdup, ptr %result, align 8
  %input3 = load ptr, ptr %input1, align 8
  %strlen4 = call i64 @strlen(ptr %input3)
  %i = alloca i64, align 8
  store i64 0, ptr %i, align 4
  br label %for_cond

for_cond:                                         ; preds = %for_body, %entry
  %i5 = load i64, ptr %i, align 4
  %for_cond6 = icmp slt i64 %i5, %strlen4
  br i1 %for_cond6, label %for_body, label %for_after

for_body:                                         ; preds = %for_cond
  %result7 = load ptr, ptr %result, align 8
  %i8 = load i64, ptr %i, align 4
  %input9 = load ptr, ptr %input1, align 8
  %i10 = load i64, ptr %i, align 4
  %char_ptr = getelementptr i8, ptr %input9, i64 %i10
  %char = load i8, ptr %char_ptr, align 1
  %calltmp = call i8 @Ascii__char-downcase(i8 %char)
  %char_ptr11 = getelementptr i8, ptr %result7, i64 %i8
  store i8 %calltmp, ptr %char_ptr11, align 1
  %i_cur = load i64, ptr %i, align 4
  %i_next = add i64 %i_cur, 1
  store i64 %i_next, ptr %i, align 4
  br label %for_cond

for_after:                                        ; preds = %for_cond
  %result12 = load ptr, ptr %result, align 8
  ret ptr %result12
}

define ptr @Ascii__capitalize(ptr %input) {
entry:
  %input1 = alloca ptr, align 8
  store ptr %input, ptr %input1, align 8
  %input2 = load ptr, ptr %input1, align 8
  %strlen = call i64 @strlen(ptr %input2)
  %len1 = add i64 %strlen, 1
  %buf = call ptr @calloc(i64 %len1, i64 1)
  %0 = call ptr @memset(ptr %buf, i32 97, i64 %strlen)
  %strdup = call ptr @strdup(ptr %buf)
  %result = alloca ptr, align 8
  store ptr %strdup, ptr %result, align 8
  %input3 = load ptr, ptr %input1, align 8
  %strlen4 = call i64 @strlen(ptr %input3)
  %i = alloca i64, align 8
  store i64 0, ptr %i, align 4
  br label %for_cond

for_cond:                                         ; preds = %ifmerge, %entry
  %i5 = load i64, ptr %i, align 4
  %for_cond6 = icmp slt i64 %i5, %strlen4
  br i1 %for_cond6, label %for_body, label %for_after

for_body:                                         ; preds = %for_cond
  %result7 = load ptr, ptr %result, align 8
  %i8 = load i64, ptr %i, align 4
  %i9 = load i64, ptr %i, align 4
  %cmptmp = icmp eq i64 %i9, 0
  %bool = zext i1 %cmptmp to i64
  %ifcond = icmp ne i64 %bool, 0
  br i1 %ifcond, label %then, label %else

for_after:                                        ; preds = %for_cond
  %result18 = load ptr, ptr %result, align 8
  ret ptr %result18

then:                                             ; preds = %for_body
  %input10 = load ptr, ptr %input1, align 8
  %i11 = load i64, ptr %i, align 4
  %char_ptr = getelementptr i8, ptr %input10, i64 %i11
  %char = load i8, ptr %char_ptr, align 1
  %calltmp = call i8 @Ascii__char-upcase(i8 %char)
  br label %ifmerge

else:                                             ; preds = %for_body
  %input12 = load ptr, ptr %input1, align 8
  %i13 = load i64, ptr %i, align 4
  %char_ptr14 = getelementptr i8, ptr %input12, i64 %i13
  %char15 = load i8, ptr %char_ptr14, align 1
  %calltmp16 = call i8 @Ascii__char-downcase(i8 %char15)
  br label %ifmerge

ifmerge:                                          ; preds = %else, %then
  %iftmp = phi i8 [ %calltmp, %then ], [ %calltmp16, %else ]
  %char_ptr17 = getelementptr i8, ptr %result7, i64 %i8
  store i8 %iftmp, ptr %char_ptr17, align 1
  %i_cur = load i64, ptr %i, align 4
  %i_next = add i64 %i_cur, 1
  store i64 %i_next, ptr %i, align 4
  br label %for_cond
}

define i1 @"Ascii__char-lower?"(i8 %c) {
entry:
  %c1 = alloca i8, align 1
  store i8 %c, ptr %c1, align 1
  %c2 = load i8, ptr %c1, align 1
  %cmptmp = icmp sge i8 %c2, 97
  %bool = zext i1 %cmptmp to i64
  %and0 = icmp ne i64 %bool, 0
  %c3 = load i8, ptr %c1, align 1
  %cmptmp4 = icmp sle i8 %c3, 122
  %bool5 = zext i1 %cmptmp4 to i64
  %andi = icmp ne i64 %bool5, 0
  %and = and i1 %and0, %andi
  %and_ext = zext i1 %and to i64
  %to_bool = trunc i64 %and_ext to i1
  ret i1 %to_bool
}

define i1 @"Ascii__char-upper?"(i8 %c) {
entry:
  %c1 = alloca i8, align 1
  store i8 %c, ptr %c1, align 1
  %c2 = load i8, ptr %c1, align 1
  %cmptmp = icmp sge i8 %c2, 65
  %bool = zext i1 %cmptmp to i64
  %and0 = icmp ne i64 %bool, 0
  %c3 = load i8, ptr %c1, align 1
  %cmptmp4 = icmp sle i8 %c3, 90
  %bool5 = zext i1 %cmptmp4 to i64
  %andi = icmp ne i64 %bool5, 0
  %and = and i1 %and0, %andi
  %and_ext = zext i1 %and to i64
  %to_bool = trunc i64 %and_ext to i1
  ret i1 %to_bool
}

define i1 @"Ascii__char-alphabetic?"(i8 %c) {
entry:
  %c1 = alloca i8, align 1
  store i8 %c, ptr %c1, align 1
  %c2 = load i8, ptr %c1, align 1
  %calltmp = call i1 @"Ascii__char-lower?"(i8 %c2)
  %or0 = icmp ne i1 %calltmp, false
  %c3 = load i8, ptr %c1, align 1
  %calltmp4 = call i1 @"Ascii__char-upper?"(i8 %c3)
  %ori = icmp ne i1 %calltmp4, false
  %or = or i1 %or0, %ori
  %or_ext = zext i1 %or to i64
  %to_bool = trunc i64 %or_ext to i1
  ret i1 %to_bool
}

define i1 @"Ascii__char-numeric?"(i8 %c) {
entry:
  %c1 = alloca i8, align 1
  store i8 %c, ptr %c1, align 1
  %c2 = load i8, ptr %c1, align 1
  %cmptmp = icmp sge i8 %c2, 48
  %bool = zext i1 %cmptmp to i64
  %and0 = icmp ne i64 %bool, 0
  %c3 = load i8, ptr %c1, align 1
  %cmptmp4 = icmp sle i8 %c3, 57
  %bool5 = zext i1 %cmptmp4 to i64
  %andi = icmp ne i64 %bool5, 0
  %and = and i1 %and0, %andi
  %and_ext = zext i1 %and to i64
  %to_bool = trunc i64 %and_ext to i1
  ret i1 %to_bool
}

define i1 @"Ascii__char-alphanumeric?"(i8 %c) {
entry:
  %c1 = alloca i8, align 1
  store i8 %c, ptr %c1, align 1
  %c2 = load i8, ptr %c1, align 1
  %calltmp = call i1 @"Ascii__char-alphabetic?"(i8 %c2)
  %or0 = icmp ne i1 %calltmp, false
  %c3 = load i8, ptr %c1, align 1
  %calltmp4 = call i1 @"Ascii__char-numeric?"(i8 %c3)
  %ori = icmp ne i1 %calltmp4, false
  %or = or i1 %or0, %ori
  %or_ext = zext i1 %or to i64
  %to_bool = trunc i64 %or_ext to i1
  ret i1 %to_bool
}

define i1 @"Ascii__char-whitespace?"(i8 %c) {
entry:
  %c1 = alloca i8, align 1
  store i8 %c, ptr %c1, align 1
  %c2 = load i8, ptr %c1, align 1
  %cmptmp = icmp eq i8 %c2, 32
  %bool = zext i1 %cmptmp to i64
  %or0 = icmp ne i64 %bool, 0
  %c3 = load i8, ptr %c1, align 1
  %cmptmp4 = icmp eq i8 %c3, 9
  %bool5 = zext i1 %cmptmp4 to i64
  %ori = icmp ne i64 %bool5, 0
  %or = or i1 %or0, %ori
  %c6 = load i8, ptr %c1, align 1
  %cmptmp7 = icmp eq i8 %c6, 10
  %bool8 = zext i1 %cmptmp7 to i64
  %ori9 = icmp ne i64 %bool8, 0
  %or10 = or i1 %or, %ori9
  %c11 = load i8, ptr %c1, align 1
  %cmptmp12 = icmp eq i8 %c11, 13
  %bool13 = zext i1 %cmptmp12 to i64
  %ori14 = icmp ne i64 %bool13, 0
  %or15 = or i1 %or10, %ori14
  %or_ext = zext i1 %or15 to i64
  %to_bool = trunc i64 %or_ext to i1
  ret i1 %to_bool
}
