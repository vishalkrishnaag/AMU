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

REPL mode executes symbolic instructions, prints the active tape after each step with the active head marked, supports `PRINT_TAPE n` for inspecting another tape, and can load `IMPORT` lines plus `def ... end` function definitions into the live function loader before calling them. On exit it can save immediate statements under `def main` and loaded imports/functions as top-level `.in10s` source.

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
- **Functions** must use `def name` and close with `end`; old `name:` function declarations are not indexed
- **`$label:`** in function bodies = local jump target; does NOT terminate function loading
- **`argRegs`** vector in VM saved/restored across CALL/RET — callee inherits caller's args, nested calls are transparent
- **`@N` cell references in arithmetic args** — `ADD @2` reads the operand from tape cell N of the active tape instead of treating it as a literal; resolved by `resolveOperand()` in VM.cpp, used by ADD/SUB/MUL/DIV/MOD
- **Dynamic tape growth** — non-negative tape indexes grow the VM tape vector on demand; the startup tape count is only the initial capacity.
- **Tape names** — cell `-2` stores a purpose name for a tape; use `TAPE name` to select it, then call Code cells directly with `CALL @cell`
- **Routed cell addresses** — `tape.cell` addresses such as `1.2` or `memory_1alpha.2` are first-class resource locators; primitive inter-tape ops accept them directly, and `@tape.cell` dereferences a routed cell.
- **Evidence route learning** — `ROUTE_OBSERVE`/`ROUTE_LEARN` turns supplied statements into observation/resource cells plus explicit weighted evidence links; `ROUTE_CONTEXT` visualizes understanding without solving, while `ROUTE_REASON` is only model-defined dispatch.
- **Route cache lifecycle** — `ROUTE_USE`, `ROUTE_CACHE`, `ROUTE_PACK`, and `ROUTE_UNPACK` support a hot/warm/cold active model tape; cold packed memories can be persisted through the existing DB tape operations.
- **`QUOTE_FUNCTION name`** copies a normal `def ... end` body into the current tape cell as Code, including local `$label:` markers for Code-cell loops.
- **Worksheets** — `WORKSHEET_BEGIN` creates an incognito tape for temporary reasoning; it is auto-cleaned on return unless `WORKSHEET_KEEP` promotes it.
- **Emotional tapes** — `EMO_INIT` uses reserved cells `-20` hormones, `-21` events, `-22` logical ticks; readable `HORMONE name ... end` blocks define custom signals that drift toward baselines and expose `EMO_FIELD`.
- **Poet-HRM scaffolding** — `POET_SEARCH`, `POET_STRATEGY`, and `RATIONAL_RUN` keep retrieval, strategy selection, and evidence gates explicit.

## Instruction Categories

Tape nav/modules: `MOVE`, `BACK`, `HOME`, `TAPE`, `SEEK n`, `PRINT_TAPE n`, `TAPE_NAME`
Worksheet: `WORKSHEET_BEGIN`, `WORKSHEET_END`, `WORKSHEET_KEEP`, `WORKSHEET_STATE`
Emotional tape: `EMO_INIT`, `HORMONE`, `HORMONE_SET`, `HORMONE_ADD`, `HORMONE_BLOCK`, `HORMONE_UNBLOCK`, `HORMONE_LEVEL`, `HORMONE_INFO`, `EMO_ON`, `EMO_CHECK`, `EMO_TICK`, `EMO_FIELD`, `EMO_STATE`
Poet/Rational: `POET_SEARCH`, `POET_STRATEGY`, `RATIONAL_RUN`
Inter-tape: `COPY`, `TAPEREAD`/`TGET`/`PEEK`, `TAPEWRITE`/`TPUT`/`POKE`, `TAPESWAP`/`TSWAP`, `TAPESEND`/`TSEND`/`SEND`, `TAPERECV`/`TRECV`/`RECV`; these accept old `tape cell` operands or routed `tape.cell` addresses
Routing: `ROUTE_ADDR`/`RADDR`, `ROUTE_GET`/`RGET`, `ROUTE_SET`/`RSET`, `ROUTE_LINK`/`RLINK`/`CONNECT`, `ROUTE_TOUCH`/`RTOUCH`, `ROUTE_FOLLOW`/`RFOLLOW`, `ROUTE_INFO`/`RINFO`, `ROUTE_RESOURCE`/`RRESOURCE`, `ROUTE_ALIAS`/`RALIAS`, `ROUTE_ASSOC`/`RASSOC`, `ROUTE_FRAME`/`RFRAME`, `ROUTE_TRIGGER`/`RTRIGGER`, `ROUTE_SLOT`/`RSLOT`, `ROUTE_OBSERVE`/`ROBSERVE`, `ROUTE_LEARN`/`RLEARN`, `ROUTE_CONTEXT`/`RCONTEXT`, `ROUTE_USE`/`RUSE`, `ROUTE_CACHE`/`RCACHE`, `ROUTE_PACK`/`RPACK`, `ROUTE_UNPACK`/`RUNPACK`, `ROUTE_REASON`/`RREASON`/`ROUTE_QUERY`
Cell ops: `SET`, `PRINT`, `PRINTJ`, `LEN`/`LENGTH`, `CMP`/`COMPARE`, `DELETE`, `CLEAR_TAPE`/`CLEARTAPE`, `JSONGET`, `JSONHAS`, `JSONKEYS`, `JSONPARSE`, `JSONLEN`, `JSONSET`, `JSONDEL`, `JSONPUSH`  
Type sys: `TYPE`, `CAST`  
Arithmetic: `ADD`, `SUB`, `MUL`, `DIV`, `MOD`, `LT`, `GT`, `LTE`, `GTE`, `ABS`, `NEG` — binary ops accept literal or `@N`; unary ops work on current cell  
Strings: `CONCAT`, `SPLIT`, `JOIN`, `SUBSTR`, `FIND`, `REPLACE`, `UPPER`, `LOWER`, `REVERSE`  
File I/O: `READFILE`, `WRITEFILE`, `INPUT`  
PostgreSQL: `DBCONNECT`, `DBSTATUS`, `DBEXEC`, `DBQUERY`, `DBSELECT`, `DBINSERT`, `DBUPDATE`, `DBDELETE`, `DBCLOSE`, `DB_TAPE_OPEN`, `DB_TAPE_INPUT`, `DB_TAPE_OUTPUT`, `DB_TAPE_SAVE`, `DB_TAPE_LOAD`, `DB_TAPE_EVENT` — loaded through `libpq` at runtime  
Homoiconic: `EXEC`, `EVAL`, `QUOTE`, `QUOTE_FUNCTION`, `MATCH`, `TRY`, `RAISE`
Control flow: `JMP $label`, `JMPIF $label`, `JMPNOT $label`  
Code manip: `CODELEN`, `CODEGET`, `CODESET`, `CODEAPPEND`, `APPEND`, `MAKEANSWERCODE`, `MAKEANSWERSOURCE`  
ML stats: `MEANVAL`, `STDDEV`, `NORMALIZE`, `ZSCORE`, `SOFTMAX`  
ML linalg: `DOTPROD`  
ML supervised: `LINEARREG`, `PREDICT`  
ML clustering: `KMEANS`  
NLP service: `NLPLOAD`, `NLPTOKENS`, `NLPANALYZE`, `NLPSIM`, `NLPDIFF`, `NLPPREDICT`, `NLPSENTIMENT`, `NLPGRAMMAR`, `NLPCONTEXT`, `NLPLOGIC`, `NLPFUZZY`, `NLPPROB`  
Control: `CALL`, `ARG`, `RET`, `EXIT`, `IMPORT`  
Arg passing: `SETARG`, `GETARG`, `CLEARARGS`, `ARGCOUNT`

Named tape Code cells:
```
TAPE 1
TAPE_NAME file_tape
SEEK 10
QUOTE_FUNCTION file_directory_body
SEEK 0
CALL @10
```
Rewriting tape 1 cell 10 changes the next Code-cell call. `TAPE_NAME` keeps the tape easy to select while the call model stays direct.

## Calling Conventions

**Tape-oriented (preferred):**
```
SETARG 0 "hello"        # write literal to arg slot 0
SETARG 1 42
SET (SET 99)            # computed value
SETARG 2                # SETARG n with no value → use current cell
CALL my_fn

def my_fn
    GETARG 0            # → "hello" in current cell
    GETARG 1            # → 42
    GETARG 2            # → Code block
    RET
end
```

**Legacy inline (still works):**
```
CALL my_fn "hello" 42
def my_fn
    ARG 0               # → "hello"
    ARG 1               # → 42
    RET
end
```

## Code Block Syntax

```intense
SET (MOVE 10 ; SET "hello" ; PRINT)   # literal code block
EXEC                                   # execute it
QUOTE PRINT                            # store single instruction as Code
QUOTE (SET 1 ; ADD 1 ; PRINT)         # store multi-instruction block
QUOTE_FUNCTION file_directory_body     # store a def/end body as Code
```

## Control Flow (in-function labels)

```intense
def my_fn
    SET 5
$loop:
    PRINT               # $label: lines are jump targets, not emitted as instructions
    SUB 1
    JMPIF $loop
    RET
end
```

## Test Files

| File | What it covers |
|------|----------------|
| `example.intense` | Basic SET, PRINT, CALL, ARG, JSON cell |
| `examples/test_types.intense` | TYPE + CAST for all 9 ValueKind variants |
| `examples/test_arithmetic.intense` | ADD/SUB/MUL/DIV/MOD, int/float promotion |
| `examples/test_comparisons.in10s` | LT/GT/LTE/GTE including zero and negative operands |
| `examples/test_strings.intense` | CONCAT SPLIT FIND REPLACE SUBSTR UPPER LOWER |
| `examples/test_join_json_utils.in10s` | JOIN JSONHAS JSONKEYS JSONDEL |
| `examples/test_inter_tape_comm.in10s` | TAPEREAD TAPEWRITE TAPESWAP TAPESEND TAPERECV |
| `examples/test_try_catch.in10s` | TRY RAISE catch blocks over Code cells |
| `examples/test_ml.intense` | All ML ops: stats, normalization, softmax, dotprod, linearreg, predict, kmeans |
| `examples/test_homoiconic.intense` | EXEC EVAL QUOTE MATCH CODE* APPEND dynamic generation |
| `examples/test_controlflow.intense` | JMP JMPIF JMPNOT $labels loops nested CALL in loop |
| `examples/test_def_syntax.in10s` | def/end function definitions with local jump labels |
| `examples/test_exit.in10s` | EXIT halts nested calls and Code blocks |
| `examples/test_argpass.intense` | SETARG GETARG CLEARARGS ARGCOUNT nested call save/restore |
| `examples/test_stdlib_loop.in10s` | stdlib tape_loop/loop callbacks over a passed tape |
| `examples/test_tape_modules.in10s` | Tape names, QUOTE_FUNCTION def-body cells, direct Code-cell CALL, CODESET patching |
| `examples/test_tape_routing.in10s` | Routed `tape.cell` read/write, `@tape.cell`, route links, touch, follow, info |
| `examples/navigation_route_model.in10s` | Model-defined route resources, central frame, triggers, and missing slot |
| `examples/test_route_learning.in10s` | Automatic problem learning plus explainable model-defined route reasoning |
| `examples/test_emotional_tape.in10s` | Custom hormone levels, single/combo triggers, refractory blocks, shutdown event |
| `examples/test_poet_hrm.in10s` | Incognito worksheet, tape search, strategy selection, rational evidence, hormone field drift |
| `examples/algorithms_data_structures_probe.in10s` | Tape-native linear search, bubble sort, iterative quicksort, stack, queue |

Run focused checks with `./intense.out examples/<file>.in10s main`. Older label-style examples need migration to `def ... end`.

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
