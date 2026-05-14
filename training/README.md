# Training And Memory

The active memory direction is tape-native Postgres storage. Postgres is used as
a persisted replica of VM tape cells; reasoning and thought still happen at
runtime inside the tape VM.

## Active Memory Model

`training/common_memory/postgres_schema.sql` defines:

```text
tape_spaces  named memory/run namespaces
tape_cells   latest scalar/code cell values keyed by space, tape index, and cell index
tape_events  runtime reasoning/process markers
```

The table model intentionally avoids JSON/JSONB and avoids graph-style
`nodes`/`edges`. Nested structures should be encoded across tape cells instead
of being stored as JSON. Programs should load only the required persisted
tapes/cells, process them in the VM, then store the resulting tape cells back to
Postgres.

## VM Opcodes

Postgres tape memory uses the regular connection opcodes plus:

```text
DB_TAPE_OPEN space [kind]
DB_TAPE_INPUT space [tape] [cell] [value]
DB_TAPE_OUTPUT space [tape] [cell]
DB_TAPE_SAVE space [tape|*]
DB_TAPE_LOAD space [tape|*]
DB_TAPE_EVENT space event_type [note]
```

See `test_cases/` for the current tape-native test cases.

## POET Training Driver

`poet_memory_training.in10s` wraps `poet_algorithm_v1.in10s` with tape-native
Postgres persistence:

```text
./intense.out training/poet_memory_training.in10s main 40
```

It seeds sample problems into `poet_training_samples`, reloads only the required
sample tape for each run, analyzes the input with NLP tape operations, calls
`poet_solve`, stores scalar result/feedback cells in `poet_training_run`, and
then retrieves a stored answer.

For a no-database POET smoke check:

```text
./intense.out training/poet_memory_training.in10s smoke_no_db 8
```

## Legacy Poet Flow

The old JSON common-memory flow around `run_amu_epoch.sh` is legacy. The active
training path is the tape-native Postgres driver above; `poet_algorithm_v1.in10s`
is kept as the direct solver it calls, without depending on deleted JSON memory
files.
