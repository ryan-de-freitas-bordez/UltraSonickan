# UltraSonickan

UltraSonickan (`.usk`) is a C11-based language project with a portable reference runtime. The compiler pipeline now has a lexer, keyword classifier, owned AST, recursive-descent/Pratt parser, and structured diagnostics alongside the runtime and module loader. The project remains under active construction and does not claim source or ABI compatibility with Lua or C++.

## Build and run

Use a C11 compiler or CMake:

```sh
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Iinclude -o usk \
  src/usk.c src/native/c_backend.c src/native/c_backend_file.c \
  src/native/c_backend_bindings.c src/native/c_backend_runtime.c \
  src/native/c_backend_expression.c src/native/c_backend_statement.c \
  src/native/c_backend_declaration.c src/module/loader.c src/frontend/lexer.c \
  src/frontend/lexer_keywords.c src/frontend/lexer_result.c \
  src/frontend/ast.c src/frontend/ast_visit.c \
  src/frontend/ast_visit_expression.c src/frontend/ast_visit_statement.c \
  src/frontend/ast_visit_declaration.c src/frontend/parser.c src/frontend/parser_options.c \
  src/frontend/parser_recovery.c src/frontend/parse_expression.c \
  src/frontend/parse_statement.c src/frontend/parse_declaration.c \
  src/common/diagnostic.c src/common/diagnostic_json.c \
  src/runtime/value.c src/runtime/value_arena.c src/runtime/arraylib.c \
  src/runtime/pathlib.c src/runtime/path_lexical.c src/runtime/filesystem.c \
  src/runtime/stringlib.c src/runtime/string_view.c src/runtime/mathlib.c \
  src/runtime/math_integer.c \
  src/runtime/environment.c \
  src/runtime/evaluator.c \
  src/runtime/eval_call.c src/runtime/eval_expression.c \
  src/runtime/eval_statement.c src/sema/typecheck.c src/sema/analyzer.c \
  src/sema/resolve_type.c \
  src/sema/analyze_declaration.c src/sema/analyze_expression.c \
  src/sema/analyze_statement.c src/bytecode/chunk.c src/bytecode/verify.c \
  src/bytecode/disassemble.c src/bytecode/lower.c src/bytecode/vm.c -lm
./usk run examples/hello.usk
```

On Windows, build with CMake or a C compiler that supports C11, then run `usk.exe run examples/hello.usk`. `usk check [--json] file.usk` parses the source, registers declarations, checks scopes and types, and requires a `main` entry point; the JSON option writes structured diagnostics. `usk ast file.usk` prints the parsed AST. `usk bytecode file.usk` lowers the entry function and prints verified bytecode; `usk vm file.usk` runs its currently supported bytecode subset, including user-function calls and arrays. `usk emit-c file.usk [-o output.c]` emits a C11 translation unit for the backend's documented subset. Extra command-line arguments are available in a `USKString[]` entry parameter.

## Example

```usk
import <iostream.usk>

[EntryPoint]:
- ([pub]class)Main : [[use] iostream::io]
{
- (fn)main(USKString args[]) {
    io::writef("Hello World! {}", /n);
}
}
[EntryPoint::End]
```

The bundled `stdlib/iostream.usk` declares the runtime I/O ABI and implements
ordinary language helpers for typed output, prompts, headings, status lines,
and repeated separators. `stdlib/string.usk` provides string manipulation
helpers; the C runtime supplies byte-based search, slicing, joining, splitting,
replacement, trimming, repetition, and ASCII case conversion. `stdlib/math.usk`
provides numeric helpers over a checked C math ABI; the evaluator and bytecode
VM share explicit overflow-checked signed integer arithmetic. `stdlib/array.usk` exposes
typed collection algorithms, and `stdlib/algorithm.usk` adds sorting, searching,
prefix sums, merge, rotation, and filtering. `stdlib/path.usk` exposes portable
lexical path operations. `stdlib/fs.usk` wraps text-file reads and writes, line
helpers, file queries, copy/move, and single-directory creation. Filesystem
built-ins touch the host filesystem when a program invokes them. The C runtime
provides array insertion, removal, reserve, copy, reverse, slicing, search, and
mutation operations. Returned text and arrays live in the caller-owned value
region.

`/n` means a newline both inside strings and as an expression. `\n` also works in string literals. `io::writef` substitutes each `{}` with the next argument; `{{` and `}}` print literal braces.

The structured lexer records source byte ranges, reports JSON-compatible diagnostics, and supports source/token/string size limits plus nested block comments. The current string ABI is NUL-terminated, so a `\0` escape is diagnosed instead of being silently truncated.

The native C backend emits checked integer/floating-point helpers and allocation-free stream formatting. It handles the bundled `io::write`, `io::writeln`, and `io::writef` ABI calls directly; unsupported language features are diagnosed instead of producing placeholder output. Native emission currently supports primitive signatures, namespaced functions, enums, aliases, branches, loops, and switches, but does not yet support aggregate field layouts, general FFI, dynamic arrays, or `io::readline`.

## Core language

The parser builds owned AST nodes for functions, records, enums, typed declarations, expressions, blocks, branches, loops, and switch arms. A read-only AST visitor walks expressions, statements, types, and declarations in preorder, supports stop/skip callbacks, enforces node/depth limits, and computes expression/declaration summaries. Parser resource profiles cover editor, batch, and strict usage, with configurable token, diagnostic, nesting, and recovery limits. The current runtime supports `fn main(...)`, the bracketed EntryPoint/class wrapper, recursive source imports, `var` and `const` declarations, primitive values, arrays, function calls, return values, arithmetic and comparison expressions, `if`/`elif`/`else`, `switch`/`case`/`default`, `while`, C-style `for`, `for (var item in array)`, `break`, and `continue`. A static semantic pass checks declaration collisions, function arguments, local/global initializers, assignment mutability, conditions, loop and switch control flow, and returns before `run` executes source. Explicit local declarations such as `var USKInt count = 0;` and `const USKString label = "ready";` also check initializer and assignment types at runtime. Bytecode lowering includes literals, locals, calls, array construction/indexing, indexed assignment, `.length`/`.size`, array receiver methods, branches, `switch`, foreach, and counted/while loops. The bytecode VM executes arithmetic, comparisons, locals, branches, top-level global initialization and access, arrays, user functions, built-ins, switch/foreach control flow, and bounded loops with shared instruction and call-depth limits. It remains an incomplete subset and is not yet the default replacement for the AST runtime. `->` and `::` are accepted as name separators for calls. `switch` arms do not fall through implicitly.

The reserved vocabulary includes `use`, `fn`, `var`, `const`, `if`, `else`, `elif`, `enum`, `class`, `struct`, `USKInt`, `USKDouble`, `USKString`, `USKChar`, `USKFloat`, `USKBool`, `USKNull`, `typedef`, `switch`, `case`, `break`, `continue`, `default`, `define`, `ifndef`, `endif`, `extern`, `public`, `private`, `for`, `in`, `USKAuto`, `USKLong`, `USKShort`, `inline`, `static`, `signed`, `unsigned`, `import`, `typeof`, `while`, `volatile`, `true`, and `false`.

Some vocabulary is reserved for future compiler/runtime work: class and struct layout, enums, preprocessor directives, FFI/linkage, access control, and qualifiers are not implemented yet. Primitive size metadata exists, but source-level modifier declarations and numeric range enforcement are still incomplete. Runtime numeric storage currently uses C's `long long` and `double`. See [the project status](docs/STATUS.md) for the current implementation count and staged scope.

UltraSonickan has no garbage collector. Interpreter value allocations live in an explicitly owned region attached to the evaluator result; call `usk_evaluator_result_destroy` to release that region after consuming its return value. Allocation-count and byte limits can be configured through evaluator options. The bytecode VM borrows a caller-owned value region for all frames, including nested user-function frames. Bytecode chunks own and release their copied constants and instructions. Array and string values never outlive their owning region or chunk.
