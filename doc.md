> Agent context: see [CLAUDE.md](CLAUDE.md) for current implementation state, architecture, and completed work.

Design a C++ interpreter for a symbolic multi-tape computational machine inspired by Turing machines, but NOT based on finite-state transitions or CPU/VM architecture.

Foundational Identity
---------------------
This system must always be understood as a modified symbolic Turing machine, not as a conventional VM.

Implementation files may use names such as `VM` for historical or practical reasons, but the conceptual machine is:
- a symbolic tape machine
- a modified Turing machine
- a tape-rewrite computation system

It does NOT contain:
- registers
- accumulators
- bytecode execution
- binary-code execution
- CPU-style fetch/decode/execute semantics
- separate program memory versus data memory

It accepts symbolic values on tapes and produces results by transforming tapes.

The output of computation is the resulting tape state:
- current cell contents
- mutated tape regions
- generated symbolic values
- executable or quoted code stored on tape
- analysis results stored back into tape cells

Every feature, including math, ML, NLP, file I/O, and reflective execution, must preserve this rule:

> external libraries may accelerate or enrich transformations, but they must act as services behind symbolic tape instructions; they must never redefine the machine as a register VM, bytecode runtime, or conventional programming language runtime.

Core Philosophy
----------------
This machine is NOT:
- a CPU
- a bytecode VM
- a register machine
- a traditional programming language runtime

Instead:
- computation fully lives on tapes
- tapes are the only computational substrate
- there are no registers or separate memory models
- algorithms directly inspect and mutate tape cells
- movement across tapes is part of computation itself

The machine is fundamentally:
- tape-oriented
- symbolic
- homoiconic
- algorithmic
- reflective

Conceptual Model
----------------
Classical Turing machines use:
(state, symbol) -> (new_state, new_symbol, movement)

This machine replaces:
- finite states
- transition tables

with:
- explicit symbolic algorithms

Algorithms directly perform:
- tape mutations
- symbolic rewrites
- movement/navigation
- algorithm evaluation

Tape Cells
-----------
Tape cells are NOT binary symbols.

Each tape cell may contain:
- strings
- integers
- lists
- symbolic structures
- nested objects
- executable instructions
- executable algorithm blocks

Example cell contents:
- "hello world"
- [1,2,3]
- {key:value}
- [MOVE 10, SET "x"]
- [COMPARE tape1]
- symbolic executable forms

The tape is homoiconic:
- instructions are data
- data can become executable
- algorithms can inspect and modify algorithms
- tape cells may contain executable symbolic structures

Execution Model
---------------
Execution works by:
1. inspecting symbolic tape cells
2. evaluating symbolic structures
3. mutating tapes
4. navigating tapes
5. continuing symbolic execution

There are:
- no finite states
- no CPU-like fetch/decode cycle
- no register system

Algorithms
-----------
Algorithms are groups of symbolic tape-mutating instructions.

Functions are NOT procedures.
Functions are:
- named algorithmic regions
- symbolic transformation groups

Example syntax:
main:
    SET "hello"
    MOVE 10
    CALL analyze
    RET

Instructions
-------------
Core instructions:
- MOVE n
- BACK n
- TAPE n
- SET value
- COPY
- COMPARE
- CALL label
- RET
- EXEC
- EVAL
- QUOTE
- MATCH
- APPEND
- DELETE
- LENGTH

Instructions themselves may be stored inside tape cells.

Example:
Cell may contain:
[MOVE 10, SET "generated"]

Another algorithm may later EXEC or EVAL that cell.

Multiple Tapes
--------------
Support multiple sparse tapes.

Algorithms can:
- switch tapes
- inspect other tapes
- compare tape regions
- move symbolic structures between tapes

Tape movement itself is computational semantics.

Memory Model
------------
Use sparse tape allocation:
- unordered_map<long long, Cell>

DO NOT preallocate large arrays.

Cells are symbolic boxed values.

Support:
- lazy allocation
- symbolic objects
- nested structures
- executable symbolic forms

Homoiconicity
--------------
The system must support:
- algorithms as data
- self-modifying symbolic structures
- symbolic rewrite operations
- runtime algorithm generation
- reflective execution

Need explicit distinction between:
- executable forms
- quoted data

Support:
- QUOTE
- EXEC
- EVAL

Lazy Function Loading
---------------------
Functions/algorithm regions should NOT all load into memory.

Requirements:
- build label index only initially
- load functions on demand
- cache max 100 functions
- use LRU eviction
- avoid huge memory usage

Function labels:
label:

Store:
unordered_map<string, streampos>

Parser
------
Use:
- line-based syntax
- assembly/Lua/Ruby style

NO semicolons required.

One instruction per line.

Labels use:
label:

Example:
main:
    SET "hello"
    PRINT
    CALL analyze

Execution Architecture
----------------------
Interpreter should:
- tokenize line-by-line
- parse symbolic instructions
- evaluate symbolic tape structures
- support reflective execution

DO NOT model:
- CPU instructions
- finite-state transitions
- hardware stacks

Debugging
---------
Include debug mode showing:
- current algorithm region
- active tape
- pointer position
- symbolic cell contents
- tape sizes
- currently evaluated structure
- cache usage
- reflective execution traces

Implementation Language
-----------------------
Implement the interpreter in modern C++17 or newer.

Project Requirements
--------------------
Provide:
- modular project structure
- parser
- lexer
- tape system
- symbolic value system
- lazy function loader
- evaluator/interpreter
- debug mode
- example symbolic programs

The final architecture should resemble:
- a symbolic reflective tape machine
- a homoiconic multi-tape computational substrate
- a symbolic rewrite-oriented execution system

NOT:
- a normal VM
- a CPU emulator
- a standard programming language runtime

Yes — now the model becomes much deeper and more unique.

You’ve introduced:

> homoiconicity on the tape itself.

That changes the machine fundamentally.

---

# 🔹 What changed conceptually

Previously:

```text id="jlwm51"
cells = data
algorithms = external instructions
```

Now:

```text id="jlwm52"
cells can ALSO contain algorithms
```

Meaning:

* instructions are data
* data can become executable
* algorithms can inspect/modify algorithms

This is exactly the core idea behind:

* homoiconic systems
* symbolic rewriting systems
* Lisp-like computation

---

# 🔹 Your machine is now self-descriptive

A tape cell may contain:

```text id="jlwm53"
SET \"hello\"
```

or:

```text id="jlwm54"
[
  MOVE 10,
  SET \"x\",
  CALL analyze
]
```

The algorithm can:

* inspect it
* modify it
* copy it
* execute it
* generate new algorithms dynamically

This is a massive shift.

---

# 🔹 The tape is no longer “memory”

It becomes:

```text id="jlwm55"
active symbolic substrate
```

where:

* data
* instructions
* algorithms
* symbolic structures

all coexist uniformly.

---

# 🔹 This is true homoiconicity

In a homoiconic system:

> program structure has the same representation as data.

Exactly what you described.

---

# 🔹 Example

A tape cell:

```text id="jlwm56"
[
  \"COMPARE\",
  \"tape1\"
]
```

is simultaneously:

* data
* executable symbolic instruction

An algorithm can mutate it into:

```text id="jlwm57"
[
  \"COMPARE\",
  \"tape2\"
]
```

during execution.

---

# 🔹 Very important implication

You no longer really have:

```text id="jlwm58"
program vs memory
```

Instead:

```text id="jlwm59"
everything is symbolic tape content
```

This is extremely different from:

* CPUs
* VMs
* compiled systems

---

# 🔹 Your “functions” now

Functions are no longer:

```text id="jlwm60"
stored code blocks
```

They become:

```text id="jlwm61"
symbolic algorithm regions on tape
```

which may themselves:

* mutate
* generate other algorithms
* move between tapes
* rewrite themselves

---

# 🔹 The interpreter role changes completely

Previously interpreter did:

```text id="jlwm62"
execute predefined instructions
```

Now interpreter becomes:

```text id="jlwm63"
symbolic evaluator over tape structures
```

Huge difference.

---

# 🔹 Execution model now

Execution becomes:

```text id="jlwm64"
inspect current symbolic cell
↓
interpret structure
↓
apply transformation
↓
mutate tape
↓
continue
```

instead of:

```text id="jlwm65"
fetch opcode
```

---

# 🔹 Tape cells now need types like

| Type        | Meaning         |
| ----------- | --------------- |
| VALUE       | symbolic data   |
| INSTRUCTION | executable op   |
| BLOCK       | algorithm group |
| LABEL       | symbolic region |
| PATTERN     | matching rule   |

---

# 🔹 Most important new capability

Your machine now supports:

## 🔸 Self-modifying symbolic computation

Example:

Algorithm writes new algorithm:

```text id="jlwm66"
SET [MOVE 10, SET \"generated\"]
```

Another algorithm later executes it.

This is VERY powerful.

---

# 🔹 You are approaching rewrite-system computation

Your architecture is now closest to:

| System                     | Similarity     |
| -------------------------- | -------------- |
| Lisp                       | very high      |
| symbolic rewriting systems | extremely high |
| graph reduction systems    | high           |
| cellular symbolic automata | high           |
| reflective systems         | high           |

---

# 🔹 Important distinction from Lisp

In Lisp:

```text id="jlwm67"
lists represent code
```

In your system:

```text id="jlwm68"
tape cells represent symbolic executable regions
```

So:

* spatial structure matters
* movement matters
* tape topology matters

That is unique.

---

# 🔹 MOVE/BACK become semantic navigation

Movement is no longer:

```text id="jlwm69"
memory access
```

It becomes:

```text id="jlwm70"
navigation across symbolic computational space
```

That is a deep conceptual difference.

---

# 🔹 This means your machine now needs

## 🔸 1. Evaluator semantics

Need explicit rule:

```text id="jlwm71"
when is a symbolic cell executed?
```

Possible:

* explicit EXEC
* implicit execution
* pattern-triggered execution

---

# 🔸 2. Quoting system

Need distinction between:

```text id="jlwm72"
data
```

and:

```text id="jlwm73"
executable symbolic form
```

Like Lisp quoting.

Example:

```text id="jlwm74"
QUOTE [MOVE 10]
```

Otherwise everything becomes executable accidentally.

---

# 🔸 3. Pattern matching

VERY important now.

Example:

```text id="jlwm75"
MATCH [SET x]
```

because symbolic algorithms become inspectable structures.

---

# 🔸 4. Rewrite semantics

Need rules for:

* replacing algorithm fragments
* transforming symbolic blocks
* composing algorithms

---

# 🔸 5. Execution control

Need operations like:

| Instruction | Purpose               |
| ----------- | --------------------- |
| `EXEC`      | execute symbolic cell |
| `EVAL`      | interpret structure   |
| `QUOTE`     | treat as data         |
| `UNQUOTE`   | activate              |

These become fundamental now.

---

# 🔹 Extremely important insight

You are no longer designing:

* a language
* a VM
* a CPU abstraction

You are designing:

> a reflective symbolic tape computation system

where:

* computation lives inside symbolic tape structures
* algorithms are manipulable tape entities
* execution itself is symbolic transformation

That is much closer to:

* reflective rewriting systems
* symbolic AI substrates
* homoiconic computational spaces

than to normal programming languages.

---

# 🔹 Final conceptual model

Your machine now consists of:

| Component  | Meaning                            |
| ---------- | ---------------------------------- |
| tapes      | symbolic computational space       |
| cells      | symbolic executable/data units     |
| algorithms | rewrite/navigation processes       |
| movement   | symbolic spatial traversal         |
| functions  | named algorithmic tape regions     |
| execution  | symbolic evaluation/transformation |

That is a genuinely distinct computational model.



Great strategic thinking! Using an established library would let you focus on the Intense VM itself rather than reinventing ML. Here are your main C++ options:

**Popular Choices:**

| Library | Focus | Ease | Notes |
|---------|-------|------|-------|
| **Eigen** | Linear algebra | Easy | Header-only, super fast, perfect for math ops; perfect fit for DOTPROD, matrix ops |
| **OpenCV** | Vision + some ML | Medium | Heavy; includes clustering, regression, but overkill if you just need math |
| **MLpack** | General ML | Medium | Algorithms in C++, clean API; supports regression, clustering, etc. |
| **TensorFlow C++** | Deep learning | Hard | Heavy dependency; overkill unless you want neural nets |
| **Dlib** | Lightweight ML | Easy | Single header option, includes clustering, regression; smaller footprint |

**For your current feature set** (KMEANS, LINEARREG, PREDICT, DOTPROD, stats), **Eigen is the natural choice** because:
- You'd implement clustering/regression on top of it (still learning value, not black-box)
- Lightweight, header-only or minimal linking
- Integrates cleanly with your tape-oriented architecture
- Stats ops (MEANVAL, STDDEV, NORMALIZE, ZSCORE, SOFTMAX) map directly to Eigen operations

**High-performance recommendation:**

Use **Eigen** as the numerical substrate, but keep the Intense VM semantics tape-native.

The VM should still expose operations as symbolic tape instructions:

```intense
SET [1,2,3]
DOTPROD 1
```

Internally, numeric list-like tape values can be converted into Eigen vectors/matrices for fast execution, then converted back into symbolic `Value` cells. This preserves the core model:

- tapes remain the computational space
- cells remain symbolic values
- ML/math instructions remain symbolic rewrites
- Eigen is only an acceleration layer, not the computational model

For high-performance targets, the best path is:

1. Keep simple scalar/list operations custom for clarity.
2. Use Eigen for dense vector and matrix operations.
3. Add conversion helpers between `Value::List` and Eigen vectors/matrices.
4. Keep algorithms like `KMEANS`, `LINEARREG`, and future matrix ops visible at the VM level.
5. Avoid heavyweight ML frameworks unless Intense grows into deep learning or model training.

This gives the project speed without turning the VM into a normal ML runtime.
