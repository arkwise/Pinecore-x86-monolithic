# Attribution Policy

> **The forward rule.** Every pull request that touches code, documentation, or research material in this repository must follow this policy. The policy exists because pinecore-x86 builds on a large set of open-source projects (DPMI hosts, USB stacks, networking, GUI libraries) whose authors deserve recognition, and because incorrect attribution causes real harm — to upstream authors, to legal compliance, and to the project's credibility.
>
> This policy is binding on every contributor, with no exemptions. **Every name in a public file is a claim. Every claim must be verifiable.**

---

## 1. The rule, in one sentence

When you write code in this repository, when you write documentation that names an upstream project, or when you record a contribution by a named person, the attribution must be verifiable against a canonical source — and that source must be either cited in-line or added to [`THIRD-PARTY.md`](THIRD-PARTY.md).

---

## 2. What counts as a canonical source

For an upstream project (library, reference implementation, specification):
1. The project's own `LICENSE` / `COPYING` / `COPYRIGHT` / `NOTICE` file.
2. The project's own `AUTHORS` / `CREDITS` / `CONTRIBUTORS` / `THANKS` file.
3. The top-of-file copyright header in the source file you are referencing.
4. The canonical upstream repository on GitHub / SourceForge / project website / Codeberg / Gitea — *not* a mirror or fork unless the mirror is itself canonical.
5. The project's own published documentation (manual, README, technical reference).

For a person credited as a contributor to pinecore (DOSCore team, contractor, or upstream):
1. A commit, PR, or issue under their handle on a public repository.
2. A signed contract or DOSCore-internal record (linkable from the DOSCore site once attribution records are populated).
3. A canonical project README naming them as a contributor.
4. A documented citation in another canonical source above.

**What is NOT a canonical source:**
- A verbal claim in a chat conversation (it is a lead to investigate, not a fact to publish).
- A memory or recollection without a documented source behind it.
- Any attribution that cannot be traced to a primary source — even if the reasoning sounds plausible.
- A secondhand statement on a forum, mailing list, or Wikipedia article *unless* that statement itself cites a canonical source.

---

## 3. When this policy applies

### 3.1 When you add or modify code that derives from an upstream

A file is "derived from upstream" when its design, structure, or algorithm was informed by a specific open-source project — even if no code was copied. Reading USBDDOS to understand UHCI bring-up makes our `src/modules/uhci.c` *derived in the design sense* from USBDDOS; we must credit USBDDOS even though we did not copy its code.

Required actions:
1. The source file's banner comment must list the upstream(s) consulted, e.g.:
   ```
   /* References consulted (read, never copied):
    *   USBDDOS (GPL-2.0)  — UHCI bring-up template (crazii/USBDDOS)
    *   UHCI 1.1 spec      — register and TD/QH layout (Intel, 1996)
    */
   ```
2. The upstream must already appear in [`THIRD-PARTY.md`](THIRD-PARTY.md). If it does not, the PR must add it.
3. The corresponding chapter under [`docs/`](docs/) (per [`DOCUMENTATION.md`](DOCUMENTATION.md)) must record the relationship: what we read, what we built original, what we changed.

### 3.2 When you write a new file from spec only

If a file is written purely from a primary specification (no reference implementation consulted), the banner cites the spec:
```
/* References:
 *   USB 2.0 specification (April 2000) §9.1.2, p.243 — enumeration sequence
 *   USB 2.0 specification §9.6, Table 9-8, p.262     — device descriptor layout
 */
```
No upstream attribution required, because none was consulted. If you later consult one, add it.

### 3.3 When you write or modify a code file's banner comment

The banner must include a `Documentation:` line pointing to the corresponding chapter under `docs/`. The chapter must carry a reciprocal `Source:` line pointing back. See [`DOCUMENTATION.md`](DOCUMENTATION.md) §1.1 for the code↔doc mapping convention.

### 3.4 When you add a name to AUTHORS.md or to a `third-party/<project>.md` page

Before merging:
1. Confirm the canonical source of the attribution per §2.
2. Cite the source in the per-contributor entry (a URL or file path is fine).
3. If the contribution is to an upstream project, verify the name appears in that project's `AUTHORS` / `CREDITS` / `THANKS` / `LICENSE` / `README`, or in commits/PRs under that name on the canonical repository. If it does not, the attribution cannot be published as canonical — at most it is recorded as "informal" with the lack of formal attribution noted.

### 3.5 When you upgrade an upstream we depend on

If pinecore upgrades the version of an upstream project we ship or interoperate with (e.g. when Watt-32 lands in `WATT32.KMD`):
1. Update the version recorded in [`THIRD-PARTY.md`](THIRD-PARTY.md).
2. Re-check the upstream's `LICENSE` / `AUTHORS` / `THANKS` for new contributors who must be credited.
3. Note the upgrade in `CHANGELOG.md` under the relevant milestone.

---

## 4. License compliance

Pinecore consults projects under a variety of licenses (GPL-2.0, GPL-3.0, MIT, public domain, zlib, giftware, Sybase). For each upstream listed in [`THIRD-PARTY.md`](THIRD-PARTY.md):

- **No code is copied.** Where licenses (especially GPL) would require pinecore to inherit their terms upon copying, our discipline of writing original code from specifications keeps pinecore's own license tractable.
- **Attribution requirements are respected even when not legally required.** LWP 2.0 is public domain but Josh Turpen's documentation requests inclusion of the THANKS list — we include it whether or not the law compels us to.
- **Where a license requires reproduction of its text in derivative works, the full text is included** in the corresponding `third-party/<project>.md` file. (Pinecore is not currently a derivative of any GPL upstream in the copyright sense; this provision applies prospectively if that ever changes.)

---

## 5. What to do when attribution is wrong

If you discover that pinecore has credited the wrong author for an upstream contribution, or has missed an attribution that should have been included:

1. Open an issue on the repository (or contact the DOSCore Games Team directly).
2. Cite the canonical source that proves the correct attribution.
3. The maintainer corrects the attribution in `AUTHORS.md` / `THIRD-PARTY.md` / the relevant code-banner comment within one release cycle.
4. The correction is noted in `CHANGELOG.md`.

This policy is itself versioned. If a future version of the policy weakens these requirements, the version bump is itself noted in `CHANGELOG.md` with the rationale, so anyone reading the project later can audit what discipline was in effect when a given piece of attribution was published.

---

## 6. Quick checklist (for PRs)

Before submitting a PR that touches code, docs, or attribution:

- [ ] Any new code file has a banner comment listing references consulted (per §3.1 or §3.2).
- [ ] Any new upstream consulted is listed in `THIRD-PARTY.md` with verified canonical author + license + license URL.
- [ ] Any new name added to `AUTHORS.md` or to a per-project page is verifiable per §2.
- [ ] Any code↔doc mapping is bidirectional (banner `Documentation:` line + chapter `Source:` line).
- [ ] `CHANGELOG.md` is updated for the milestone if the change is user-visible.
- [ ] The PR description explains *why* the attribution is correct (which canonical source, and the link).

---

*This policy is version 1.0, effective from the initial public release. Future revisions will be noted in `CHANGELOG.md`.*
