# Intense — Lazy Loaded Symbolic Tape VM (C++ Project)

A symbolic tape-oriented virtual machine inspired by Turing machines, Brainfuck minimalism, stack VMs, and symbolic execution engines.

> Agent context: see [CLAUDE.md](CLAUDE.md) for architecture summary, design decisions, and bug history.

---

## Quick Start

```bash
make
./intense.out <file.intense|file.in10s> [entry=main] [tapes=4] [--debug] [--step]
./intense.out --repl [file.intense|file.in10s] [tapes=4] [--debug]
```

Examples:
```bash
./intense.out example.intense
./intense.out examples/test_ml.intense main 8
./intense.out examples/test_types.intense main --debug
./intense.out --repl
```

REPL mode accepts one symbolic instruction per line, shows the active tape head after each step, and can save the entered session as a `.in10s` file on exit. Both `.intense` and `.in10s` are valid source extensions.

---

## Basic Language Rules

### 1 — One instruction per line
Each non-blank, non-comment line contains exactly one instruction, optionally followed by arguments:

```
OPCODE [arg1] [arg2] ...
```

### 2 — Comments
`#` starts a comment; everything from `#` to end-of-line is ignored.

```
SET 42       # stores integer 42 in the current cell
```

### 3 — Opcodes are case-insensitive
`SET`, `set`, and `Set` are all equivalent.

### 4 — Functions use def/end
`def name` starts a function and `end` closes it. Function names are case-insensitive.

```
def my_function
    SET 99
    PRINT
    RET
end
```

### 5 — Value types are explicit (ValueKind enum)

The VM uses a strict `ValueKind` enum — no implicit type guessing:

| Kind  | C++ type          | Literal examples            |
|-------|-------------------|-----------------------------|
| Nil   | `std::monostate`  | `null`, `nil`               |
| Bool  | `bool`            | `true`, `false`             |
| Int   | `long long`       | `42`, `-7`, `+100`          |
| Float | `double`          | `3.14`, `-0.5`, `1e10`      |
| Char  | `char`            | `'A'`, `'\n'`               |
| Str   | `std::string`     | `"hello world"`             |
| List  | `vector<Value>`   | `[1, 2.5, "x"]`             |
| Map   | `map<str, Value>` | `{"key": 42, "flag": true}` |

### 6 — Tapes are sparse integer→Value maps
Each tape is an `unordered_map<long long, Value>`. The tape pointer starts at 0. Unwritten cells return `nil`.

Tapes can be named for a purpose. Cell `-2` stores the optional tape name so
instructions such as `TAPE file_tape` can select it without relying on a raw
index.

### 7 — Functions, CALL, and RET
```
CALL function_name [arg0] [arg1] ...
CALL @0
```
Arguments are parsed as Values. `CALL name` still runs a source-loaded
function. `CALL @0` can dispatch from a cell containing a function name or a
`Code` value. `RET` returns to the caller.

### 8 — Imports
```
IMPORT path/to/file.intense
IMPORT path/to/directory/
```
Resolved relative to the importing file. Circular imports are detected and skipped.

---

## Instruction Reference

### Tape Navigation

| Instruction | Description |
|-------------|-------------|
| `MOVE n`    | Move tape pointer forward by `n` cells |
| `BACK n`    | Move tape pointer backward by `n` cells |
| `SEEK n`    | Set the active tape pointer to cell `n` |
| `HOME`      | Reset the active tape pointer to cell 0 |
| `TAPE n/name` | Switch active tape to non-negative index, `tapeN`, or a named tape; tapes grow on demand |
| `PRINT_TAPE n/name` | Print tape without switching the active tape; accepts index or named tape |
| `TAPE_NAME name` | Name the active tape and store that name in reserved cell `-2` |
| `TAPE_NAME tape name` | Name a specific tape by index or existing tape name |

### Incognito Worksheets

Worksheets are temporary tapes for problem visualization and short-term
reasoning. They are auto-cleaned when the creating function or Code block
returns unless promoted.

| Instruction | Description |
|-------------|-------------|
| `WORKSHEET_BEGIN [name]` | Create a temporary tape, name it, push a worksheet frame, and switch to it |
| `WORKSHEET_END [keep]` | Close the active worksheet, restore the previous tape, and destroy it unless kept |
| `WORKSHEET_KEEP [name]` | Promote the active worksheet into a normal named tape |
| `WORKSHEET_STATE` | Store the active worksheet stack in the current cell |

### Emotional / Hormone Tape

These instructions build a programmable signaling tape. Hormone names are
custom strings; `cortisol`, `dopamine`, and `adrenaline` are examples, not
reserved names. Reserved cells on the emotional tape are `-20` for the hormone
registry, `-21` for event rules, and `-22` for the logical tick clock.

| Instruction | Description |
|-------------|-------------|
| `EMO_INIT [name]` / `EMOTIONAL_TAPE` | Initialize the active tape as a named emotional tape |
| `HORMONE name ... end` | Create/update a hormone signal with readable block fields |
| `HORMONE_SET name level` | Set a hormone percentage, clamped to its bounds |
| `HORMONE_ADD name delta` / `HORMONE_SPIKE` / `HORMONE_DROP` | Adjust a hormone level; blocked spike/drop windows are respected |
| `HORMONE_BLOCK name ticks [spike|drop|both]` | Prevent further changes in one direction until the logical tick expires |
| `HORMONE_UNBLOCK name` | Clear a hormone block window |
| `HORMONE_LEVEL name` | Store the current hormone level in the current cell |
| `HORMONE_INFO name` | Store the full hormone metadata map in the current cell |
| `EMO_ON event action [options] conditions...` | Register a trigger; conditions are all required |
| `EMO_CHECK` | Evaluate triggers and call their action functions, or `EXIT`/`SHUTDOWN` |
| `EMO_TICK [n]` | Advance logical time, drift hormones toward baseline, and run `EMO_CHECK` |
| `EMO_FIELD` | Store the current global emotional field summary |
| `EMO_STATE` | Store `{tick,hormones,events,field}` in the current cell |

Trigger conditions can be compact (`dopamine>=60`) or spaced
(`dopamine >= 60`). Multiple conditions create a combination trigger. Options
include `cooldown=N`, `refractory=N`, `block=name1,name2`, and `block_for=N`.

Hormones should prefer block syntax once they carry more than a level:

```intense
HORMONE cortisol
    hormone_level 5
    hormone_nature stress
    hormone_min 0
    hormone_max 100
    hormone_baseline 20
    hormone_decay 0.1
    hormone_sensitivity 1
    hormone_weight 1
end
```

The loader lowers that block into the VM's compact hormone instruction. Fields
can also be written as short names (`level`, `nature`, `min`, `max`,
`baseline`, `decay`, `sensitivity`, `weight`).

### Poet / Rational Scaffolding

These are explicit symbolic hooks for hierarchical reasoning: retrieve relevant
tapes, choose a strategy, and gate activation by evidence rather than
probability alone.

| Instruction | Description |
|-------------|-------------|
| `POET_SEARCH [query] [limit]` | Search all tape cells for textual relevance; results include `address` as `tape.cell` |
| `POET_STRATEGY [problem]` | Classify a problem and return selected layers/methods/evidence gate |
| `RATIONAL_RUN [boolean|fuzzy|math|nlp|ml]` | Evaluate the current cell through one rational layer and return structured evidence |

### Inter-Tape Communication

Modern routed addressing uses `tape.cell`, where the tape side may be a numeric
index (`1.2`) or a `TAPE_NAME` (`memory_1alpha.2`). A routed resource locator can
also be written as `@tape.cell`; `@N` keeps its original meaning as a local cell
reference or an indirect cell containing an address.

| Instruction | Description |
|-------------|-------------|
| `COPY tape` | Copy current cell to the destination tape's current pointer |
| `TAPEREAD tape [cell]` / `TGET` / `PEEK` | Read another tape cell, or a routed address like `1.2`, into the current cell |
| `TAPEWRITE tape [cell] [value]` / `TPUT` / `POKE` | Write current cell, or `value`, into another tape cell or routed address |
| `TAPESWAP tape [cell]` / `TSWAP` | Swap current cell with another tape cell or routed address |
| `TAPESEND tape [cell] [value]` / `TSEND` / `SEND` | Append current cell, or `value`, to a list mailbox on another tape or routed address |
| `TAPERECV tape [cell]` / `TRECV` / `RECV` | Pop the oldest value from a list mailbox into the current cell; empty mailbox gives `nil` |
| `ROUTE_ADDR [tape] [cell]` / `RADDR` | Return the canonical numeric address for the current cell, a cell, or a tape/cell pair |
| `ROUTE_GET address` / `RGET` | Read a routed address into the current cell |
| `ROUTE_SET address [value]` / `RSET` | Write current cell, or `value`, into a routed address |
| `ROUTE_LINK from to [relation] [strength]` / `RLINK` / `CONNECT` | Create or update a weighted cell-to-cell connection |
| `ROUTE_TOUCH from to [relation] [delta]` / `RTOUCH` | Strengthen or weaken an existing connection; default delta is `0.1` |
| `ROUTE_FOLLOW address [relation] [limit]` / `RFOLLOW` | Return strongest linked target cells, including their current `value` |
| `ROUTE_INFO address` / `RINFO` | Return address metadata plus incoming/outgoing links |
| `ROUTE_RESOURCE key [kind] [label]` / `RRESOURCE` | Create or reuse a central resource cell in the active model tape |
| `ROUTE_ALIAS canonical_key alias_key` / `RALIAS` | Add an alternate lookup key for the same central resource |
| `ROUTE_ASSOC from_key to_key [relation] [strength]` / `RASSOC` | Connect two model resources by key without manual address lookup |
| `ROUTE_FRAME key [decision] [question]` / `RFRAME` | Define a direct router frame as a central decision point |
| `ROUTE_TRIGGER frame_key concept_key [strength]` / `RTRIGGER` | Add required evidence for a frame; matched by direct resource index lookup |
| `ROUTE_SLOT frame_key slot_key [question]` / `RSLOT` | Add a missing information slot that the frame should ask for |
| `ROUTE_OBSERVE [text] [memory_tape]` / `ROBSERVE` | Store a statement as an observation and build understandable resource associations |
| `ROUTE_LEARN [text] [memory_tape]` / `RLEARN` | Alias of `ROUTE_OBSERVE` for compatibility |
| `ROUTE_CONTEXT [text] [memory_tape] [limit]` / `RCONTEXT` | Inspect recognized resources, unresolved concepts, associations, and possible frames without deciding |
| `ROUTE_USE key_or_address [memory_tape] [amount]` / `RUSE` | Mark a resource/observation as currently used and heat it for runtime cache retention |
| `ROUTE_CACHE [memory_tape] [hot] [cold]` / `RCACHE` | Classify active route cells as hot/warm/cold for model-cache management |
| `ROUTE_PACK [memory_tape] [hot|warm|cold|*]` / `RPACK` | Serialize selected route cells into scalar JSON text suitable for DB persistence |
| `ROUTE_UNPACK [payload] [memory_tape]` / `RUNPACK` | Rehydrate a packed route snapshot and rebuild resource/frame indexes |
| `ROUTE_REASON [text] [memory_tape]` / `RREASON` / `ROUTE_QUERY` | Optional model-level dispatch: match context against frame triggers and return an explainable route decision |

### Tape-Native Postgres Memory

These instructions treat Postgres tables as a persisted tape replica. Reasoning
still happens in VM runtime; programs load only the tape cells they need. The
Postgres replica stores scalar/code cells; nested structures should be expressed
across tape cells instead of JSON.

| Instruction | Description |
|-------------|-------------|
| `DB_TAPE_OPEN space [kind]` | Ensure a tape memory namespace exists |
| `DB_TAPE_INPUT space [tape] [cell] [value]` | Persist one scalar/code tape cell; defaults to active tape, current cell, current value |
| `DB_TAPE_OUTPUT space [tape] [cell]` | Load one persisted cell into the current VM cell |
| `DB_TAPE_SAVE space [tape|*]` | Persist occupied cells from one tape or all current tapes |
| `DB_TAPE_LOAD space [tape|*]` | Load persisted cells from one tape or all tapes |
| `DB_TAPE_EVENT space event_type [note]` | Record a runtime reasoning/process marker |

### Cell Operations

| Instruction    | Description |
|----------------|-------------|
| `SET value`    | Write a value into the current cell |
| `PRINT`        | Print the current cell (human-readable) |
| `PRINTJ`       | Print the current cell as JSON |
| `LEN`          | Store the cell count of the current tape into the current cell |
| `CMP n`        | Compare current cell with tape `n`'s current cell; store `bool` result |
| `DELETE`       | Delete the active tape's current cell |
| `CLEAR_TAPE`   | Clear occupied cells on the active sparse tape and reset pointer to 0 |
| `JSONGET key`  | Read a map key or list index from the current JSON-like value |
| `JSONHAS key`  | Store whether a map key or list index exists |
| `JSONKEYS`     | Store sorted map keys, or list indexes, as a List |
| `JSONSET key value` | Set a map key or list index |
| `JSONDEL key`  | Delete a map key or list index |
| `JSONPUSH value` | Append to current List, creating one from nil if needed |

### Type System

| Instruction    | Description |
|----------------|-------------|
| `TYPE`         | Store the type name string of the current cell (`"int"`, `"str"`, etc.) |
| `CAST target`  | Convert current cell to `int`, `float`, `bool`, `str`, or `char` |

### Arithmetic

All arithmetic ops read the current cell as the **left operand** and write the result back.
When both operands are `Int`, the result is `Int`; otherwise `Float`.

The operand argument accepts two forms:
- **Literal** — `ADD 5` adds the integer 5 to the current cell
- **Cell reference** — `ADD @2` reads the value from cell 2 of the active tape

| Instruction   | Description |
|---------------|-------------|
| `ADD value`   | current ← current + value |
| `SUB value`   | current ← current − value |
| `MUL value`   | current ← current × value |
| `DIV value`   | current ← current ÷ value (throws on zero) |
| `MOD value`   | current ← current mod value (integer only) |
| `LT value`    | current ← current < value |
| `GT value`    | current ← current > value |
| `LTE value`   | current ← current <= value |
| `GTE value`   | current ← current >= value |

**Cell reference syntax:** prefix the argument with `@` followed by an integer cell index.
The index refers to the **active tape** (use `TAPE n` first for cross-tape reads).

```intense
def main
    SEEK 0
    SET 10          # cell[0] = 10
    SEEK 1
    SET 3           # cell[1] = 3
    SEEK 5
    SET 4
    MUL @0          # cell[5] = 4 * cell[0]  →  40
    ADD @1          # cell[5] = 40 + cell[1]  →  43
    DIV @0          # cell[5] = 43 / cell[0]  →  4  (int)
    PRINT
    RET
end
```

### Control Flow

Local jump labels use `$name:` so they are visually distinct from `@N` cell references.
They are scoped to the enclosing function and are valid targets for `JMP`, `JMPIF`, and `JMPNOT`.

```intense
def main
    SET 3
$loop:
    PRINT
    SUB 1
    JMPIF $loop
    RET
end
```

### String Operations

| Instruction        | Description |
|--------------------|-------------|
| `CONCAT value`     | Append `value` (stringified) to current cell |
| `SPLIT delim`      | Split current string by delimiter; store List of Str |
| `JOIN [delim]`     | Join current List into a Str using `delim` |
| `SUBSTR start len` | Extract substring from current Str cell |
| `FIND sub`         | Store 0-based index of first occurrence, or −1 |
| `REPLACE old new`  | Replace all occurrences of `old` with `new` in current Str |
| `UPPER`            | Convert current Str to uppercase in place |
| `LOWER`            | Convert current Str to lowercase in place |
| `REVERSE`          | Reverse current Str in place |

### File I/O

| Instruction        | Description |
|--------------------|-------------|
| `READFILE path`    | Read entire file into current cell as Str; `path` may be a literal or `@N` cell reference |
| `WRITEFILE path`   | Write current cell (stringified) to file; `path` may be a literal or `@N` cell reference |

### ML — Descriptive Statistics

Operate on a List of numbers in the current cell.

| Instruction | Description |
|-------------|-------------|
| `MEANVAL`   | Replace list with its arithmetic mean (Float) |
| `STDDEV`    | Replace list with its population standard deviation (Float) |

### ML — Normalization

| Instruction  | Description |
|--------------|-------------|
| `NORMALIZE`  | Min-max scale list to [0, 1] |
| `ZSCORE`     | Standardise list to zero mean, unit variance |

### ML — Activation Functions

| Instruction | Description |
|-------------|-------------|
| `SOFTMAX`   | Apply softmax to list (output sums to 1.0) |

### ML — Linear Algebra

| Instruction    | Description |
|----------------|-------------|
| `DOTPROD cell` | Dot product of current List with List at tape position `cell`; store scalar |

### ML — Supervised Learning

| Instruction    | Description |
|----------------|-------------|
| `LINEARREG`    | Fit `y = slope·x + intercept` on a List of `[x, y]` pairs; stores `[slope, intercept]` |
| `PREDICT cell` | Predict `y` for the number in current cell using model at position `cell` |

### ML — Clustering

| Instruction         | Description |
|---------------------|-------------|
| `KMEANS k [iters]`  | 1-D k-means on List; stores List of `k` centroids. Default 100 iterations |

### NLP — Text Service

NLP is implemented as core tape-native text analysis by default. The machine does not depend on an archived NLP framework.
`NLPLOAD` is retained as a compatibility hook for future optional backends, but the default backend stays deterministic and local.

| Instruction              | Description |
|--------------------------|-------------|
| `NLPLOAD [path]`         | Store backend status for a future external NLP model path; default backend remains core |
| `NLPTOKENS`              | Tokenize current text/code cell into lowercase token list |
| `NLPANALYZE`             | Store text/code stats map: chars, lines, tokens, unique tokens, instruction-like lines |
| `NLPSIM cell [TOKEN]`    | Compare current text with text at tape `cell` using token cosine similarity |
| `NLPDIFF cell`           | Store map with similarity, distance, shared, left-only, and right-only tokens |
| `NLPPREDICT [k] [thr]`   | Classify current text using the core lexicon classifier; stores `[[label, score], ...]` |
| `NLPSENTIMENT [k] [thr]` | Store transparent lexicon sentiment: label, score, polarity, hits, predictions |
| `NLPGRAMMAR`             | Check grammar of current text; store map with score and errors |
| `NLPCONTEXT`             | Analyze context; store map with nouns, verbs, adjectives, topic |
| `NLPLOGIC [op]`         | Apply logical operation (contrapositive, converse) to current statement; store result map |
| `NLPFUZZY [mem]`        | Fuzzy logic analysis with membership threshold; store fuzzy scores |
| `NLPPROB [type]`        | Calculate token probabilities; store map with frequencies and probabilities |

### Assembly-Level Error Handling

Errors are represented as tape values when caught. `TRY` executes the Code stored in the current cell.
If execution throws and a catch cell is provided, the current cell is replaced with an error map such as
`{"error":"DIV by zero","handled":true}`, then the catch Code cell is executed.

| Instruction        | Description |
|--------------------|-------------|
| `TRY [catchCell]`  | Execute current Code cell; on error, optionally execute Code stored at `catchCell` |
| `RAISE [message]`  | Throw an assembly-level error using `message`, or the current cell if omitted |

### Function Definitions and Calls

Preferred function syntax:

```intense
def greet
SET "hello"
PRINT
RET
end

def main
CALL greet
RET
end
```

Functions must be declared with `def name` and closed with `end`.

Tape-stored code:

```intense
def file_directory_body
SET "module directory"
PRINT
RET
end

def main
TAPE 1
TAPE_NAME file_tape
SEEK 10
QUOTE_FUNCTION file_directory_body
SEEK 0
CALL @10
RET
end
```

`QUOTE_FUNCTION name` copies a normal `def ... end` body into the current cell as
a `Code` value, which is much more workable for larger logic than a one-line
`SET (...)` literal. The function body lives in tape cell `10`; rewriting that
cell or patching it with `CODESET` changes the next call. A named tape is still
just a tape: switch to it, position the pointer where you want execution state,
and call the code cell with `CALL @cell`.

Reusable stdlib tape loop helper:

```intense
IMPORT stdlib.intense

def main
TAPE 5
SEEK 0
SET 3
SETARG 0 5
SETARG 1 (SEEK 0 ; PRINT ; SUB 1)
CALL tape_loop
RET
end
```

`tape_loop` uses cell `0` on the passed tape as the condition/state. The body
runs on that tape and should mutate cell `0` toward a falsy value.

| Instruction          | Description |
|----------------------|-------------|
| `CALL name [args…]`  | Push frame, run a source function, function name cell, or Code value |
| `QUOTE_FUNCTION name` | Copy a `def ... end` body into the current cell as mutable Code |
| `WORKSHEET_BEGIN [name]` | Open an incognito worksheet tape for temporary reasoning |
| `POET_SEARCH [query] [limit]` | Retrieve relevant tape/cell references, including routed `address`, into the current cell |
| `POET_STRATEGY [problem]` | Select a problem-solving strategy and rational layers |
| `RATIONAL_RUN [layer]` | Produce structured evidence from a selected rational layer |
| `ROUTE_FRAME key [decision] [question]` | Define a model-level router frame in an Intense model file |
| `ROUTE_TRIGGER frame_key concept_key [strength]` | Declare which central resources activate the frame |
| `ROUTE_SLOT frame_key slot_key [question]` | Declare what missing information the frame needs before continuing |
| `ROUTE_OBSERVE [text] [memory_tape]` | Grow tape-native memory from a supplied statement |
| `ROUTE_CONTEXT [text] [memory_tape]` | Visualize what the active model understands before any solution path is chosen |
| `ROUTE_CACHE [memory_tape]` | Inspect hot/warm/cold model-memory cells for runtime cache behavior |
| `ROUTE_PACK` / `ROUTE_UNPACK` | Move route memory between active model tapes and scalar DB-storable snapshots |
| `ROUTE_REASON [text] [memory_tape]` | Optional frame dispatch when the loaded model wants to act on the context |
| `ARG index`          | Load call argument at `index` into current cell |
| `RET`                | Return from current function |
| `EXIT`               | Halt the entire running program or REPL session |

---

## Bugs Fixed

| Bug | Fix |
|-----|-----|
| `auto& tape` declared twice in `execute()` — compile error | Single declaration at top, unified if-else chain |
| `<fstream>` not included — `READFILE`/`WRITEFILE` would not compile | Added include |
| `CONCAT`/`SPLIT`/`FIND`/`REPLACE` passed raw quoted tokens as strings | All string op args now go through `parseValue` → quotes stripped |
| `LEN` printed to stdout instead of storing | Now stores `long long` in current cell |
| `CMP` printed to stdout instead of storing | Now stores `bool` in current cell |
| LRU cache hit did not update recency — effective FIFO | `std::list::splice` with iterator map gives true O(1) LRU |
| Labels looked up case-sensitively — `CALL Message` failed for `message:` | Labels normalized to uppercase in `buildIndex`; lookup normalizes name |
| No comment syntax | `#` starts a line or inline comment |
| Unknown opcodes silently ignored | Now throw `runtime_error` |

---

## File Reference

| File | Role |
|------|------|
| `Value.hpp` | `ValueKind` enum, boxed `Value` variant, `kind()`, `kindName()` |
| `Instruction.hpp` | `Instruction` struct (opcode, args, line) |
| `Tape.hpp` | Sparse tape: `unordered_map<long long, Value>` + pointer |
| `Lexer.hpp/cpp` | Tokenises a line; handles strings, chars, JSON, `#` comments |
| `Parser.hpp/cpp` | Normalises opcode to uppercase, builds `Instruction` |
| `FunctionLoader.hpp/cpp` | Lazy load + LRU-100 cache + IMPORT directive |
| `VM.hpp/cpp` | Executes instructions; all opcodes |
| `main.cpp` | CLI entry: `<file> [entry] [tapes] [--debug] [--step]` |

### Example / Test Files

| File | Tests |
|------|-------|
| `example.intense` | Basic SET, PRINT, CALL, ARG, JSON cell |
| `examples/test_types.intense` | TYPE, CAST, all ValueKind variants |
| `examples/test_ml.intense` | MEANVAL, STDDEV, NORMALIZE, ZSCORE, SOFTMAX, DOTPROD, LINEARREG, PREDICT, KMEANS |
| `examples/test_strings.intense` | CONCAT, SPLIT, FIND, REPLACE, SUBSTR, UPPER, LOWER |
| `examples/test_join_json_utils.in10s` | JOIN, JSONHAS, JSONKEYS, JSONDEL |
| `examples/test_inter_tape_comm.in10s` | TAPEREAD, TAPEWRITE, TAPESWAP, TAPESEND, TAPERECV |
| `examples/test_try_catch.in10s` | TRY, RAISE, catch Code cells |
| `examples/test_tape_modules.in10s` | Tape names, `QUOTE_FUNCTION`, direct Code-cell calls, code patching |
| `examples/test_emotional_tape.in10s` | Hormone tape levels, triggers, combo events, refractory blocks, shutdown |
| `examples/test_poet_hrm.in10s` | Incognito worksheet, tape search, strategy selection, rational evidence, hormone field drift |
| `examples/test_arithmetic.intense` | ADD, SUB, MUL, DIV, MOD, int/float promotion, `@N` cell references |
| `examples/example_json.intense` | JSON object/array storage |
| `examples/example_import.intense` | IMPORT directive |
| `examples/example_params.intense` | ARG parameter passing |
