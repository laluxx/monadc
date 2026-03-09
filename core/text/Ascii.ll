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
@undef_msg = private unnamed_addr constant [71 x i8] c"./core/text/Ascii.mon:35:3: error: called undefined function 'upcase'\0A\00", align 1
@undef_msg.17 = private unnamed_addr constant [73 x i8] c"./core/text/Ascii.mon:39:3: error: called undefined function 'downcase'\0A\00", align 1
@undef_msg.18 = private unnamed_addr constant [75 x i8] c"./core/text/Ascii.mon:43:3: error: called undefined function 'capitalize'\0A\00", align 1

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
  store ptr @str, ptr @Ascii__lowercase, align 8
  store ptr @str.9, ptr @Ascii__uppercase, align 8
  store ptr @str.10, ptr @Ascii__letters, align 8
  store ptr @str.11, ptr @Ascii__digits, align 8
  store ptr @str.12, ptr @Ascii__hexdigits, align 8
  store ptr @str.13, ptr @Ascii__octdigits, align 8
  store ptr @str.14, ptr @Ascii__punctuation, align 8
  store ptr @str.15, ptr @Ascii__whitespace, align 8
  store ptr @str.16, ptr @Ascii__printable, align 8
  ret void
}

define ptr @Ascii__upcase(ptr %input) {
entry:
  %input1 = alloca ptr, align 8
  store ptr %input, ptr %input1, align 8
  %0 = call i32 (ptr, ...) @printf(ptr @undef_msg)
  call void @abort()
  unreachable

undef_dead:                                       ; No predecessors!
  ret ptr undef
}

declare i32 @fprintf(ptr, ptr, ...)

declare i32 @printf(ptr, ...)

declare void @abort()

define ptr @Ascii__downcase(ptr %input) {
entry:
  %input1 = alloca ptr, align 8
  store ptr %input, ptr %input1, align 8
  %0 = call i32 (ptr, ...) @printf(ptr @undef_msg.17)
  call void @abort()
  unreachable

undef_dead:                                       ; No predecessors!
  ret ptr undef
}

define ptr @Ascii__capitalize(ptr %input) {
entry:
  %input1 = alloca ptr, align 8
  store ptr %input, ptr %input1, align 8
  %0 = call i32 (ptr, ...) @printf(ptr @undef_msg.18)
  call void @abort()
  unreachable

undef_dead:                                       ; No predecessors!
  ret ptr undef
}
