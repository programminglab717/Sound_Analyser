# ADR 0003 — Non-destructive EDL document model

**Status:** Accepted · 2026-09-18

## Context
Audio editors historically edit samples in place, holding an undo stack of audio
snapshots. That is simple but costs disk, makes undo expensive, and loses every
decision the moment a file is saved.

## Decision
The document is an **edit decision list** over immutable, content-hashed sources,
with an ordered chain of non-destructive process nodes. "Destructive" operations
are implemented as `flatten()`: render a range, replace it with a new source.

## Rationale
- Undo becomes O(1) and costs no disk.
- Sessions reopen with every parameter still adjustable — the thing users most
  often wish for after the fact.
- Render caching keyed by `(clipId, nodeChainHash, range)` means editing node 3
  of 5 only re-renders from node 3.
- Spectral layers need this model anyway; retrofitting it later would be a rewrite.

## Consequences
- More complex than in-place editing, and the render scheduler becomes a real
  component rather than an afterthought.
- Cache invalidation is now a correctness concern, and needs its own tests.
- Users expecting destructive behaviour still get it via `flatten()`, so the
  familiar workflow survives.

## Revisit if
Profiling shows the node graph dominates interactive latency in a way caching
cannot address. Considered unlikely — this is the standard model in every
professional editor.
