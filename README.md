# dasdl

A front end for DASDL — the language a Unisys ClearPath MCP database uses to
describe itself — that runs on a PC. It reads a description, builds a model of
the database, and writes out both that model, as JSON, and a MariaDB schema
generated from it.

The grammar is drawn, not written:

```
<SET DECLARATION>:

>>█── SET(1) ── OF ── IDNTOK(2) ── [<WHERE CONDITION>] ── [<SET TYPE>] ── [<PHYSICAL OPTIONS>] ──█<<
```

The numbers are stations. When the parser reaches one it runs the code kept for
it in `grammar/dasdl.sem`:

```
@@BEGIN_SEM <SET DECLARATION>@@
'1': {'C': {'text': ['model.setKind("set");']}},
'2': {'C': {'text': ['model.setTarget(token.value);']}}
@@END_SEM@@
```

101 entities, 71 of them carrying semantics. That is the whole compiler.

## Try it

```
make compile
```

Builds the front end and runs it over the eight sample descriptions from the
Unisys manual, writing a `.model.json` and a `.sql` for each into `build/`.
You need `g++` with C++17; nothing else.

To read one description:

```
make build
build/dasdl -s samples/8-subsets.dasdl -o build/8-subsets
```

It writes `build/8-subsets.model.json` and `build/8-subsets.sql`. The schema is
written from the model in memory, not from the JSON: there is no second program
and no round trip through a file, so the two outputs cannot disagree.

`--maintain` says who keeps the automatic subsets current: `triggers`, the
default, has the server do it; `runtime` leaves the tables and skips the
triggers. `DASDL_NO_SQL=1` stops after the model, for a syntax-only run.

## Two files, one grammar

`grammar/dasdl.graph` is the syntax and nothing else, so it can be read as
syntax. `grammar/dasdl.sem` holds the Trackway configuration and the semantic
blocks, each with the comment explaining it. Trackway wants the two as a single
file; recombining them is a step that happens outside this repository.

## How the parser gets made

```
grammar/dasdl.graph  +  grammar/dasdl.sem
                    ↓   Trackway          ← not in this repository
              build/dasdl.cpp
                    ↓   g++
                build/dasdl
```

**Trackway is not here.** It is a compiler generator that reads railroad
diagrams and emits a recursive-descent parser, in C++, Java, Python or ALGOL,
and it is self-hosting — it compiles itself from its own syntactic definition.
It is a separate project and is licensed separately.

So you can build this compiler and change what it does with a description, by
editing the model and the SQL generator. You cannot regenerate `dasdl.cpp` from
the grammar without Trackway. That is deliberate, and saying it plainly seemed
better than letting you find out.

## What is here

| | |
|---|---|
| `grammar/` | The railroad grammar and its semantics |
| `build/dasdl.cpp` | The parser Trackway generated from them |
| `properties/` | The model DASDL builds, and the lexer and parser runtime |
| `properties/dasdl_sql.hpp` | Model → MariaDB schema |
| `samples/` | Eight descriptions from the Unisys DASDL manual |

## What this is not

This reads a database description. It does not move any data.

Getting data out of a live DMSII database is a different problem, and a harder
one: you read the audit trail rather than the database, apply the changes as
each block is written, and survive a reorganisation without anybody being
called at three in the morning. That side runs on the MCP machine, in DMALGOL
and WFL, and it is not published.

[dmsii-to-sql](https://github.com/AdolfoCl/dmsii-to-sql) shows what this front
end produces on a production description: 19,011 lines of DASDL into 780
tables, 69 indexes and 1,034 triggers, in 0.29 seconds.

## Author

Adolfo Díaz — Unisys ClearPath MCP: DMSII, DASDL, WFL, COBOL-74, ALGOL, and the
compilers for them. adolfo.diaz@ies.cl

DASDL is a Unisys language; the samples are short excerpts from *Enterprise
Database Server DASDL Programming Reference Manual*, 8600 0213-424, reproduced
for reference. Unisys has no connection with this work.

## License

MIT — see [LICENSE](LICENSE).
