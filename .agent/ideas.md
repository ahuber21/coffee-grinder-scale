# Parked ideas — explicitly out of scope for this rewrite

Things worth remembering, not worth acting on now. Hardware is fixed for
this rewrite (D1) — anything here that touches hardware waits for a
deliberate future decision to revisit that boundary, not a default to
sneak it into "just software" work.

---

### Reduce topup-pulse clumping via a grinder/chute modification

Raised 2026-09-11 during AR-022/D13 (see `.agent/design/topup-model.md`
§7). The measured ~0.15-0.2g minimum controllable topup increment is
driven by ground coffee clumping unpredictably during the relay's
~0.3-0.4s minimum on-time — a clump either falls during that window or it
doesn't, and that's what limits topup precision, not the software.

The owner's hypothesis: a physical change to the grinder's chute/funnel
geometry might reduce clumping (encourage a more continuous trickle
instead of discrete clumps even during a short pulse), which would lower
the achievable topup granularity and make the 0.05g "spot on" target more
reachable via topup itself, not just via a better main-grind stop.

Not investigated. Would need to be scoped as its own project (hardware
change, likely printable, would need testing against the same historical
data-collection pattern this rewrite is setting up) — worth revisiting
once the software side of this rewrite is stable and its data collection
is running for a while, so there's a clean "before" baseline to compare
against.
