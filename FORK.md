# This is a fork of VTIL-Core (wmpdevrit devirtualizer)

This repository is **not** upstream [vtil-project/VTIL-Core](https://github.com/vtil-project/VTIL-Core).
It is a fork carrying a small set of **loop-aware** patches used by the
[`wmpdevrit`](https://github.com/perfectplug/wmpdevrit) VMProtect devirtualizer. Everything in
`README.md` still describes upstream VTIL; this file documents only what diverges.

## Why the fork exists

VMProtect routines with loops carry values across the loop back-edge in virtual registers (VRs) and
in operand-stack slots. Several of VTIL's optimizer passes (and the tracer) are **loop-unaware**:
following the CFG through a back-edge, they resolve a loop-header read to the single *dominating
pre-loop definition* and fold the loop-carried value to its entry constant. That is sound for
straight-line / loop-control values, but it collapses the recurrence of a loop-*carried* VR (the
counter never advances, an accumulator reads its INIT value every iteration).

The fork adds opt-in guards that keep **lift-marked, loop-carried** VRs / stack slots symbolic
across the back-edge, so `wmpdevrit`'s loop recovery sees the real recurrence.

## Gating discipline (default OFF → byte-identical)

Every behavioural change is behind an environment-variable gate that **defaults unset**, so a build
of this fork with no `WMP_*` variable set produces byte-identical output to upstream for every
existing routine. The guards are additionally **allowlist-scoped** (except `WMP_LOOPAWARE_RTRACE`,
see below): they only act on VRs / stack slots the lift explicitly marked, so even with the flag set
they are inert when the allowlist is empty.

### Cross-project contract (the allowlists)

Two public globals in namespace `vtil::optimizer` are the interface with `wmpdevrit`:

| symbol | header | contents |
|---|---|---|
| `wmp_loopcarried_vrs` | `VTIL-Compiler/optimizer/mov_propagation_pass.hpp` | `std::set<uint64_t>` of loop-carried VR `local_id`s |
| `wmp_loopcarried_stackslots` | `VTIL-Compiler/optimizer/stack_propagation_pass.hpp` | `std::set<int64_t>` of loop-carried operand-stack `REG_SP` offsets |

`wmpdevrit`'s lifter **populates these sets before calling `optimizer::apply_all`**. VTIL-Core only
reads them; nothing inside this repository inserts into them, so in isolation the loop-aware guards
are inert unless a test / caller populates the sets (see `VTIL-Tests/wmp_loopaware.cpp`).

## Environment-variable surface

All default **OFF** (unset). The load-bearing guards cache their flag in a `static const bool` read
once per process.

### Load-bearing loop-aware guards (allowlist-scoped)

| var | pass / file | effect when set |
|---|---|---|
| `WMP_LOOPAWARE_MOV` | `optimizer/mov_propagation_pass.cpp` | do not forward a cross-block read of an allowlisted loop-carried **virtual** register to its dominating pre-loop def |
| `WMP_LOOPAWARE_STACK` | `optimizer/stack_propagation_pass.cpp` | do not propagate a cross-block load of an allowlisted loop-carried operand-stack `REG_SP` slot |
| `WMP_LOOPAWARE_DCE` | `optimizer/dead_code_elimination_pass.cpp` | keep (do not DCE) an allowlisted loop-carried VR's per-iteration def |
| `WMP_LOOPAWARE_SYMREWRITE` | `optimizer/symbolic_rewrite_pass.cpp` | keep an allowlisted loop-carried VR symbolic instead of baking it to its pre-loop constant |

### Loop-aware guard with BROADER effect — use with care

| var | pass / file | effect when set |
|---|---|---|
| `WMP_LOOPAWARE_RTRACE` | `trace/tracer.cpp` | inside a potential loop, do not fold **any** cross-block memory-or-virtual read to a pre-loop def. **Not** restricted to `wmp_loopcarried_vrs` — it fires for every loop-local memory/virtual read, so it can over-conservatively keep unrelated values symbolic. Enable only for the loop routines that need it. |

### Diagnostic / bisection (no semantic change beyond skipping a pass)

| var | pass / file | effect when set |
|---|---|---|
| `WMP_SKIP_SYMREWRITE` | `optimizer/symbolic_rewrite_pass.cpp` | skip the symbolic-rewrite pass (bisect which `apply_all` pass collapses a value) |
| `WMP_SKIP_RENAMING` | `optimizer/register_renaming_pass.cpp` | skip the register-renaming pass (bisection) |
| `WMP_SYMRW_KEEPMEM` | `optimizer/symbolic_rewrite_pass.cpp` | experimental: keep a lift-marked loop-carried `$sp` memory cell symbolic even when it folds to a constant |
| `WMP_BAKEWATCH` | `optimizer/symbolic_rewrite_pass.cpp` | stderr report when a symbolic value is baked to a constant |
| `WMP_LCVR_DBG` | `trace/tracer.cpp` | stderr trace of loop-carried-VR tracing decisions |
| `WMP_MOVPROP_DBG` | `optimizer/mov_propagation_pass.cpp` | stderr trace of cross-block virtual mov-forwarding decisions |
| `WMP_STACKPROP_DBG` | `optimizer/stack_propagation_pass.cpp` | stderr trace of cross-block stack-slot propagation decisions |

(`WMP_CANON_VOL` appears only in a comment in `dead_code_elimination_pass.cpp`; it is not an active
`getenv` gate.)

## Build / dependency notes

- `Dependencies/capstone` is fetched from `capstone-engine/capstone` (the maintained home of Capstone;
  the original `aquynh/capstone` mirror is inactive) pinned to the same commit upstream VTIL used.
- The MSBuild `.vcxproj` toolset was bumped v142→v143 for `Debug|x64` during fork work; other
  configs may still be v142. The CMake build (used by CI) is unaffected.

## Tests

`VTIL-Tests/wmp_loopaware.cpp` exercises the allowlist-scoping of the loop-aware guards (the guard
acts only on allowlisted VRs; an empty allowlist is inert). Run the suite with the relevant
`WMP_LOOPAWARE_*` variable set to cover the gated-on path; without it, the suite covers the
default-off (byte-identical) path. CI runs both.
