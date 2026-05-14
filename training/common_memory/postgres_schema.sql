CREATE TABLE IF NOT EXISTS tape_spaces (
    space_key TEXT PRIMARY KEY,
    kind TEXT NOT NULL DEFAULT 'memory',
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS tape_cells (
    space_key TEXT NOT NULL REFERENCES tape_spaces(space_key) ON DELETE CASCADE,
    tape_index BIGINT NOT NULL CHECK (tape_index >= 0),
    cell_index BIGINT NOT NULL,
    value_kind TEXT NOT NULL CHECK (
        value_kind IN ('nil', 'bool', 'int', 'float', 'char', 'str', 'code')
    ),
    bool_value BOOLEAN,
    int_value BIGINT,
    float_value DOUBLE PRECISION,
    char_value TEXT,
    text_value TEXT,
    code_text TEXT,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (space_key, tape_index, cell_index)
);

CREATE INDEX IF NOT EXISTS tape_cells_space_tape_idx
    ON tape_cells (space_key, tape_index);

CREATE INDEX IF NOT EXISTS tape_cells_space_cell_idx
    ON tape_cells (space_key, tape_index, cell_index);

CREATE TABLE IF NOT EXISTS tape_events (
    id BIGSERIAL PRIMARY KEY,
    space_key TEXT NOT NULL REFERENCES tape_spaces(space_key) ON DELETE CASCADE,
    event_type TEXT NOT NULL,
    tape_index BIGINT NOT NULL CHECK (tape_index >= 0),
    cell_index BIGINT NOT NULL,
    opcode TEXT,
    note TEXT,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS tape_events_space_created_idx
    ON tape_events (space_key, created_at);

CREATE INDEX IF NOT EXISTS tape_events_space_tape_idx
    ON tape_events (space_key, tape_index, cell_index);
