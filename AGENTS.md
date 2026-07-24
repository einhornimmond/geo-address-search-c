# AGENTS.md – Gradido Blockchain Core

## C Modules (Doxygen)

- Every public C header MUST define exactly one module using `@defgroup`.
- The module MUST wrap the API using `@{` … `@}`.

```c
/** @defgroup grdd_unit grdd_unit
  *  @ingroup data
  *  @brief Fixed-point GDD (scale 10^4)
  *  @{
  */

// API here

/** @} */
```

- Modules MUST belong to a parent via `@ingroup` with there folder name (`data`, `utils`, etc).
- If the parent does not exist, DEFINE it once:

```c
/** @defgroup data Primary Data Structures */
```

----------

### Rules

- One module per header
- All public API must be inside the module block
- Use flat, stable identifiers (`grdd_unit`)

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
- scaling rules (e.g. fixed-point 10^4)
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

The `@whisper` is an optional, poetic one‑liner at the end of a Doxygen comment. It is **not required for every function**, but encouraged for functions that carry significant meaning – especially core economic functions (e.g., decay, growth, time‑based transformations).

### When to Use a `@whisper`

- **High‑impact functions** (e.g., `grdd_unit_calculate_decay`) should almost always have a `@whisper`. They are the heart of Gradido and deserve a quiet, memorable line.
- **Medium‑impact functions** (e.g., `grdd_unit_from_string`) may have a `@whisper` if a fitting image or quote comes naturally.
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

### Core Functions – Reference to Gradido Philosophy Allowed

The following functions implement Gradido’s foundational concepts. For these only, comments may explicitly mention the relevant natural laws, pillars, or the triple good.

#### Which Concepts Exist?

- **Three Natural Laws**: Symbiosis & Cooperation, Cycle of Becoming and Passing Away (decay), Support of the Living.
- **Three Pillars**: Active Basic Income, State Income, Equalisation and Environment Fund.
- **Triple Good**: Individual, Community, and Whole wellbeing.

When documenting a core function (e.g., `grdd_unit_calculate_decay`), you are free to say: *“Gradido’s second natural law – the cycle of becoming and passing away – guides this function.”* This signals to readers that they are looking at a central piece of the economic model.

**Even for core functions:** Do not preach. Describe the law, do not praise it.

### What to Avoid (Short List)

- Preaching (“should”, “must”, “good”, “fair”).
- Exclamation marks.
- Floating‑point illusions (always mention fixed‑point scaling where relevant).
- Redundant philosophy in helper functions (e.g., `grdd_unit_to_string` gets no natural law).
- Deleting or editing an existing `@whisper` unless the function changed completely.

### Enforcement & Maintenance

- When you generate or edit comments, prioritise **poetic precision** over dry correctness.
- This file is authoritative. When in doubt, follow these guidelines.

----------

**Remember:** The goal is not to produce perfect technical prose. The goal is to make reading the code a quiet pleasure – accurate, calm, and a little beautiful.
