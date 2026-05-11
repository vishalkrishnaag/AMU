# Intense VM — Claude Project Context

## Documents

- [doc.md](doc.md) — original design spec: core philosophy, homoiconicity model, aspirational instruction set
- [in10s_lazy_loaded_vm_cpp_project.md](in10s_lazy_loaded_vm_cpp_project.md) — full language reference: instruction tables, value types, language rules

## What This Is

A **symbolic tape-oriented virtual machine** implemented in C++. Inspired by Turing machines, Brainfuck, stack VMs, and symbolic execution engines. Source files may use `.intense` or `.in10s`.

## Build & Run

```bash
make                                              # compiles → intense.out
./intense.out <file.intense|file.in10s> [entry=main] [tapes=4] [--debug] [--step]
./intense.out --repl [file.intense|file.in10s] [tapes=4] [--debug]
```

REPL mode executes one symbolic instruction at a time, prints the active tape after each step with the active head marked, supports `PRINT_TAPE n` for inspecting another tape, and asks on exit whether to save the entered instruction history as a `.in10s` file.

## Architecture — One Sentence Each

| File | Role |
|------|------|
| `Value.hpp` | `ValueKind` enum + boxed `Value` variant (9 kinds: Nil Bool Int Float Char Str List Map **Code**) |
| `Instruction.hpp` | POD struct: opcode (uppercase string), args (vector<string>), line number + `operator==` |
| `Tape.hpp` | Sparse tape: `unordered_map<long long, Value>` + head pointer |
| `Lexer.hpp/cpp` | Tokenises a line; handles quoted strings, chars, JSON literals, `(...)` code blocks, `#` comments |
| `Parser.hpp/cpp` | Normalises opcode to uppercase, builds `Instruction` |
| `FunctionLoader.hpp/cpp` | Lazy load + LRU-100 cache + IMPORT; `Function` now carries `localLabels` for `$label:` jump targets |
| `VM.hpp/cpp` | Executes instructions; owns tapes + `argRegs` vector for SETARG/GETARG |
| `main.cpp` | CLI entry: `<file> [entry] [tapes] [--debug] [--step]` |

## Key Design Decisions (do not reverse without discussion)

- **ValueKind enum** — explicit; no string-based type guessing anywhere (now 9 kinds including Code)
- **Opcodes & labels normalised to UPPERCASE** everywhere (parse + lookup)
- **LRU cache** uses `std::list` + `unordered_map<string, list<string>::iterator>` for true O(1)
- **String op args** all go through `parseValue()` → `stringifyValue()` to strip quotes before use
- **`LEN` and `CMP`** store into current cell (not print) — they are composable
- **Unknown opcodes** throw `std::runtime_error` (no silent ignore)
- **`#`** starts a comment anywhere on a line (handled in Lexer before token loop)
- **`SOFTMAX`** subtracts max before exp for numerical stability
- **Code blocks** use `(INSTR1 ; INSTR2 ; ...)` syntax; `(` tokenised as structured token in Lexer
- **`$label:`** in function bodies = local jump target; does NOT terminate function loading
- **`argRegs`** vector in VM saved/restored across CALL/RET — callee inherits caller's args, nested calls are transparent
- **`@N` cell references in arithmetic args** — `ADD @2` reads the operand from tape cell N of the active tape instead of treating it as a literal; resolved by `resolveOperand()` in VM.cpp, used by ADD/SUB/MUL/DIV/MOD

## Instruction Categories

Tape nav: `MOVE`, `BACK`, `TAPE`, `SEEK n`, `PRINT_TAPE n`  
Cell ops: `SET`, `PRINT`, `PRINTJ`, `LEN`/`LENGTH`, `CMP`/`COMPARE`, `COPY`, `DELETE`, `CLEAR_TAPE`/`CLEARTAPE`  
Type sys: `TYPE`, `CAST`  
Arithmetic: `ADD`, `SUB`, `MUL`, `DIV`, `MOD`, `ABS`, `NEG` — binary ops accept literal or `@N`; unary ops work on current cell  
Strings: `CONCAT`, `SPLIT`, `SUBSTR`, `FIND`, `REPLACE`, `UPPER`, `LOWER`  
File I/O: `READFILE`, `WRITEFILE`, `INPUT`  
Homoiconic: `EXEC`, `EVAL`, `QUOTE`, `MATCH`, `TRY`, `RAISE`  
Control flow: `JMP $label`, `JMPIF $label`, `JMPNOT $label`  
Code manip: `CODELEN`, `CODEGET`, `CODESET`, `CODEAPPEND`, `APPEND`  
ML stats: `MEANVAL`, `STDDEV`, `NORMALIZE`, `ZSCORE`, `SOFTMAX`  
ML linalg: `DOTPROD`  
ML supervised: `LINEARREG`, `PREDICT`  
ML clustering: `KMEANS`  
NLP service: `NLPLOAD`, `NLPTOKENS`, `NLPANALYZE`, `NLPSIM`, `NLPDIFF`, `NLPPREDICT`, `NLPSENTIMENT`, `NLPGRAMMAR`, `NLPCONTEXT`, `NLPLOGIC`, `NLPFUZZY`, `NLPPROB`  
Control: `CALL`, `ARG`, `RET`, `IMPORT`  
Arg passing: `SETARG`, `GETARG`, `CLEARARGS`, `ARGCOUNT`

## Calling Conventions

**Tape-oriented (preferred):**
```
SETARG 0 "hello"        # write literal to arg slot 0
SETARG 1 42
SET (SET 99)            # computed value
SETARG 2                # SETARG n with no value → use current cell
CALL my_fn

my_fn:
    GETARG 0            # → "hello" in current cell
    GETARG 1            # → 42
    GETARG 2            # → Code block
    RET
```

**Legacy inline (still works):**
```
CALL my_fn "hello" 42
my_fn:
    ARG 0               # → "hello"
    ARG 1               # → 42
    RET
```

## Code Block Syntax

```intense
SET (MOVE 10 ; SET "hello" ; PRINT)   # literal code block
EXEC                                   # execute it
QUOTE PRINT                            # store single instruction as Code
QUOTE (SET 1 ; ADD 1 ; PRINT)         # store multi-instruction block
```

## Control Flow (in-function labels)

```intense
my_fn:
    SET 5
$loop:
    PRINT               # $label: lines are jump targets, not emitted as instructions
    SUB 1
    JMPNOT $done        # jump if current cell is falsy
    JMP $loop
$done:
    RET
```

## Test Files

| File | What it covers |
|------|----------------|
| `example.intense` | Basic SET, PRINT, CALL, ARG, JSON cell |
| `examples/test_types.intense` | TYPE + CAST for all 9 ValueKind variants |
| `examples/test_arithmetic.intense` | ADD/SUB/MUL/DIV/MOD, int/float promotion |
| `examples/test_strings.intense` | CONCAT SPLIT FIND REPLACE SUBSTR UPPER LOWER |
| `examples/test_ml.intense` | All ML ops: stats, normalization, softmax, dotprod, linearreg, predict, kmeans |
| `examples/test_homoiconic.intense` | EXEC EVAL QUOTE MATCH CODE* APPEND dynamic generation |
| `examples/test_controlflow.intense` | JMP JMPIF JMPNOT $labels loops nested CALL in loop |
| `examples/test_argpass.intense` | SETARG GETARG CLEARARGS ARGCOUNT nested call save/restore |

All tests pass. Run each with `./intense.out examples/<file>.intense main`.

## Bugs Already Fixed (do not reintroduce)

| Bug | Fix |
|-----|-----|
| `auto& tape` declared twice in `execute()` | Single declaration at top of unified if-else chain |
| `<fstream>` missing | Added to VM.cpp |
| String op args included raw quotes | All args through `parseValue()` |
| `LEN`/`CMP` printed instead of storing | Now store into current cell |
| LRU cache was FIFO | `list::splice` + iterator map gives true O(1) LRU |
| Labels case-sensitive | Normalised to uppercase in `buildIndex()` and `loadFunction()` |
| No comment syntax | `if (c == '#') break;` in Lexer token loop |
| Unknown opcodes silently ignored | Now throw `runtime_error` |
| `_` wildcard in JSON array patterns threw | `parseJsonLiteral` is now the catch-all fallback in `parseJsonValue` |
| Tape ptr accumulates across test calls | `HOME` instruction resets active tape ptr to 0 |

## Conventions

- `.intense` files: one instruction per line, opcodes case-insensitive, labels end with `:`
- Local jump labels: `$name:` — scoped to the enclosing function, used with JMP/JMPIF/JMPNOT
- Arithmetic operands: literal value (`ADD 5`) or `@N` tape cell reference (`ADD @2` reads cell 2 of the active tape)
- Value literals: `42` int, `3.14` float, `true`/`false` bool, `'A'` char, `"str"` string, `[1,2]` list, `{"k":v}` map, `null`/`nil` nil, `(INSTR ; ...)` code
- Arithmetic cell refs: `@N` in an arithmetic arg reads from cell N of the active tape (e.g. `ADD @0`, `MUL @3`)
- `IMPORT path/to/file.intense` or `IMPORT path/to/dir/` — relative to importer, circular-safe
