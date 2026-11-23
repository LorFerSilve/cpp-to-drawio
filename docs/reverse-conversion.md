# Draw.io to C++

`drawio-to-cpp` accepts an `mxfile` with plain or compressed diagrams and a
single `mxGraphModel`. For the converter's multipage exports, detailed
declarations supersede matching compact boxes. An inheritance edge on the
overview can connect classes on different detail pages. Invalid declarations
identify the page and cell when those are available.

## Hand-drawn boxes

Use a UML class shape with its native rows or an ordinary box. Mark the title
of an empty ordinary box with `+`. The title can be qualified as
`+ inventory::Order`, or you can put `namespace inventory` under a short
title or on its namespace container. For a nested type, use a title such as
`+ Order::Line` and give both boxes the same explicit namespace.

| Diagram text | C++ declaration |
| --- | --- |
| `«struct»`, `«union»`, `«enumeration»`, `«enum class»` | Type kind on or above the title. The default is a class. |
| `+ Repository<class T, unsigned N = 4>` | Template class with a type and a non-type parameter. |
| `- items: T $[N]` | Private array field. The optional `$` marks the identifier in a complex declarator. |
| `+ count: int {static}` | Public static field. Underlining also marks a static member. |
| `- bits: unsigned {bits 12}` | Private bit field. |
| `+ callback: void (*$)(int)` | Public function pointer field. The same notation works in parameters. |
| `+ Order(id: int) {init id_(id)}` | Constructor with a member initializer. |
| `+ ~Order() {virtual} {default}` | Defaulted virtual destructor. |
| `# get(index: unsigned = 0): T & {const} {&} {noexcept}` | Protected, const, lvalue-qualified member function. |
| `+ <class U> convert(value: U): T` | Member function template. |
| `+ read(): int {virtual} {pure}` | Pure virtual operation. Italicizing the operation also marks it as pure. |
| `underlying unsigned` and `ready = 1` | An enum underlying type in the header and an enumerator in the field rows. |

Use `+` for public, `#` for protected and `-` for private. When the marker
is omitted on a member, the C++ default for the type kind applies. Constructors,
destructors and conversion operators omit the UML return type. Other operations
use `: Type` after their parameter list, including `: void` when appropriate.

Use the C++ spelling for types and template parameters. Qualify types in other
namespaces. Add `#include <header>` or `#include "header"` lines below the title
for dependencies outside the diagram. Initializers, defaults and exception
conditions are C++ expressions. Parenthesize comparisons used inside template
argument lists. To specify direct or delegated construction, provide
`{init Base(id), member_(value)}` on a constructor.

## Inheritance and dependencies

A hollow triangle points to the base class. An arrow alone creates a public
base. Put a line such as `base protected virtual inventory::Base<int>` under
the title to retain access, virtual inheritance or template arguments. A typed
clause for the same base supplies the C++ spelling used for the arrow. On an
exported diagram the typed clauses are also visible in the shape's Edit Data
dialog under `cppBases`. Edit that data when changing an existing typed base.

References to types defined in the diagram produce generated includes or
forward declarations. Complete types are needed for base classes and by-value
data members. Two ordinary classes can refer to each other through pointers or
references. Association and dependency connectors describe those references
visually. The converter uses the declared C++ types to generate the code.

## Generated code and limits

Constructors, destructors and void operations have TODO bodies. Concrete
non-void operations use `std::terminate()` until implemented. Constant-evaluated
operations use a value-initialized result or a matching argument for a
reference result. Pure virtual, defaulted and deleted functions keep their
declaration form. A base constructor with an unambiguous matching set of
parameters can be selected automatically. Use an explicit initializer list
when constructor arguments or reference members need a different expression.

Skeletons target C++17 or later. Use C++20 when the diagram uses C++20 syntax
such as `consteval`. The diagram retains declarations, visible defaults and
initializer information. It does not encode implementations, free functions,
build scripts, arbitrary macros, using declarations or every attribute.
Some defaults and initializers may refer to names that must be supplied by
the original project or replaced in the skeleton. An external include may
need the project's compiler search paths.

Partial and explicit class template specializations, class template
`requires` clauses, and unresolved deduced return types support forward
conversion only. Complex dependent declarations, type aliases nested within
signature types and non-default-constructible static data can require manual
source changes. Some template static declarations and reference statics need
a definition before odr-use. The end-to-end tests compile and link the supported
examples against a real C++ compiler.
