# PostgreSQL VM Examples

These files are small runnable test cases for the Intense VM database opcodes.

## No database required

```bash
./intense.out examples/test_db_status.in10s main 4
```

Expected result: a JSON object showing the PostgreSQL backend and
`connected:false`.

## Database required

The remaining examples use this default connection string:

```text
host=localhost port=5432 dbname=intense user=intense password=intense
```

Edit the first `SET` line in each file if your local PostgreSQL role/database is
different.

```bash
./intense.out examples/test_db_raw_sql_from_tape.in10s main 4
./intense.out examples/test_db_memory_crud.in10s main 4
./intense.out examples/test_db_poet_memory_schema.in10s main 4
./intense.out examples/postgres_memory.in10s main 4
```

For the local AMU database configured as `dbname=AMU user=postgres password=postgres`:

```bash
./intense.out examples/test_db_amu_logic_network.in10s main 4
```

## What each file tests

- `test_db_status.in10s`: `DBSTATUS` without an active connection.
- `test_db_raw_sql_from_tape.in10s`: `DBCONNECT @cell`, `DBQUERY @cell`, `DBCLOSE`.
- `test_db_memory_crud.in10s`: `DBEXEC`, `DBINSERT`, `DBUPDATE`, `DBSELECT`, `DBDELETE`.
- `test_db_poet_memory_schema.in10s`: memory event/link tables for Poet-style relational memory.
- `postgres_memory.in10s`: compact end-to-end memory example.
- `test_db_amu_logic_network.in10s`: concept-node and weighted-edge network for reasoning memory.
