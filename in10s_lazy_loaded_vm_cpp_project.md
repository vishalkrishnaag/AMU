# Intense — Lazy Loaded Symbolic Tape VM (C++ Project)

A symbolic tape-oriented virtual machine inspired by Turing machines, Brainfuck minimalism, stack VMs, and symbolic execution engines.

> Agent context: see [CLAUDE.md](CLAUDE.md) for architecture summary, design decisions, and bug history.

---

## Quick Start

```bash
make
./intense.out <file.intense> [entry=main] [tapes=4] [--debug] [--step]
```

Examples:
```bash
./intense.out example.intense
./intense.out examples/test_ml.intense main 8
./intense.out examples/test_types.intense main --debug
```

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

### 4 — Labels define functions
A line ending with `:` defines a label (function entry point). Labels are case-insensitive.

```
my_function:
    SET 99
    PRINT
    RET
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

### 7 — Functions, CALL, and RET
```
CALL function_name [arg0] [arg1] ...
```
Arguments are parsed as Values. Access them inside the function with `ARG <index>`. `RET` returns to the caller.

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
| `TAPE n`    | Switch active tape to index `n` |

### Cell Operations

| Instruction    | Description |
|----------------|-------------|
| `SET value`    | Write a value into the current cell |
| `PRINT`        | Print the current cell (human-readable) |
| `PRINTJ`       | Print the current cell as JSON |
| `LEN`          | Store the cell count of the current tape into the current cell |
| `CMP n`        | Compare current cell with tape `n`'s current cell; store `bool` result |

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

**Cell reference syntax:** prefix the argument with `@` followed by an integer cell index.
The index refers to the **active tape** (use `TAPE n` first for cross-tape reads).

```intense
main:
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
```

### String Operations

| Instruction        | Description |
|--------------------|-------------|
| `CONCAT value`     | Append `value` (stringified) to current cell |
| `SPLIT delim`      | Split current string by delimiter; store List of Str |
| `SUBSTR start len` | Extract substring from current Str cell |
| `FIND sub`         | Store 0-based index of first occurrence, or −1 |
| `REPLACE old new`  | Replace all occurrences of `old` with `new` in current Str |
| `UPPER`            | Convert current Str to uppercase in place |
| `LOWER`            | Convert current Str to lowercase in place |

### File I/O

| Instruction        | Description |
|--------------------|-------------|
| `READFILE path`    | Read entire file into current cell as Str |
| `WRITEFILE path`   | Write current cell (stringified) to file |

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

### Function Calls

| Instruction          | Description |
|----------------------|-------------|
| `CALL name [args…]`  | Push frame, run `name`, pop frame |
| `ARG index`          | Load call argument at `index` into current cell |
| `RET`                | Return from current function |

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
| `examples/test_arithmetic.intense` | ADD, SUB, MUL, DIV, MOD, int/float promotion, `@N` cell references |
| `examples/example_json.intense` | JSON object/array storage |
| `examples/example_import.intense` | IMPORT directive |
| `examples/example_params.intense` | ARG parameter passing |
