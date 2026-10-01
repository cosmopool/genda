# Core Principles

> Philosophy based on "Codin' Dirty" by Carson Gross — important things should be big, unimportant things should be little. Minimize the total number of functions, structs, headers, translation units, layers, and concepts in the system. Less code and fewer abstractions beat "purist" decomposition.

## Data and performance
- Organize code around data and transformations.
- Flat structures, contiguous memory: arrays of structs over arrays of pointers; fixed-size buffers or stack storage; size `malloc`/`realloc` once when length is known.
- Avoid unnecessary allocations, copies, and dynamic dispatch (function-pointer calls in hot loops, needless `void *` type erasure).
- Optimize hot paths; keep cold paths simple.

## Debuggability
- Code should step cleanly in a debugger: explicit control flow, no clever constructs.

## Correctness by construction
- Distinct types for ids/enums/units: `enum` for closed sets, single-member struct wrappers (`typedef struct { long long v; } RawId;`) — plain `typedef` doesn't stop mixing.
- Parse, don’t validate: the boundary parse fn (`dbParseInput(body, len, &in)`) is the only code that fills the struct and the one place allowed to return 0/-1 — the handler must answer 400. Past the boundary, values are trusted. Keep structs by value; no opaque heap handles.

## Fewer codepaths
Every branch a caller must handle multiplies codepaths; give callers one.
- Zero is initialization: `{0}` is a valid empty value. Failed work leaves zero and consumers just work.
- No NULL from lookups: return a read-only nil sentinel (`static const Node nil_node = {&nil_node, ...}`) or take a default.
- AND over OR: return result + its errors (`{ Events ev; Msgs msgs; }`), not result-or-error.
- Fail early: acquire buffers/resources up front in a shallow frame so deep code can't fail.
- Errors go to a log side-channel (`configLog`), not errno-style slots.

# Rules
- After changing C code, run `make test` and fix every failure.
- Always follow `.agents/rules/testing.md` when writing/editing/reviewing tests.

## Naming conventions
- Types: PascalCase (`Input`, `MpReader`)
- Functions: camelCase, prefixed per module (`dbStoreRaw`, `imapPollOnce`, `serverRun`); `static` marks private, no separate naming for private functions
- Variables: snake_case (`is_event`, `raw_id`)
- Defines, enums: UPPER_SNAKE_CASE (`BODY_CAP`, `NODE_NIL`)
- SQL schema and msgpack wire keys are contracts: rename only via migration + protocol bump.

## Testing
- Suites must be isolated: free port + temp sqlite file per run, no shared state.
- Fuzz tests (`test/fuzzy/`) only for hostile input (msgpack, HTTP); fixed seeds, bounded runtime.

## Decisions are mine — interview, don't assume
- Facts: if it can be found in the environment (filesystem, tools, git), look it up — never ask me for it.
- Decisions (approach, design, scope, tradeoffs, anything ambiguous): mine. Put each one to me with your recommended answer before acting on it — never silently pick for me.
- One question at a time; wait for my answer before the next question or any dependent work.
- Trivial calls with one obvious answer don't need a question — when in doubt, it's a decision: ask.
