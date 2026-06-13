#!/usr/bin/env python3

from pathlib import Path


CONTEXT = (
    "monadc.context.reader.sugar.inline-assembly, "
    "monadc.context.language.inline-asm, "
    "monadc.context.language.codegen"
)


def case(name, purpose, atom, signature, params, asm, call, stdout, fn=None):
    return {
        "name": name,
        "purpose": purpose,
        "atom": atom,
        "signature": signature,
        "params": params,
        "asm": asm,
        "call": call,
        "stdout": stdout,
        "fn": fn,
    }


CASES = [
    case("syscall-write-string", "Syscall-shaped asm writes through Linux write(2).", "A String buffer argument lowers as an asm operand usable by syscall.", "Int -> String -> Int -> Int", "fd buf len", "mov 1 %rax mov fd %rdi mov buf %rsi mov len %rdx syscall ret", '(asm-sys-write 1 "ok" 2)', "ok2\n"),
    case("sys-write-u8-pointer", "Inline asm functions accept pointer-sugar parameter types.", "A function body using the user-facing sys-write shape with *U8 must compile.", "Int -> *U8 -> Int -> Int", "fd buf len", "mov 1 %rax mov fd %rdi mov buf %rsi mov len %rdx syscall ret", "42", "42\n", fn="asm-sys-write-u8"),
    case("mov-param-to-rax", "Parameter placeholder identity through mov.", "mov from the first parameter into the return register preserves the value.", "Int -> Int", "x", "mov x %rax ret", "(asm-id 42)", "42\n"),
    case("int-add-two-params", "Two-operand add uses destination/source rewriting.", "add a b returns a + b through the tied output/input constraint.", "Int -> Int -> Int", "a b", "add a b", "(asm-add 20 22)", "42\n"),
    case("int-add-immediate", "Immediate add uses escaped LLVM inline-asm immediates.", "add x 5 emits an immediate add and returns x + 5.", "Int -> Int", "x", "add x 5", "(asm-add-imm 37)", "42\n"),
    case("int-sub-two-params", "Subtraction keeps the first operand as the mutable result.", "sub a b returns a - b.", "Int -> Int -> Int", "a b", "sub a b", "(asm-sub 50 8)", "42\n"),
    case("int-add-three-params", "Multiple asm instructions are split by mnemonic recognition.", "add a b followed by add a c returns a + b + c.", "Int -> Int -> Int -> Int", "a b c", "add a b add a c", "(asm-add3 20 10 12)", "42\n"),
    case("int-inc", "Unary inc mutates the tied output register.", "inc x returns x + 1.", "Int -> Int", "x", "inc x", "(asm-inc 41)", "42\n"),
    case("int-dec", "Unary dec mutates the tied output register.", "dec x returns x - 1.", "Int -> Int", "x", "dec x", "(asm-dec 43)", "42\n"),
    case("int-neg", "Unary neg supports signed integer returns.", "neg x returns -x.", "Int -> Int", "x", "neg x", "(asm-neg -42)", "42\n"),
    case("int-bitnot", "Bitwise not uses inferred q suffix on Int parameters.", "not x returns bitwise complement.", "Int -> Int", "x", "not x", "(asm-not -43)", "42\n"),
    case("int-shift-left-immediate", "Left shift with an immediate keeps AT&T source-first order.", "shl 1 x returns x shifted left by one.", "Int -> Int", "x", "shl 1 x", "(asm-shl1 21)", "42\n"),
    case("int-imul-two-params", "Two-operand imul uses destination/source rewriting.", "imul a b returns a * b.", "Int -> Int -> Int", "a b", "imul a b", "(asm-imul 6 7)", "42\n"),
    case("int-and-two-params", "Bitwise and combines two integer operands.", "and a b returns a & b.", "Int -> Int -> Int", "a b", "and a b", "(asm-and 47 42)", "42\n"),
    case("sse-float-movsd", "Float return constraints use SSE registers.", "movsd x %xmm0 preserves a Float argument.", "Float -> Float", "x", "movsd x %xmm0 ret", "(asm-float-id 1.5)", "1.5\n"),
    case("wisp-asm-add", "Integer addition remains valid in Wisp-style asm functions.", "add a b returns a + b in a fixture replacing the old top-level for-loop asm snippet.", "Int -> Int -> Int", "a b", "add a b", "(asm-for-loop 19 23)", "42\n"),
    case("char-return-from-param", "Char return type selects byte-sized LLVM return type.", "mov x %al returns a Char from an integer byte.", "Int -> Char", "x", "mov x %al ret", "(asm-char 65)", "A\n"),
    case("int-or-two-params", "Bitwise or combines two integer operands.", "or a b returns a | b.", "Int -> Int -> Int", "a b", "or a b", "(asm-or 40 2)", "42\n"),
    case("int-xor-two-params", "Bitwise xor combines two integer operands.", "xor a b returns a ^ b.", "Int -> Int -> Int", "a b", "xor a b", "(asm-xor 40 2)", "42\n"),
    case("int-sub-immediate", "Immediate sub uses escaped LLVM inline-asm immediates.", "sub x 8 returns x - 8.", "Int -> Int", "x", "sub x 8", "(asm-sub-imm 50)", "42\n"),
    case("int-imul-immediate", "Immediate imul uses escaped LLVM inline-asm immediates.", "imul x 7 returns x * 7.", "Int -> Int", "x", "imul x 7", "(asm-imul-imm 6)", "42\n"),
    case("int-and-immediate", "Immediate and uses escaped LLVM inline-asm immediates.", "and x 63 returns x & 63.", "Int -> Int", "x", "and x 63", "(asm-and-imm 106)", "42\n"),
    case("int-or-immediate", "Immediate or uses escaped LLVM inline-asm immediates.", "or x 2 returns x | 2.", "Int -> Int", "x", "or x 2", "(asm-or-imm 40)", "42\n"),
    case("int-xor-immediate", "Immediate xor uses escaped LLVM inline-asm immediates.", "xor x 8 returns x ^ 8.", "Int -> Int", "x", "xor x 8", "(asm-xor-imm 34)", "42\n"),
    case("int-shift-right-immediate", "Right shift with an immediate keeps AT&T source-first order.", "shr 1 x returns x shifted right by one.", "Int -> Int", "x", "shr 1 x", "(asm-shr1 84)", "42\n"),
    case("int-arithmetic-shift-right", "Arithmetic shift with an immediate handles signed negatives.", "sar 1 x returns x arithmetic-shifted right by one.", "Int -> Int", "x", "sar 1 x", "(asm-sar1 84)", "42\n"),
    case("six-param-forward-sum", "Five input operands remain addressable through LLVM placeholders.", "a plus b/c/d/e/f returns the sum of six Int parameters.", "Int -> Int -> Int -> Int -> Int -> Int -> Int", "a b c d e f", "add a b add a c add a d add a e add a f", "(asm-six 1 2 3 4 5 6)", "21\n"),
    case("int-inc-twice", "Repeated unary operations stay in the tied output register.", "inc x inc x returns x + 2.", "Int -> Int", "x", "inc x inc x", "(asm-inc2 40)", "42\n"),
    case("int-dec-twice", "Repeated decrement operations stay in the tied output register.", "dec x dec x returns x - 2.", "Int -> Int", "x", "dec x dec x", "(asm-dec2 44)", "42\n"),
    case("int-add-then-sub", "A mixed add/sub instruction stream preserves order.", "add a b then sub a c returns a + b - c.", "Int -> Int -> Int -> Int", "a b c", "add a b sub a c", "(asm-add-sub 50 10 18)", "42\n"),
    case("int-sub-then-add", "A mixed sub/add instruction stream preserves order.", "sub a b then add a c returns a - b + c.", "Int -> Int -> Int -> Int", "a b c", "sub a b add a c", "(asm-sub-add 50 20 12)", "42\n"),
    case("int-and-then-or", "Logical instruction stream preserves order.", "and a b then or a c returns (a & b) | c.", "Int -> Int -> Int -> Int", "a b c", "and a b or a c", "(asm-and-or 47 40 2)", "42\n"),
    case("int-xor-three-operands", "Xor instruction stream preserves order.", "xor a b then xor a c returns a ^ b ^ c.", "Int -> Int -> Int -> Int", "a b c", "xor a b xor a c", "(asm-xor3 40 1 3)", "42\n"),
    case("int-neg-then-add", "Unary neg followed by add works on the same output.", "neg x then add x y returns -x + y.", "Int -> Int -> Int", "x y", "neg x add x y", "(asm-neg-add 8 50)", "42\n"),
    case("mov-immediate-return", "Immediate mov can seed a return value independent of input.", "mov 42 %rax ret returns 42.", "Int -> Int", "x", "mov 42 %rax ret", "(asm-const 0)", "42\n"),
    case("int-imul-then-add-immediate", "Immediate add after multiplication preserves instruction order.", "imul a b then add a 2 returns a*b + 2.", "Int -> Int -> Int", "a b", "imul a b add a 2", "(asm-mul-add 8 5)", "42\n"),
    case("int-imul-then-sub-immediate", "Immediate subtract after multiplication preserves instruction order.", "imul a b then sub a 6 returns a*b - 6.", "Int -> Int -> Int", "a b", "imul a b sub a 6", "(asm-mul-sub 8 6)", "42\n"),
    case("int-two-immediate-adds", "Two immediate adds can be chained.", "add x 20 then add x 2 returns x + 22.", "Int -> Int", "x", "add x 20 add x 2", "(asm-add-imm2 20)", "42\n"),
    case("sse-float-empty-identity", "Empty Float asm still exercises =x,0 constraints.", "An empty asm body returns the tied Float input.", "Float -> Float", "x", "", "(asm-float-empty 2.5)", "2.5\n"),
    case("syscall-write-string-bang", "String operand syscall test uses real side effects safely.", "write(2) returns the requested byte count after writing to stdout.", "Int -> String -> Int -> Int", "fd buf len", "mov 1 %rax mov fd %rdi mov buf %rsi mov len %rdx syscall ret", '(asm-write-bang 1 "hi" 2)', "hi2\n"),
    case("mov-then-add", "Parameter mov can be followed by integer arithmetic.", "mov x %rax ret-style setup plus add keeps the return value meaningful.", "Int -> Int", "x", "mov x %rax add x 2", "(asm-mov-add 40)", "42\n"),
    case("nop-before-add", "A no-op instruction can precede a real mutation.", "nop add a b returns a + b.", "Int -> Int -> Int", "a b", "nop add a b", "(asm-nop-add 20 22)", "42\n"),
    case("nop-after-add", "A no-op instruction can follow a real mutation.", "add a b nop returns a + b.", "Int -> Int -> Int", "a b", "add a b nop", "(asm-add-nop 20 22)", "42\n"),
    case("int-large-immediate-add", "Size suffix inference handles 64-bit immediates.", "add x 1000000000 returns x plus a large immediate.", "Int -> Int", "x", "add x 1000000000", "(asm-large-imm -999999958)", "42\n"),
    case("three-placeholder-sum", "Three parameter placeholders are split correctly.", "a + b + c with three params returns 42.", "Int -> Int -> Int -> Int", "a b c", "add a b add a c", "(asm-three-placeholders 10 20 12)", "42\n"),
    case("int-sub-negative-operands", "Subtraction from negative values returns signed Int.", "sub a b handles signed first operands.", "Int -> Int -> Int", "a b", "sub a b", "(asm-sub-negative -40 -82)", "42\n"),
    case("nop-nop-inc", "Increment works after a no-op stream.", "nop nop inc x returns x + 1.", "Int -> Int", "x", "nop nop inc x", "(asm-nop-inc 41)", "42\n"),
    case("nop-dec", "Decrement works after a no-op stream.", "nop dec x returns x - 1.", "Int -> Int", "x", "nop dec x", "(asm-nop-dec 43)", "42\n"),
    case("char-return-immediate", "Immediate mov can return Char values.", "mov 65 %al ret returns character A.", "Int -> Char", "x", "mov 65 %al ret", "(asm-char-const 0)", "A\n"),
    case("sse-float-explicit-movsd", "Float identity with explicit movsd covers SSE operand formatting.", "movsd x %xmm0 ret returns the original float.", "Float -> Float", "x", "movsd x %xmm0 ret", "(asm-float-mov 3.25)", "3.25\n"),
    case("six-param-reverse-sum", "Six integer parameters cover every SysV register allocator slot.", "The first six Int parameters remain callable and addable.", "Int -> Int -> Int -> Int -> Int -> Int -> Int", "a b c d e f", "add a f add a e add a d add a c add a b", "(asm-six-reverse 1 6 5 4 3 23)", "42\n"),
    case("cmov-max", "Conditional move selects a maximum without language-level branching.", "cmp plus cmovg returns b when b is greater than a.", "Int -> Int -> Int", "a b", "cmp a b cmovg b a", "(asm-cmov-max 20 42)", "42\n"),
    case("cmov-min", "Conditional move selects a minimum without language-level branching.", "cmp plus cmovl returns b when b is less than a.", "Int -> Int -> Int", "a b", "cmp a b cmovl b a", "(asm-cmov-min 50 42)", "42\n"),
    case("idiv-quotient", "Signed division uses explicit rax/rdx setup around a placeholder divisor.", "mov into rax, cqo, and idiv by a parameter returns the quotient.", "Int -> Int -> Int", "a b", "mov a %rax cqo idiv b ret", "(asm-idiv 84 2)", "42\n"),
    case("xchg-two-params", "xchg mutates the tied output register.", "xchg a b returns the second argument through the first tied result register.", "Int -> Int -> Int", "a b", "xchg a b", "(asm-xchg 7 42)", "42\n"),
    case("rol-one-bit", "Rotate-left accepts an immediate count and tied Int output.", "rol 1 x doubles 21 by rotating within a 64-bit register.", "Int -> Int", "x", "rol 1 x", "(asm-rol1 21)", "42\n"),
    case("ror-one-bit", "Rotate-right accepts an immediate count and tied Int output.", "ror 1 x halves 84 by rotating within a 64-bit register.", "Int -> Int", "x", "ror 1 x", "(asm-ror1 84)", "42\n"),
    case("sse-float-add", "SSE scalar addition combines language Float arguments.", "addsd b a returns a + b through xmm tied output constraints.", "Float -> Float -> Float", "a b", "addsd b a", "(asm-float-add 1.5 2.25)", "3.75\n"),
    case("sse-float-mul", "SSE scalar multiplication combines language Float arguments.", "mulsd b a returns a * b through xmm tied output constraints.", "Float -> Float -> Float", "a b", "mulsd b a", "(asm-float-mul 2.5 4.0)", "10\n"),
    case("byte-width-zero-extend", "Writing eax in asm zero-extends to the Int return register.", "mov immediate %eax ret exposes x86-64 zero-extension behavior for 32-bit writes.", "Int -> Int", "x", "mov 4294967295 %eax ret", "(asm-eax-zero-extend 0)", "4294967295\n"),
]


def function_name(spec):
    if spec.get("fn"):
        return spec["fn"]
    call = spec["call"].strip()
    if call.startswith("("):
        return call[1:].split()[0].rstrip(")")
    return call.split()[0]


def write_case(root, spec):
    fn = function_name(spec)
    path = root / f"asm-{spec['name']}.mon"
    stdout_path = root / f"asm-{spec['name']}.stdout"
    content = [
        f";; TEST-ID: tests.codegen.asm.{spec['name']}",
        ";; TEST-SECTION: codegen",
        f";; TEST-CONTEXT: {CONTEXT}",
        f";; TEST-PURPOSE: {spec['purpose']}",
        f";; TEST-ATOM: {spec['atom']}",
        ";; TEST-EXPECT: compile, run",
        "",
        "module AsmTest",
        "",
        f"define {fn} :: {spec['signature']}",
        f"  {spec['params']} -> (asm {spec['asm']})",
        "",
        f"show {spec['call']}",
    ]
    path.write_text("\n".join(content).rstrip() + "\n", encoding="utf-8")
    stdout_path.write_text(spec["stdout"], encoding="utf-8")


def main():
    root = Path("tests/codegen/asm")
    root.mkdir(parents=True, exist_ok=True)
    for path in root.glob("asm-*.mon"):
        path.unlink()
    for path in root.glob("asm-*.stdout"):
        path.unlink()
    for spec in CASES:
        write_case(root, spec)
    print(f"Created {len(CASES)} real function-body asm tests in {root}")


if __name__ == "__main__":
    main()
