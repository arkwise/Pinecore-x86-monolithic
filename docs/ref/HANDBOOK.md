# Project Handbook

> Human-readable reference for the DOS Desktop project.

---

## Programmer's Guides

| Topic | Doc |
|---|---|
| Writing kernel modules (`.kmd` drivers) | [`MODULES-GUIDE.md`](MODULES-GUIDE.md) |
| V86MT API (V86 multi-task interface) | [`../design/V86MT-API.md`](../design/V86MT-API.md) |
| CWSDPMIX (DPMI host fork) | [`../design/CWSDPMIX.md`](../design/CWSDPMIX.md) |

---

## Architecture Overview

(To be created after Phase 0 research is complete)

## Key Decisions

| Decision | Why | Reference |
|----------|-----|-----------|
| Custom window manager | Allegro 4.x only has modal dialogs | ch-04 |
| Pseudo-terminal for shell | V86 task creation requires Ring 0 | ch-02 |
| Allegro for drawing/input/timers only | GUI system too limited | ch-04 |
| CWSDPMI as DPMI host | Free, standard, source available | ch-03 |

## Deep Dives

| Chapter | Topic | Status |
|---------|-------|--------|
| 01 | i386 Multitasking (TSS, context switching) | In progress |
| 02 | V86 Mode (shell capture feasibility) | In progress |
| 03 | CWSDPMI Internals (DPMI services) | In progress |
| 04 | Allegro GUI System | In progress |

---

*Last updated: 2026-04-28*
