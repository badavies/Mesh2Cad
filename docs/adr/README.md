# Architecture decision records

The index of decisions made so far is in [`../ARCHITECTURE.md`](../ARCHITECTURE.md), section 14. Each decision that needs more than one line gets a file here, named `NNNN-short-title.md`.

A decision is changed by a new record that supersedes the old one, never by editing it.

## Template

```markdown
# NNNN. Title

- Status: Proposed | Accepted | Superseded by NNNN
- Date: YYYY-MM-DD

## Context
What forces the decision. Link the roadmap issue.

## Decision
What we will do.

## Alternatives considered
What else was possible and why not.

## Consequences
What gets easier, what gets harder, what must be checked at the next milestone gate.
```
