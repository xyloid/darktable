<!-- llm-wiki-schema -->
# Darktable Codebase Companion — LLM Wiki Schema

A knowledge base about the darktable source code — architecture, subsystems,
processing pipelines, and hard-won gotchas — built up incrementally while
working on the codebase (which lives one directory up, at the repo root).

This directory is an LLM Wiki. The user curates sources in `raw/`, directs
the analysis, and asks questions. You (the LLM) write and maintain everything
under `wiki/`. You never modify anything in `raw/`.

## Structure

- `raw/` — immutable source documents. Read-only. The source of truth.
- `raw/assets/` — images downloaded from sources.
- `wiki/index.md` — catalog of every wiki page: link + one-line summary,
  organized by category. Update it on every page create/rename/delete.
- `wiki/log.md` — append-only chronological record of operations.
- `wiki/sources/` — one summary page per ingested source.
- `wiki/subsystems/` — major modules and code areas (iop modules, pixelpipe,
  library/database, GUI, OpenCL layer, ...).
- `wiki/concepts/` — domain ideas the code implements: color science,
  demosaicing, masks, wavelets, and similar.
- `wiki/pipelines/` — data-flow walkthroughs: how an image or event moves
  through the code end to end.
- `wiki/gotchas/` — traps, invariants, and non-obvious constraints learned
  the hard way (thread-safety rules, buffer layouts, ordering requirements).

## Conventions

- Link wiki pages to each other with `[[wikilinks]]`. When page A
  substantially builds on page B, link both directions.
- Every wiki page starts with YAML frontmatter:

  ```yaml
  ---
  type: source | subsystem | concept | pipeline | gotcha
  tags: []
  created: YYYY-MM-DD
  updated: YYYY-MM-DD
  sources: [raw/<file>]   # raw files this page draws on
  ---
  ```

- Log entry format: `## [YYYY-MM-DD] <op> | <title>` where `<op>` is one of
  `init`, `ingest`, `query`, `lint`, `file`. (`query` is reserved; filed
  query answers are logged as `file`.) Entries are append-only.
  (`grep "^## \[" wiki/log.md | tail -5` shows recent activity.)
- Contradictions are surfaced, not resolved: when a new source conflicts with
  an existing claim, keep both claims side by side with their sources and
  flag the conflict with a callout starting exactly `> ⚠️ Conflict:` (this
  exact prefix — it is how lint finds flagged conflicts).
- Every claim on a wiki page should be traceable to a raw source via the
  page's `sources` frontmatter or an inline citation.

## Workflows

- **Ingest** (interactive by default): see the wiki-ingest skill.
- **Query**: answer from wiki pages with citations; substantial answers get
  filed back into the wiki (wiki-query, wiki-file skills).
- **Lint**: periodic health check for contradictions, orphans, staleness
  (wiki-lint skill).

## Emphasis

(nothing specific yet)

## Scaling notes

- At the current scale, `wiki/index.md` plus grep is the search layer. If
  the wiki outgrows it (hundreds of pages), consider a proper local search
  tool such as qmd (https://github.com/tobi/qmd).
- Query answers default to markdown, but other formats are fair game when
  they fit: Marp slide decks, matplotlib charts, comparison tables.

## Evolving this schema

This file co-evolves with the wiki. When a convention isn't working or a new
page type is needed, propose an amendment to the user and update this file
on approval. Skills defer to this schema wherever the two disagree.
