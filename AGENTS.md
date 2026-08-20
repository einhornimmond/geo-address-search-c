# AGENTS.md – geo-address-search-c

Everything in this repository is yours to edit — src, tests, the build files, the docs.
The one exception is third_party: leave those files exactly as they are. A bug in a
vendored library is worked around in our own code, never patched at the source.

## C Modules (Doxygen)

- Every public C header MUST define exactly one module using `@defgroup`.
- The module MUST wrap the API using `@{` … `@}`.

```c
/** @defgroup geo_cell geo_cell
  *  @ingroup search
  *  @brief Coordinates folded into a cell token
  *  @{
  */

// API here

/** @} */
```

- Modules MUST belong to a parent via `@ingroup` with there folder name
  (`foundation`, `parser`, `search`, `types`).
- If the parent does not exist, DEFINE it once:

```c
/** @defgroup foundation Foundation */
```

----------

### Rules

- One module per header
- All public API must be inside the module block
- Use flat, stable identifiers (`geo_cell`)

----------

### Goal

Ensure all APIs appear in Doxygen “Modules” with a clear hierarchy.

## Commenting Guidelines for AI Agents, Poetic Precision – Dual-Layer Commenting Standard

## Core Model

All comments consist of two aligned layers:

### 1. Technical Layer (Ground Truth)

Hard, verifiable specification.

Must include:

- parameters, types, constraints
- widths and their limits (e.g. ranks are `uint32_t`, offsets `uint64_t`)
- edge cases
- return behavior
- overflow / limits
- deterministic rules

Rules:

- no ambiguity
- no metaphor instead of facts
- fully sufficient for implementation without poetic layer

----------

### 2. Semantic Layer (Poetic Precision)

Describes system behavior as **natural process perception**.

Allowed:

- flow, cycle, rhythm, transition
- dissolve, emerge, settle, converge
- stream, season, tide, growth, decay
- backward projection / forward preparation

Constraints:

- must not change technical meaning
- must not introduce moral framing
- must not replace constraints with imagery
- must stay fact-consistent

Purpose:

- reduce cognitive load
- improve conceptual continuity
- express system behavior as continuous process

----------

## Forbidden Transformations

Do NOT convert:

- constraints → metaphors only
- limits → value judgments
- edge cases → poetic ambiguity
- precision → narrative softness

----------

## Writing Principle

Each comment is:

> deterministic logic + natural process description

Never:

- poetry instead of specification
- specification without semantic flow

----------

## Internal Objective

Increase:

- readability of complex systems
- continuity of mental model
- semantic coherence across codebases

Without reducing:

- precision
- determinism
- auditability

## The `@whisper` Tag – Optional Poetic Signature

The `@whisper` is an optional, poetic one‑liner at the end of a Doxygen comment. It is **not required for every function**, but encouraged for functions that carry significant meaning – especially the ones the whole program turns on (e.g., merging, folding, mapping an index into memory).

### When to Use a `@whisper`

- **High‑impact functions** (e.g., `name_run_merge`, `geo_index_open`) should almost always have a `@whisper`. They are the heart of the program and deserve a quiet, memorable line.
- **Medium‑impact functions** (e.g., `geo_cell_of`) may have a `@whisper` if a fitting image or quote comes naturally.
- **Low‑level helpers** (e.g., internal byte swappers) rarely need a `@whisper`. If in doubt, omit it.

### What a `@whisper` Must Do

- Briefly describe the **essence of the function** in poetic, calm language.
- OR quote a **famous person** (with attribution) that fits the function’s purpose. Keep quotes short and universally respectful.
- Be **subtle, never loud**. No exclamation marks, no moralizing.
- End without a period.

### What a `@whisper` Must NOT Do

- Replace or compensate for missing technical documentation.
- Preach (“you should”, “it is good to”).
- Drift into irony or sarcasm.

### Editing Existing `@whisper` Lines

- **Never delete** an existing `@whisper` unless it is completely unrelated to the current function’s behavior.
- **Updating** is allowed only when the function itself has changed so much that the old whisper no longer fits. In that case, rewrite it to match the new purpose while preserving the poetic tone.

### Respect Existing `@whisper` Lines

- **Never delete** an existing `@whisper` unless it has become completely unrelated to the function’s current behavior.
- **Updating** is allowed only when the function itself has changed so much that the old whisper no longer fits. In that case, rewrite it to match the new purpose while preserving the poetic tone.
- Do not change a `@whisper` just for stylistic preference. If it works, let it be.

### Standard Comment Structure (Flexible)

The structure is a suggestion, not a straitjacket. Adapt length and order as needed.

```c
/**
 * @brief One-line summary (poetic but clear).
 *
 * A few sentences explaining what the function does. Use calm, image‑rich
 * language. Mention technical details naturally within the flow.
 *
 * @param[in/out] name   Description.
 * @return               Exact return values (e.g., true/false, number of bytes).
 * @note (optional)      Important constraints.
 * @whisper (optional)   Short poetic line, no period.
 */
```

### The Guiding Principles – And Why They Stay Out of the Comments

Three principles carry this codebase:

- **Sufficiency** – Use only what is needed.
- **Flow** – Never make the human wait unnecessarily.
- **Simplicity** – Complexity must earn its place.

They shape what gets written and what gets left out. They do not get named in comments.

The reason is that a principle is not a fact about the code. *“Sufficiency guides this function”* cannot be checked against anything, so it cannot be wrong, so it survives every change the code makes underneath it. What a reader can check — and what actually keeps the next person from undoing the decision — is the mechanism and what it costs:

```c
/* The wait is the throttle: a producer that cannot place its batch stops
   reading, and the stream slows to the pace the parsers set. */
```

That comment carries Flow without the word. Write the reason and its consequence; the principle is what made you write it, not part of what you write.

### What to Avoid (Short List)

- Preaching (“should”, “must”, “good”, “fair”).
- Exclamation marks.
- Silent widths (always name the type a count or an offset is measured in where it matters).
- Naming a principle where the mechanism would say it better — which is everywhere.
- Deleting or editing an existing `@whisper` unless the function changed completely.

### Enforcement & Maintenance

- When you generate or edit comments, prioritise **poetic precision** over dry correctness.
- This file is authoritative. When in doubt, follow these guidelines.

----------

**Remember:** The goal is not to produce perfect technical prose. The goal is to make reading the code a quiet pleasure – accurate, calm, and a little beautiful.
