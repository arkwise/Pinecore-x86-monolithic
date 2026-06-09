# Contributing to pinecore-x86

> Welcome. pinecore-x86 is a research-driven project: every architectural decision is grounded in a primary source (an Intel manual, a DPMI specification, a USB spec, or a clean reference implementation), and every change is expected to follow the same discipline.
>
> This file is the project's working contract. It tells contributors what's expected before a PR is opened, what conventions the codebase follows, and what the toolchain looks like. It applies equally to human contributors and to AI-assisted contributions (see also [`ATTRIBUTION-POLICY.md`](ATTRIBUTION-POLICY.md)).

---

## 1. Before you start

Read these, in order:

1. [`README.md`](README.md) — what the project is and what's in the box today.
2. [`roadmap.md`](roadmap.md) — multi-phase plan; what phase the project is in and what's next.
3. [`DOCUMENTATION.md`](DOCUMENTATION.md) — the documentation roadmap; per-subsystem chapter index.
4. [`FILE-STATUS.md`](FILE-STATUS.md) — per-file stability tracker. *Read this before editing ANY source file.*
5. [`DECISIONS.md`](DECISIONS.md) — architectural decision record.
6. [`ATTRIBUTION-POLICY.md`](ATTRIBUTION-POLICY.md) — the forward rule on crediting upstream code and contributors.

For a specific subsystem, the relevant `docs/research/NN-*.md` chapter is the primary reference. The `docs/research/` directory is where every architectural claim is sourced.

---

## 2. Pre-edit checklist

Before modifying any source file:

1. **Is the change in scope?** Does it relate to the current task, or are you drifting?
2. **Is the file STABLE?** Check [`FILE-STATUS.md`](FILE-STATUS.md). STABLE files are treated as read-only unless you are fixing a confirmed bug.
3. **Minimum change only.** Smallest edit that achieves the goal. Don't refactor adjacent code "while you're there".
4. **Blast radius.** Will this change force edits in other files? List them in the PR description.
5. **Roadmap alignment.** If you're adding a feature, it must be in the current phase per [`roadmap.md`](roadmap.md). Speculative work belongs in a branch.

---

## 3. Working rules (non-negotiable)

### 3.1 Research discipline

- **Investigate first, never assume.** Read existing code before changing anything.
- **Source everything in research docs.** When a new architectural claim is added to a `docs/research/` chapter, cite the primary source (page number from the Intel manual, paragraph from a spec, file:line from a reference implementation). Routine engineering doesn't need a citation; novel design claims do.
- **Never copy code directly.** Study principles, write original. The [`ATTRIBUTION-POLICY.md`](ATTRIBUTION-POLICY.md) elaborates.

### 3.2 Process

- **Follow the roadmap.** Work follows phases in order; don't jump ahead.
- **Update [`CHANGELOG.md`](CHANGELOG.md).** Every change that's user-visible or behaviorally significant gets a milestone entry.
- **Update [`FILE-STATUS.md`](FILE-STATUS.md)** if a file's stability status changes.
- **Update [`DOCUMENTATION.md`](DOCUMENTATION.md)** if a doc chapter's status changes (e.g., `STUB → DRAFT`).

### 3.3 Anti-churn

- **Don't refactor what you're not working on.**
- **Don't add features not in the current phase.**
- **Don't change file structure unless the task requires it.**
- **If a file is STABLE, treat it as read-only.**

### 3.4 Citation conventions (project-specific)

When a code comment or research chapter cites a primary source, use these formats:

| Source | Format | Example |
|---|---|---|
| Intel 386 PRM (1986) | `(386-bible p.NNN)` | `(386-bible p.142)` |
| Intel SDM (current) | `(SDM Vol N §X.Y)` | `(SDM Vol 3 §4.7)` |
| CWSDPMI source | `(cwsdpmi: file.c:line)` | `(cwsdpmi: exphdlr.c:115)` |
| Allegro 4.x source | `(allegro: file.c:line)` | `(allegro: src/dos/d_vesa.c:230)` |
| USB specs | `(USB 2.0 §X.Y, p.NN)` | `(USB 2.0 §9.1.2, p.243)` |
| Watt-32 source | `(watt32: src/<file>:<line>)` | `(watt32: src/sock_io.c:88)` |
| Pinecore source | `(pinecore: src/path.c:line)` | `(pinecore: src/kernel/dpmi.c:614)` |

Format the citation immediately after the claim so the reader can verify it.

### 3.5 STABLE-after-test requirement

A source file is not promoted to STABLE in [`FILE-STATUS.md`](FILE-STATUS.md) until the affected behavior has been observed working in DOSBox / QEMU / real DOS hardware. Promotion criteria are documented in the relevant `docs/` chapter.

---

## 4. Toolchain

pinecore-x86 has **two cross-compilers**. Both are required for a full build.

| Toolchain | Used for | Source |
|---|---|---|
| `i686-elf-gcc` (and `-ld`, `-objcopy`) | Kernel + boot stubs + `.kmd` modules (freestanding) | Built from binutils + gcc with `--target=i686-elf`; see `docs/research/06-toolchain.md` |
| `i586-pc-msdosdjgpp-gcc` (DJGPP) | DOS user-space programs (`pinecone/DESKTOP.EXE`, the test client) | DJGPP cross-compiler distribution |
| `nasm` | Bootloader, ISR stubs, low-level PM stubs | Standard |

### Do not use clang on the kernel sources

Clang LSP diagnostics on kernel C files are spurious — clang can't find the freestanding `types.h`, doesn't know `DPMI_MAX_CLIENTS` or our other macros, and produces apparent-error noise that doesn't apply to the actual cross-compiler build. Do not run clang on this codebase, do not act on clang diagnostics, and do not "fix" kernel code based on them. Verify your changes by running `make` in `src/` and checking that it finishes with `Pure kernel build OK` and `DOS kernel build OK`.

For DJGPP code (`pinecone/`), only the DJGPP cross-compiler from the DOSCore team's pinned toolchain set is supported; other DJGPP installations may produce divergent behavior.

---

## 5. Build and run

```bash
# Kernel + FreeDOS-loaded HDD image
cd src && make all              # → kernel.dos.bin + kernel.pure.bin
cd src && make modules          # → *.kmd kernel modules
cd src && make pure-usb         # → pinecore-pure-usb.img (64 MB FAT16, bootable USB)

# Pinecone test client (DJGPP + Allegro)
cd pinecone && make             # → DESKTOP.EXE

# Run in QEMU
cd src && make run-pure         # → boots Pinecore to the Commando shell
cd src && make run-pure-usb     # → boots the pure-USB image
cd pinecone && make run-pinecore  # → auto-launches DESKTOP.EXE
```

The serial log is the primary debugging surface — every DPMI call, IRQ delivery, scheduler sample, and PM exception is traced via COM1.

---

## 6. Pull request expectations

A good pinecore-x86 PR carries:

1. **A focused change.** One subsystem, one purpose. Big refactors get their own PR series with a written plan in the PR description.
2. **A research citation.** If the change implements something new (a driver, a syscall, a behavior), cite the spec or reference implementation that grounds the design. Use the formats in §3.4.
3. **An attribution check.** Per [`ATTRIBUTION-POLICY.md`](ATTRIBUTION-POLICY.md): if you consulted an upstream project, the source file's banner comment references it, and [`THIRD-PARTY.md`](THIRD-PARTY.md) records the relationship.
4. **A test plan.** What you ran (`make all`, `make run-pure`, target QEMU options), what you observed in the serial log, what real-hardware results you have if any.
5. **`CHANGELOG.md` update** for user-visible changes.
6. **`DOCUMENTATION.md` status update** if a doc chapter moves between STUB / DRAFT / DONE.

If the PR is purely research (a new `docs/research/` chapter), the citation discipline still applies — every claim referencing a primary source, in the formats above.

---

## 7. AI-assisted contributions

AI-assisted PRs (drafting, research synthesis, code generation) are accepted under the same discipline as human PRs. Specifically:

- The contributor is the human who reviews and submits — not the AI. Sign your PR with your own handle.
- Attribution must be verified per [`ATTRIBUTION-POLICY.md`](ATTRIBUTION-POLICY.md). AI-proposed attribution that cannot be traced to a primary source must be removed or downgraded to "informal" with the lack-of-formal-attribution noted.
- Code that the AI drafted must still build cleanly under the project's cross-compiler before merge — clang-only "fixes" are explicitly disallowed (§4).

---

## 8. Getting help

- For technical questions: read [`docs/research/`](docs/research/) first; the answer is often there.
- For build / toolchain issues: check [`docs/research/06-toolchain.md`](docs/research/06-toolchain.md).
- For attribution questions: read [`ATTRIBUTION-POLICY.md`](ATTRIBUTION-POLICY.md).
- For everything else: open an issue on the canonical repository, or contact the DOSCore Games Team.
