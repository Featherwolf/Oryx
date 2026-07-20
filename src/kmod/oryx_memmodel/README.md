# oryx_memmodel — per-thread hardware x86 memory mode (Part A)

The production driver that grants a translated thread hardware Total Store Order on Oryon
cores, so Box64/FEX can emit plain loads/stores instead of barriers. This is what turns the
Phase 0 discovery into shippable performance.

## How it works

- A translated thread opens `/dev/oryx_memmodel` and issues `ORYX_MM_ENABLE_TSO`.
- The driver registers a **preempt notifier** for that thread:
  - **sched_in** → set the discovered control bit on the (eligible) core it lands on.
  - **sched_out** → clear it, so the next thread on that core runs weak (fast).
- The grant is bound to the fd: closing it (or the thread exiting) restores weak ordering.
- Only opted-in threads pay the ~9% hardware-TSO cost; the rest of the system is unaffected.

This mirrors how Apple's kernel sets the `ACTLR_EL1` TSO bit per-thread for Rosetta, but as a
loadable module using preempt notifiers instead of a core-kernel scheduler hook.

## Parameters (pinned from Phase 0)

| Param | Meaning |
|-------|---------|
| `oryx_reg_idx` | Which candidate register holds the control bit (0 = `ACTLR_EL1`) |
| `oryx_bit` | Bit position within that register |
| `oryx_confirmed` | **Must be 1** or `ENABLE_TSO` fails closed with `-ENODEV` |
| `oryx_eligible` | Core bitmask where TSO is permitted (honors any core restriction found in Phase 0) |

## Requirements & safety

- arm64 with `CONFIG_PREEMPT_NOTIFIERS=y` (present when KVM is enabled).
- Setting the bit is on the context-switch fast path **only for TSO threads**; measure the
  overhead during bring-up.
- Fails closed: with no confirmed bit, no thread can enable TSO. Ship with `oryx_confirmed=0`
  until Phase 0 passes on the target kernel/firmware.
- Access is `0666` on the node but gated in practice by SELinux policy + the Oryx entitlement;
  do not ship it world-enablable in a real image.

## Userspace

Emulators use [`liboryxmm`](../../liboryxmm/) rather than touching the ioctl directly:

```c
int fd = oryx_mm_thread_tso_on();   /* per translated thread */
if (fd >= 0) translator_disable_memory_model_emulation();
/* ... keep fd open for the thread's lifetime ... */
oryx_mm_close(fd);                  /* on thread exit: restores weak ordering */
```

## Upstream path (Phase 3)

The module is the enthusiast/opt-in delivery. The sanctioned end state is an in-tree
`prctl(PR_SET_MEM_MODEL, PR_MEM_MODEL_TSO)` — mirroring the ARM-Linux TSO patch series —
so unrooted devices can use it. See [Part A design](../../../docs/partA-hardware-tso.md).
