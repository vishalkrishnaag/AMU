# Tape-Native Test Cases

This folder holds the new tape-memory test cases. The older `examples/` folder is
kept as VM opcode smoke/demo material and is not the working area for the
tape-native Postgres model.

## Local Tests

```bash
./intense.out test_cases/test_dynamic_tapes.in10s main 4
```

## Postgres Tests

These tests expect a local Postgres connection string in the first `SET` cell.
They create the tape-native schema from `training/common_memory/postgres_schema.sql`.

```bash
./intense.out test_cases/test_db_tape_memory.in10s main 4
./intense.out test_cases/test_db_tape_load_required_tapes.in10s main 4
```

The Postgres memory model is a tape replica:

- `tape_spaces` names a memory/run namespace.
- `tape_cells` stores latest scalar/code cell values by tape and cell index.
- `tape_events` records runtime process markers.
