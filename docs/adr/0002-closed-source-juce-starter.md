# ADR 0002 — Closed-source under the JUCE Starter tier

**Status:** Accepted · 2026-09-18

## Context
The product ships free at launch and moves to freemium later. JUCE is dual
licensed: AGPLv3, or commercial in Starter/Indie/Pro tiers. Open-sourcing was
raised as a way to avoid licensing friction.

## Decision
**Remain closed-source.** Use the free **JUCE Starter** tier during Stage 1,
upgrade to Indie/Pro when revenue approaches the cap.

## Rationale
- Starter is free, permits closed-source distribution, and since JUCE 8 imposes
  no splash-screen requirement.
- Open-sourcing would force AGPLv3 on the whole application, making the freemium
  entitlement model unenforceable — anyone could strip the checks and
  redistribute.
- **Critically, open-sourcing would not solve our actual blocker.** The binding
  constraint is ML weight licensing (Demucs is research-only; several strong
  models are CC BY-NC). Those are restrictions on *use*, not on source
  disclosure, so our source licence is irrelevant to them.
- The two things open-sourcing *would* unlock (FFTW, Rubber Band) both have
  acceptable permissive substitutes.

## Consequences
- We must monitor the Starter revenue cap, which counts **all** revenue and
  funding from framework use including donations and sponsorship.
- The licence must be maintained for as long as we distribute binaries, not just
  while developing.
- We cannot use GPL dependencies at all. Every candidate dependency needs a
  licence check before adoption — enforced in CI via the manifest.

## Revisit if
JUCE materially changes the Starter terms, or the business model moves away from
paid tiers entirely.
