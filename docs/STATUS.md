# UltraSonickan implementation status

## Accepted project constraint

The source tree is being grown toward at least 300 substantive implementation files, with at least 100 lines of real implementation in every counted file. The current work is an early milestone and does not meet that threshold. Line count is tracked with code that implements a language or developer tool; repeated boilerplate is not counted as an implementation module.

## Current inventory

There are 51 C implementation files, 23 headers, and seven standard-library `.usk` source files: 81 implementation/source files total. All 51 C translation units and all seven standard-library modules exceed 100 lines; ten headers also exceed 100 lines. The other 13 headers are currently below the per-file size floor. In total, 68 of the 81 source files meet the 100-line floor. The literal 300-file minimum is 219 files away; UltraSonickan remains an early implementation rather than a finished Lua/C++ replacement.

| Area | Files | Current responsibility |
|---|---:|---|
| CLI/runtime driver | 1 C file | Command dispatch, parsing, AST printing, semantic checks, runtime invocation |
| Front end | 14 C files, 6 headers | Structured lexing with source spans, keyword metadata, bounded token and literal sizes, nested comments, AST ownership/printing, bounded read-only AST traversal and subtree metrics, configurable parser profiles and resource limits, delimiter-aware error recovery, parsing of declarations, expressions, and statements |
| Diagnostics | 2 C files, 1 header | Structured diagnostic storage, terminal rendering, severity queries, JSON and JSON-lines output |
| Module system | 1 C file, 1 header | Recursive import expansion, relative lookup, and import de-duplication |
| Runtime | 15 C files, 9 headers | Values, limited explicit region ownership, geometric arrays and array built-ins, lexical path utilities, checked filesystem text operations, byte-string operations, binary-safe views and UTF-8 validation/slicing, checked floating-point and signed integer arithmetic, evaluator, calls, expressions, statements, nested scopes, and assignment |
| Semantic analysis | 6 C files, 3 headers | Symbol registration, typedef and primitive type resolution, expression typing, scopes, control flow, and diagnostics |
| Bytecode compiler/VM | 5 C files, 1 header | AST lowering, owned constant pools, source-mapped instructions, operand-stack/control-flow verifier, disassembler, and bounded VM with built-ins, nested user-function frames, initialized global bindings, arrays, indexing, receiver methods, switch, and foreach |
| Native C11 backend | 7 C files, 2 headers | Scoped binding/type inference, checked C11 expression and statement emission, allocation-free generated runtime helpers, explicit unsupported-feature diagnostics, staged file output, and `emit-c` CLI integration |
| Standard library | 7 `.usk` files | Stream helpers, byte-string helpers, numeric helpers, typed array algorithms, sequence algorithms, lexical path helpers, and filesystem convenience functions |

## Next implementation milestones

1. Current grammar slice: lexer with structured diagnostics, source offsets, keyword metadata, nested comments, and resource caps; owned AST; read-only AST visitor and expression/declaration summaries; declaration/expression/statement parser; parser resource profiles; bounded nesting and token/error limits; delimiter-aware recovery; syntax checking; AST dump; and basic AST evaluation. Broaden grammar coverage and editor tooling.
2. Static semantic foundation: declaration registration, function arguments, initializer and assignment types, primitive and typedef resolution, control-flow placement, and return checking are integrated in `check` and before `run`. Add overloads, visibility enforcement, definite assignment, inheritance checks, and more complete conversions.
3. AST lowering, the `bytecode` inspection command, and bounded VM execution are integrated for literals, locals, built-in and user-function calls, top-level globals, assignments, arrays, indexing, array mutation/search/slicing methods, length, branches, arithmetic, C-style/foreach loops, switch, string/path/filesystem built-ins, and numeric built-ins. Add remaining aggregate operations and make bytecode execution the default backend.
4. Implement class/struct layouts, methods, constructors, `USKAuto` member dispatch, and complete enum/switch semantics.
5. Implement preprocessing, external declarations/FFI, generics/templates, and source-level storage and size modifiers.
6. Expand the standard library, command-line tooling, native back ends, and compiler subsystems as substantive modules until every source file meets the line floor and at least 300 source files exist. Array, math, algorithm, path, filesystem, and the initial C11 backend modules are included in the current 81-file inventory.

Each milestone must be integrated into the build and represented in the language specification before it is counted as implemented. The C11 backend currently accepts an explicitly limited subset and reports unsupported syntax instead of emitting pretend implementations. It maps imported stream `write`, `writeln`, and `writef` calls to allocation-free helpers; the general FFI ABI and `readline` are not implemented.

## Memory management constraint

UltraSonickan has no garbage collector. The interpreter uses an explicit region owner attached to each evaluator result; callers release it with `usk_evaluator_result_destroy`. Allocation-count and byte limits fail with diagnostics. Shallow-copied string and array values remain valid for that result's lifetime, and array growth updates the owner's allocation record. The bytecode VM borrows one caller-owned value region across globals and nested function frames, while bytecode chunks separately own and release copied constants and instruction storage. The VM shares one instruction budget and call-depth limit across nested calls. Heap constructors and value formatting report allocation failure without terminating the process; arena-backed runtime constructors set the arena's failure flag.
