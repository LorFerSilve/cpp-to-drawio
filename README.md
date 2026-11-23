# C++ to Draw.io

Generate editable UML class diagrams from C++ projects or create C++ skeletons
with Doxygen comments from Draw.io diagrams. Both commands run locally.

## C++ to Draw.io

Generate a compilation database with your project's build system. For CMake, use
Ninja or a Makefile generator with `CMAKE_EXPORT_COMPILE_COMMANDS`. Configure with
the compiler and options you want the diagram to represent. Build the project
first if it needs generated headers.
[CMake compilation database option](https://cmake.org/cmake/help/latest/variable/CMAKE_EXPORT_COMPILE_COMMANDS.html)

```powershell
cmake -S "C:\Projects\My C++ Project" -B "C:\Projects\My C++ Project\build" -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
.\dist\bin\cpp-to-drawio.exe --source "C:\Projects\My C++ Project" --compile-db "C:\Projects\My C++ Project\build" --output ".\diagrams\architecture.drawio" --create-output-dir --overview
.\dist\bin\cpp-to-drawio.exe --help
```

Open the file in Draw.io Desktop or diagrams.net. You can move the class boxes,
edit compartment labels, and reconnect edges. With `--overview`, the first tab
groups types by namespace and includes all edges. Each namespace has a detail
tab with full signatures and its internal relationships.

| Argument | Purpose |
| --- | --- |
| `--source <directory>` | Select the project tree. Start at a root that contains both the sources and headers you want to diagram. |
| `--output <file.drawio>` | Write deterministic Draw.io XML. |
| `--compile-db <path>` | Use this compilation database file or the directory that contains it. Omit to search the source directory and its ancestors for `compile_commands.json` or `build/compile_commands.json`. |
| `--create-output-dir` | Create missing output directories after successful analysis. |
| `--overview` | Add a compact overview followed by detailed namespace pages. |
| `--no-static-analysis` | Skip the scan for potential defects. |
| `--force` | Replace an existing regular file, including any edits made in Draw.io. |
| `--help` | Show usage and exit. |

Clang uses the working directory, include paths, macros, language standard and
other compiler flags from each command. The summary counts translation units,
types, relationships and warnings. Diagnostics go to standard error with
file, line and column where available.

Each export also runs the Clang Static Analyzer on the parsed code. It reports
possible defects such as null pointer dereferences, use after free, memory
leaks and unsafe library calls. Findings include a source location and a
checker ID, and the summary gives their count. The diagram is written when
the source parses successfully, even when the scan finds issues.
[Analysis and scan details](docs/analysis.md)

| Exit code | Meaning |
| --- | --- |
| `0` | Output written or help displayed. Files without a compilation command can be skipped with a warning. |
| `2` | Invalid arguments or input path, invalid compilation database, or no matching C++ sources. |
| `3` | Clang analysis failed, configurations disagree on a type, or reverse input contains invalid XML or UML. |
| `4` | The converter could not safely write the requested output. |

## Draw.io to C++

```powershell
.\dist\bin\drawio-to-cpp.exe --input ".\diagrams\architecture.drawio" --output ".\generated sources" --author "First Last"
.\dist\bin\cpp-to-drawio.exe drawio-to-cpp --help
```

The converter reads plain and compressed diagrams. It combines detailed pages
and uses overview connectors for relationships across namespaces. It creates
one `.hpp` and one `.cpp` for each top-level type under its namespace path.
Nested types stay in the enclosing header. A conflict with an existing output
file stops conversion before publishing any new file.

Use a UML class shape or a box with one declaration per line. For example,
this box generates `shop/Order.hpp` and `shop/Order.cpp`.

```text
+ shop::Order
- quantity: int {init 0}
+ Order(quantity: int) {init quantity(quantity)}
+ total(unitPrice: int): int {const} {noexcept}
```

Every generated type, field, enumerator and operation has a Doxygen template.
`--author` adds an author tag to each type. Template parameters, arguments
and results get `@tparam`, `@param` and `@return` where applicable.
See the [reverse conversion guide](docs/reverse-conversion.md) for native rows,
typed base classes, includes and other UML notation.

## Supported C++ constructs

| Construct | Diagram | Skeleton |
| --- | --- | --- |
| Classes, structs, unions and named nested types | Declared types and members with access markers. | Namespace headers, nested definitions and access sections. |
| Scoped and unscoped enums | Enumerators, explicit values and fixed underlying types. | Enum declarations with the same values and underlying types. |
| Fields and operations | Static and instance fields, arrays, bit fields, overloaded methods, constructors and destructors. | Member declarations, parameter defaults, constructor initializer lists and TODO definitions. |
| Operation qualifiers | `const`, `volatile`, ref qualifiers, `noexcept`, `virtual`, `override`, `final`, `explicit`, `constexpr`, `consteval`, and pure, defaulted or deleted operations. | Retained where the skeleton can express the declaration. |
| Primary templates | Class and member templates, including type and non-type parameters and defaults. | Templates and constant-evaluated definitions live in the header. |
| Partial and explicit class template specializations | Separate class boxes and resolved relationships. | Forward conversion only. |
| Multiple inheritance | One hollow-triangle connector per base, with typed bases, access and virtual inheritance retained in shape data. | Typed bases and editable generalization arrows. |

## Build and test

Use CMake 3.21 or newer, a C++20 compiler, matching LLVM and Clang development
packages (18 or newer), TinyXML2, zlib and Python 3.8 or newer for the tests.

On Windows, install these packages in an MSYS2 **CLANG64** terminal.

```bash
pacman -Syu
pacman -S --needed mingw-w64-clang-x86_64-{clang,clang-tools-extra,llvm,cmake,ninja,python,tinyxml2,zlib}
```

From the repository in PowerShell, build and install both commands.

```powershell
$clangRoot = 'C:\msys64\clang64'
$env:Path = "$clangRoot\bin;$env:Path"
cmake -S . -B build -G Ninja "-DCMAKE_PREFIX_PATH=$clangRoot" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix dist
```

On Ubuntu 24.04, use the distribution LLVM 18 development packages.

```bash
sudo apt-get install cmake ninja-build clang-18 llvm-18-dev libclang-18-dev libclang-cpp18-dev libtinyxml2-dev zlib1g-dev python3
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 -DCMAKE_PREFIX_PATH=/usr/lib/llvm-18
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix dist
./dist/bin/cpp-to-drawio --help
```

The Windows install includes the converter's runtime DLLs. The Linux install
uses the distribution's shared libraries. Both installs include the matching
Clang built-in headers. Forward analysis also needs the target project's
standard library, SDK and any external headers used by its compilation database.

Local end-to-end validation used Clang **23.1.5** on Windows x64 (MSYS2 CLANG64)
and Clang **18.1.8** on Linux x64 (Ubuntu 20.04 in WSL). Tests analyze C++17 and
C++20 translation units with different flags, verify the XML's editable shapes
and links, and compile and link generated C++17 and C++20 sources. GitHub Actions
builds and runs the same suites on Windows and Linux.

## Differences from Java to Draw.io

The CLI conventions, three-part class boxes, layout and integration tests use
the fixed [Java to Draw.io v0.4.0](https://github.com/LorFerSilve/java-to-drawio/tree/v0.4.0)
reference.

- Clang analyzes configured translation units and their included headers. The
  compilation database is the C++ equivalent of providing both source files and
  dependency configuration. Conflicting preprocessor variants of a type need
  separate runs.
- C++ groups by namespace and treats inheritance as generalization, including
  pure virtual bases. Access and virtual inheritance need typed base clauses.
- C++ skeletons need headers, declarations shared across translation units, and
  template definitions visible at instantiation. Doxygen tags use C++ template
  and argument names. See [analysis and diagram rules](docs/analysis.md) and
  [reverse conversion limits](docs/reverse-conversion.md).

## License

[MIT](LICENSE)
