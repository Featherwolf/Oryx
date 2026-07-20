# ADR-0003 — Extend GameNative/GameHub, don't fork

- **Status:** Accepted
- **Date:** design phase

## Context

The user goal explicitly includes integration with apps like GameNative and GameHub. These
front-ends already contain the seams Oryx needs:

- **Backend selection** — GameNative already switches between Box64 and FEX per game
  (community "FEX compatibility profiles").
- **Driver swap** — users already sideload community Turnip builds (Kimchi, Mr. Purple,
  K11MCH1).
- **Per-game config** — env vars / options are already per-title.

Forking would fragment a small community and duplicate the Wine/container plumbing that is
these apps' actual hard-won value.

## Decision

Ship Oryx as **drop-in components that populate existing hooks**, not a fork:

| Oryx part | Integration seam |
|-----------|------------------|
| A — HW-TSO | A capability flag the Box64/FEX backend consults; an opt-in kernel/Magisk module for rooted users |
| B — Cache client | A step inserted into the launch sequence; writes into the emulator's existing translation/shader cache paths |
| C — Auto-tuner | Populates the per-game config the front-end already applies |
| D — Driver/pipeline | Curates and auto-selects the Turnip build the front-end already lets users swap |

## Consequences

- **Positive:** Users keep their existing front-end, library, and container setup.
- **Positive:** Part A stays opt-in — rooted enthusiasts get hardware TSO; everyone else
  still gets B/C/D on a stock phone.
- **Negative:** We depend on the front-ends' hook stability; upstreaming (Phase 3) is needed
  to make integration first-class rather than bolted-on.
- **Negative:** Coordinating changes across independent front-ends is social, not just
  technical, work.
