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
generated_outputs/latest/  latest generated runners, candidates, traces, report
```

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
