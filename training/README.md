# Poet Training

The training folder now uses a single common-memory model instead of many
`training/AMU*` work folders.

## Current Flow

```text
training/common_memory/test_cases.tsv
  -> training/run_amu_epoch.sh
  -> writes one temporary runner per test case
  -> runner imports training/poet_algorithm_v1.in10s
  -> Poet answers directly from common memory or reusable routes
  -> Poet writes a generated candidate algorithm for that input
  -> script executes the candidate and records pass/fail output
  -> all artifacts land under training/common_memory/generated_outputs/latest/
```

## Common Memory

`training/common_memory/` is the durable model/memory area:

```text
facts.json                 learned/common facts
algorithm_templates.json   reusable route names and intent
training_examples.json     reserved learned-example store
test_cases.tsv             test inputs and expected outputs
postgres_schema.sql        optional Postgres schema for event/link and logic-network memory
generated_outputs/latest/  latest generated runners, candidates, traces, report
```

The VM also has optional PostgreSQL opcodes: `DBCONNECT`, `DBSTATUS`, `DBEXEC`,
`DBQUERY`, `DBSELECT`, `DBINSERT`, `DBUPDATE`, `DBDELETE`, and `DBCLOSE`. They
dynamically load `libpq` at runtime, so the VM still builds without Postgres
installed. See `examples/postgres_memory.in10s`.

## Postgres Logic Network

`postgres_schema.sql` now has two layers:

- `poet_memory_events` and `poet_memory_links`: time-ordered observations,
  outputs, feedback, and event-to-event links.
- `logic_nodes` and `logic_edges`: stable concept nodes and weighted
  relationships that Poet can retrieve as reasoning context.

For the local AMU database:

```bash
./intense.out examples/test_db_amu_logic_network.in10s main 4
```

That example stores concepts like `cat`, `human`, `legs`, `count_4`, and edges
such as `cat -> legs_count -> count_4`. The final query returns the retrieved
answer path and generated logic (`SET 4`).

Generated Poet-code proposals are stored under
`training/common_memory/generated_outputs/latest/proposed_poet/`. They are not
promoted automatically; a future trainer can test and approve them.

## Run

```bash
training/run_amu_epoch.sh
training/run_amu_epoch.sh training/common_memory/test_cases.tsv
```

The script produces:

```text
training/common_memory/generated_outputs/latest/
  candidates/
  runners/
  traces/
  proposed_poet/
  report.tsv
  training_examples.tsv
  summary.txt
```

## Test

```bash
training/test_output_amu.sh
```

This runs the current test cases and exits non-zero if any case fails.

For a small direct smoke test:

```bash
./intense.out training/train_epoch.in10s main 8
```

## Poet Tape Layout

`poet_algorithm_v1.in10s` keeps concerns on separate tapes:

| Tape | Role |
|------|------|
| 0 | Raw and normalized problem |
| 1 | Common memory and route knowledge |
| 2 | Token/NLP analysis |
| 3 | Generated candidate code/source/metadata |
| 4 | Direct answer, route metadata, execution result |
| 5 | Scratch |
