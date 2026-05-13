CREATE TABLE IF NOT EXISTS poet_memory_events (
    id BIGSERIAL PRIMARY KEY,
    source TEXT NOT NULL DEFAULT 'user',
    input_text TEXT NOT NULL,
    output_text TEXT,
    route TEXT,
    confidence DOUBLE PRECISION,
    context JSONB NOT NULL DEFAULT '{}'::jsonb,
    generated_logic TEXT,
    feedback TEXT,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS poet_memory_links (
    id BIGSERIAL PRIMARY KEY,
    from_event_id BIGINT REFERENCES poet_memory_events(id) ON DELETE CASCADE,
    to_event_id BIGINT REFERENCES poet_memory_events(id) ON DELETE CASCADE,
    relation TEXT NOT NULL,
    strength DOUBLE PRECISION NOT NULL DEFAULT 1.0,
    note TEXT,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS poet_memory_events_context_gin
    ON poet_memory_events USING gin (context);

CREATE INDEX IF NOT EXISTS poet_memory_links_from_idx
    ON poet_memory_links (from_event_id);

CREATE INDEX IF NOT EXISTS poet_memory_links_to_idx
    ON poet_memory_links (to_event_id);

CREATE TABLE IF NOT EXISTS logic_nodes (
    node_key TEXT PRIMARY KEY,
    kind TEXT NOT NULL,
    payload JSONB NOT NULL DEFAULT '{}'::jsonb,
    confidence DOUBLE PRECISION NOT NULL DEFAULT 1.0,
    source_event_id BIGINT REFERENCES poet_memory_events(id) ON DELETE SET NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS logic_edges (
    id BIGSERIAL PRIMARY KEY,
    source_key TEXT NOT NULL REFERENCES logic_nodes(node_key) ON DELETE CASCADE,
    relation TEXT NOT NULL,
    target_key TEXT NOT NULL REFERENCES logic_nodes(node_key) ON DELETE CASCADE,
    weight DOUBLE PRECISION NOT NULL DEFAULT 1.0,
    logic_weight JSONB NOT NULL DEFAULT '{"state":"assumed","support":"unverified","scope":"general"}'::jsonb,
    validity TEXT NOT NULL DEFAULT 'active',
    evidence JSONB NOT NULL DEFAULT '{}'::jsonb,
    generated_logic TEXT,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (source_key, relation, target_key)
);

ALTER TABLE logic_edges
    ADD COLUMN IF NOT EXISTS logic_weight JSONB NOT NULL DEFAULT '{"state":"assumed","support":"unverified","scope":"general"}'::jsonb;

ALTER TABLE logic_edges
    ADD COLUMN IF NOT EXISTS validity TEXT NOT NULL DEFAULT 'active';

ALTER TABLE logic_edges
    ADD COLUMN IF NOT EXISTS updated_at TIMESTAMPTZ NOT NULL DEFAULT now();

CREATE INDEX IF NOT EXISTS logic_nodes_kind_idx
    ON logic_nodes (kind);

CREATE INDEX IF NOT EXISTS logic_nodes_payload_gin
    ON logic_nodes USING gin (payload);

CREATE INDEX IF NOT EXISTS logic_edges_source_idx
    ON logic_edges (source_key);

CREATE INDEX IF NOT EXISTS logic_edges_target_idx
    ON logic_edges (target_key);

CREATE INDEX IF NOT EXISTS logic_edges_relation_idx
    ON logic_edges (relation);

CREATE INDEX IF NOT EXISTS logic_edges_logic_weight_gin
    ON logic_edges USING gin (logic_weight);

CREATE TABLE IF NOT EXISTS reasoning_routes (
    route_key TEXT PRIMARY KEY,
    intent TEXT NOT NULL,
    matcher JSONB NOT NULL DEFAULT '{}'::jsonb,
    retrieval_relation TEXT,
    answer_path JSONB NOT NULL DEFAULT '{}'::jsonb,
    generated_logic_template TEXT,
    logic_weight JSONB NOT NULL DEFAULT '{"state":"assumed","support":"route_seed","scope":"general"}'::jsonb,
    status TEXT NOT NULL DEFAULT 'active',
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS reasoning_routes_intent_idx
    ON reasoning_routes (intent);

CREATE INDEX IF NOT EXISTS reasoning_routes_matcher_gin
    ON reasoning_routes USING gin (matcher);

CREATE TABLE IF NOT EXISTS logic_feedback (
    id BIGSERIAL PRIMARY KEY,
    edge_id BIGINT REFERENCES logic_edges(id) ON DELETE CASCADE,
    route_key TEXT REFERENCES reasoning_routes(route_key) ON DELETE SET NULL,
    feedback TEXT NOT NULL,
    previous_logic_weight JSONB,
    new_logic_weight JSONB NOT NULL,
    reason TEXT,
    source TEXT NOT NULL DEFAULT 'trainer',
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS logic_feedback_edge_idx
    ON logic_feedback (edge_id);

CREATE TABLE IF NOT EXISTS logic_contradictions (
    id BIGSERIAL PRIMARY KEY,
    subject_key TEXT NOT NULL,
    relation TEXT NOT NULL,
    edge_a BIGINT REFERENCES logic_edges(id) ON DELETE CASCADE,
    edge_b BIGINT REFERENCES logic_edges(id) ON DELETE CASCADE,
    conflict JSONB NOT NULL DEFAULT '{}'::jsonb,
    resolution_state TEXT NOT NULL DEFAULT 'unresolved',
    resolution_logic JSONB NOT NULL DEFAULT '{}'::jsonb,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS logic_contradictions_subject_relation_idx
    ON logic_contradictions (subject_key, relation);

CREATE TABLE IF NOT EXISTS logic_edge_history (
    id BIGSERIAL PRIMARY KEY,
    edge_id BIGINT REFERENCES logic_edges(id) ON DELETE CASCADE,
    action TEXT NOT NULL,
    old_logic_weight JSONB,
    new_logic_weight JSONB,
    note TEXT,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

INSERT INTO reasoning_routes
    (route_key, intent, matcher, retrieval_relation, answer_path, generated_logic_template, logic_weight, status)
VALUES
    (
        'symbolic_fact_lookup',
        'answer_lookup',
        '{"requires":["subject","relation"],"candidate_kind":["question","fact"]}'::jsonb,
        null,
        '{"target_payload_key":"value","fallback_payload_key":"name"}'::jsonb,
        'SET {{answer}}',
        '{"state":"confirmed","support":"seed_route","scope":"general"}'::jsonb,
        'active'
    ),
    (
        'symbolic_contrast',
        'explain_difference',
        '{"requires":["subject","contrast_target"],"candidate_kind":["question"]}'::jsonb,
        'contrast',
        '{"target_payload_key":"name"}'::jsonb,
        'SET {{explanation}}',
        '{"state":"assumed","support":"seed_route","scope":"general"}'::jsonb,
        'active'
    )
ON CONFLICT (route_key) DO NOTHING;
