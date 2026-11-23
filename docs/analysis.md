# Analysis and diagram rules

`cpp-to-drawio` reads either form of a Clang compilation database entry.
`arguments` preserves the argument vector. `command` uses Clang's command
line parser. Relative arguments are interpreted in the entry's working
directory. LibTooling runs Clang in syntax-only mode. The converter uses the
commands as compiler arguments and does not invoke their compiler executables
or link the project.
[Clang compilation database format](https://clang.llvm.org/docs/JSONCompilationDatabase.html)

## Selecting translation units

The project tree is the boundary for model declarations. C++ source extensions
include `.cpp`, `.cc`, `.cxx`, `.c++`, `.cp` and uppercase `.C`.
The converter analyzes matching compilation commands and models named types
defined by those sources and included project headers. It resolves referenced
external types through Clang and keeps their type spellings. System headers
and external libraries stay outside the set of class boxes when they are
outside the selected tree.

Discovery skips symbolic links and directories named `.git`, `.vs`, `build`,
`out`, `dist`, `node_modules`, `build-*` and `cmake-build-*`. Files found
without a command are reported as skipped and the summary marks that analysis
as incomplete. Headers must be included by a selected translation unit. For a
header-only project, add a small translation unit that includes its public
headers and export that unit's compile command.

Clang merges declaration chains within each translation unit. The converter
coalesces repetitions of the same definition across translation units and
keeps explicit member declarations, including overloads. Template instantiation
does not add duplicate class or operation boxes. If two compile commands
expose different definitions for the same qualified type, use a database for
one consistent configuration. A Clang error or conflicting definition stops
publication. Clean analysis with no named definitions produces an empty
editable page and a warning.

## Static analysis

The Clang Static Analyzer shares the parsed translation unit and its compilation
settings with the UML analysis. Its checks cover null pointer dereferences,
uninitialized values, division by zero, references to expired storage, memory
leaks, use after free, mismatched deallocation and unsafe library calls. It
reports findings located in the selected source tree, including findings in
headers reached through analyzed code. Repeated findings at the same location
with the same check and message count once.
[Clang checker reference](https://clang.llvm.org/docs/analyzer/checkers.html)

Findings appear on standard error in this form.

```text
src/cache.cpp:24:12: warning: Dereference of null pointer [core.NullDereference]
```

The scan uses the project configurations in the selected compilation database
and focuses path-sensitive analysis on individual translation units. The
reported count reflects the paths Clang explored in that run. Files skipped
because they lack a compilation command also fall outside the scan. Use
`--no-static-analysis` when you want to export the diagram without running
the additional checks.

## Relationships

| Kind | Evidence | Appearance |
| --- | --- | --- |
| Generalization | A direct base class from the derived type, including virtual bases. | Solid connector with a hollow triangle at the base. |
| Association | A modeled type in a field's type, including pointer, reference, array and template argument types. | Solid connector. Self-associations are loops. |
| Dependency | A modeled type in a constructor or method signature or a base's template arguments. | Dashed connector with an open arrow. |

Each source, target and kind appears once. A dependency is omitted when a
stronger relationship already connects that directed pair. Associations
describe declared structure and carry no inferred ownership or multiplicity.
Types and uses outside declared structure, including local classes, lambda
types and relationships from statements in method bodies, are outside this
model. Named types inside anonymous namespaces are also outside the model.

## Layout and output

The writer emits editable Draw.io `mxCell` vertices, child compartments and
edges. A deterministic grid groups related types. Text determines the
dimensions of each box and orthogonal routes use gaps between rows and
columns. Dense diagrams can share connector segments. Draw.io can reposition
the shapes and edges after export.
[Draw.io file format](https://www.drawio.com/docs/reference/file-format/)

Output is staged in its destination directory and published after successful
analysis. With `--create-output-dir`, the converter creates missing directories
at that point. With `--force`, it replaces the entire existing diagram.
