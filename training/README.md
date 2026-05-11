# AMU Poet Training

AMU means **Automata + Memory + Universe**:

- **Automata**: one Intense modified Turing machine process.
- **Memory**: that AMU's generated algorithms, traces, and learned artifacts.
- **Universe**: the input/problem/data world given to that AMU.

The C++ trainer owns orchestration. It creates many AMU folders, launches
Intense interpreter processes in parallel, gives each AMU the same or varied
universe, tests the generated algorithm, and merges qualified memory into an
output AMU.

## Current Flow

```text
training/trainer.cpp
  -> creates training/AMU1, AMU2, ...
  -> writes AMU-local poet_runner.in10s
  -> runs ./intense.out training/AMU*/poet_runner.in10s
  -> poet_runner imports training/poet_algorithm.in10s
  -> poet discriminates the universe using instruction knowledge and analysis tapes
  -> poet writes candidate algorithm source from its generated-logic tape into AMU memory
  -> trainer executes candidate in a second interpreter process
  -> trainer selects AMUs whose candidate output matches expected feedback
  -> trainer merges winner memory into training/output_AMU
```

## Poet Tape Layout

`poet_algorithm.in10s` keeps major concerns on separate tapes so new data does
not overwrite the universe or generated logic:

| Tape | Role |
|------|------|
| 0 | Universe/input data |
| 1 | Intense instruction knowledge and generation instincts |
| 2 | Discriminated analysis/features |
| 3 | Generated algorithm Code, source text, and metadata |
| 4 | Execution result and feedback state |
| 5 | Scratch |

This is the first version of "logic creates logic": the poet reads the universe,
chooses Intense instructions from its knowledge tape, writes a candidate program
to tape 3, executes a copy on tape 4, and the trainer decides whether that logic
needs another training epoch.

## Run

```bash
training/run_amu_epoch.sh [AMU_COUNT] [WORKER_THREADS]
```

Both arguments are dynamic. If omitted, the runner uses the detected online CPU
count for both AMU count and worker threads.

Examples:

```bash
# Low-end CPU: train two AMUs, one at a time.
training/run_amu_epoch.sh 2 1

# High-end CPU: train 800 AMUs with 64 concurrent workers.
training/run_amu_epoch.sh 800 64
```

Manual compile:

```bash
g++ -std=c++17 -O2 -pthread training/trainer.cpp -o training/trainer.out
training/trainer.out [AMU_COUNT] [WORKER_THREADS]
```

`AMU_COUNT` is how many AMU universes participate in the epoch. `WORKER_THREADS`
is the concurrency cap, so a low-end machine can keep it small while a high-end
machine can run many AMUs in parallel.

## Test After Training

```bash
training/test_output_amu.sh [AMU_COUNT] [WORKER_THREADS] [MAX_RETRIES]
```

The tester compiles `training/tester.cpp`, executes every candidate algorithm in
`training/output_AMU/memory/generated_algorithms/`, and checks whether any
candidate produces the expected output. If the output AMU is not valid, it runs
another training epoch and tests again until `MAX_RETRIES` is exhausted.

Example:

```bash
training/test_output_amu.sh 8 4 2
```

This tests the current output AMU, and if it fails, retrains with 8 AMUs and 4
worker threads for up to 2 retry epochs.

## Folder Layout

```text
training/
  AMU1/
    poet_runner.in10s
    memory/generated_algorithms/
    traces/
    score.txt
  AMU2/
    ...
  output_AMU/
    memory/generated_algorithms/
    manifest.txt
    test_report.txt
    test_traces/
```

## Present Boundary

`poet_algorithm.in10s` can generate algorithm source and executable Code cells.
The trainer supplies the universe and validates candidates. Later, this folder
memory can be replaced by Postgres or ClickHouse without changing the poet's
basic contract: input universe in, candidate algorithm out, feedback selects
memory.

Self-modification should be introduced carefully. The safe first step is for an
AMU to write a mutated copy of `poet_algorithm.in10s` into its own memory, then
let the C++ trainer run that copy in a future epoch. Directly rewriting the
shared source while parallel AMUs are running would destroy isolation.
