import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

from cli_support import catalog, database, entry, invoke, pages


CLI, COMPILER = sys.argv[1:3]
del sys.argv[1:3]


class ForwardCli(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="cpp-to-drawio tests ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.project, self.db = catalog(self.root, COMPILER)
        self.diagram = self.root / "Catalog architecture.drawio"

    def run_cli(self, *args, status=0):
        result = invoke(CLI, *args)
        self.assertEqual(result.returncode, status, result.stdout + result.stderr)
        return result

    def convert(self, *args, status=0, output=None):
        return self.run_cli("--source", self.project, "--output", output or self.diagram, *args, status=status)

    def assert_no_overlap(self, page):
        rects = [page.bounds(key) for key in page.vertices()]
        for i, (x1, y1, x2, y2) in enumerate(rects):
            for left, top, right, bottom in rects[i + 1:]:
                self.assertFalse(x1 < right and x2 > left and y1 < bottom and y2 > top)

    def test_help_and_arguments(self):
        help_text = self.run_cli("--help").stdout
        for flag in ("--source", "--output", "--compile-db", "--create-output-dir", "--overview", "--force"):
            self.assertIn(flag, help_text)
        self.assertIn("Unexpected argument --unknown", self.run_cli("--unknown", status=2).stderr)
        self.assertIn("Repeated argument --force", self.run_cli("--force", "--force", status=2).stderr)
        self.assertIn("must name a .drawio file", self.convert(output=self.root / "foo.xml", status=2).stderr)

    def test_compiles_project_and_writes_editable_deterministic_uml(self):
        for cmd in json.loads(self.db.read_text(encoding="utf-8")):
            args = cmd["arguments"]
            compiled = invoke(args[0], *args[1:-4], args[-3], "-fsyntax-only", cwd=cmd["directory"])
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)

        run = self.convert("--compile-db", self.db.parent)
        self.assertIn("Processed 2 C++ files, 9 UML types", run.stdout)
        self.assertEqual(run.stderr, "")
        self.assertFalse((self.project / "unused object.o").exists())
        (diagram,) = pages(self.diagram)
        self.assert_no_overlap(diagram)
        self.assertEqual(set(diagram.vertices()), {
            "type:audit::Trail", "type:GlobalRecord", "type:shop::Order", "type:shop::Order::Audit",
            "type:shop::Shelf", "type:stock::Availability", "type:stock::Entry",
            "type:stock::Identified", "type:stock::Readable",
        })
        self.assertIn("swimlane;", diagram.cells["type:shop::Order"].attrib["style"])
        self.assertEqual(diagram.cells["methods:shop::Order"].attrib["parent"], "type:shop::Order")
        self.assertIn("childLayout=stackLayout", diagram.cells["type:shop::Order"].attrib["style"])
        self.assertIn("«enum class»", diagram.labels["type:stock::Availability"])
        self.assertIn("back_order = 4", diagram.labels["fields:stock::Availability"])
        self.assertIn("class T, ", diagram.labels["type:shop::Shelf"])
        self.assertIn("$[N]", diagram.labels["fields:shop::Shelf"])
        members = diagram.labels["methods:shop::Order"]
        self.assertEqual(members.count("total("), 2)
        for qualifier in ("{const}", "{virtual}", "{&amp;}", "{override}", "{noexcept}"):
            self.assertIn(qualifier, members)
        self.assertIn("{constexpr}", diagram.labels["methods:stock::Entry"])
        self.assertIn("{pure}", diagram.labels["methods:stock::Readable"])
        self.assertIn("entry_(original)", members)
        self.assertIn("scorer_: int (*$)(int)", diagram.labels["fields:shop::Order"])
        for source, target, kind, style in [
            ("shop::Order", "stock::Identified", "generalization", "endArrow=block;endFill=0;dashed=0;"),
            ("shop::Order", "stock::Readable", "generalization", "endArrow=block;endFill=0;dashed=0;"),
            ("shop::Order", "shop::Order", "association", "endArrow=none;dashed=0;"),
            ("shop::Order", "shop::Shelf", "association", "endArrow=none;dashed=0;"),
            ("shop::Order", "stock::Entry", "association", "endArrow=none;dashed=0;"),
            ("audit::Trail", "stock::Readable", "dependency", "endArrow=open;endFill=0;dashed=1;"),
        ]:
            edge = diagram.edges()[f"edge:{source}>{target}:{kind}"]
            self.assertEqual(edge.attrib["source"], "type:" + source)
            self.assertEqual(edge.attrib["target"], "type:" + target)
            self.assertIn(style, edge.attrib["style"])
        again = self.root / "Repeat.drawio"
        self.convert("--compile-db", self.db, output=again)
        self.assertEqual(self.diagram.read_bytes(), again.read_bytes())

    def test_overview_namespaces_have_compact_boxes_and_detailed_tabs(self):
        self.convert("--overview")
        overview, global_page, audit, shop, stock = pages(self.diagram)
        self.assertEqual([p.name for p in (overview, global_page, audit, shop, stock)],
                         ["Overview", "(global namespace)", "audit", "shop", "stock"])
        self.assertEqual(len(overview.vertices()), 9)
        self.assertTrue(overview.cells["type:shop::Order"].attrib["parent"].startswith("namespace:"))
        self.assertNotIn("methods:shop::Order", overview.cells)
        self.assertIn("total(", shop.labels["methods:shop::Order"])
        self.assertIn("edge:shop::Order>stock::Readable:generalization", overview.edges())
        self.assertNotIn("edge:shop::Order>stock::Readable:generalization", shop.edges())
        for page in (overview, global_page, audit, shop, stock):
            self.assert_no_overlap(page)
        repeated = self.root / "Repeat.drawio"
        self.convert("--overview", output=repeated)
        self.assertEqual(self.diagram.read_bytes(), repeated.read_bytes())

    def test_output_creation_and_force(self):
        output = self.root / "docs" / "architecture" / self.diagram.name
        self.assertIn("--create-output-dir", self.convert(status=4, output=output).stderr)
        self.assertFalse(output.parent.exists())
        self.convert("--create-output-dir", output=output)
        good = output.read_bytes()
        output.write_bytes(b"manual changes")
        self.assertIn("--force", self.convert(output=output, status=4).stderr)
        self.assertEqual(output.read_bytes(), b"manual changes")
        self.convert("--force", output=output)
        self.assertEqual(output.read_bytes(), good)

    def test_error_reports_source_line_without_creating_or_replacing_output(self):
        invalid = self.project / "src" / "broken file.cpp"
        invalid.write_text("// Source location\nclass Broken { void bad( };\n", encoding="utf-8")
        commands = json.loads(self.db.read_text(encoding="utf-8"))
        commands.append(entry(COMPILER, self.project, "src/broken file.cpp", "-std=c++17"))
        database(self.project, commands)
        absent = self.root / "unused directory" / self.diagram.name
        result = self.convert("--create-output-dir", output=absent, status=3)
        self.assertRegex(result.stderr, r"broken file\.cpp:2:\d+: error:")
        self.assertIn("Analysis failed", result.stderr)
        self.assertFalse(absent.parent.exists())
        self.diagram.write_bytes(b"keep me")
        self.convert("--force", status=3)
        self.assertEqual(self.diagram.read_bytes(), b"keep me")

    def test_partial_analysis_identifies_source_without_a_command(self):
        (self.project / "src" / "unconfigured.cpp").write_text("struct Unconfigured {};", encoding="utf-8")
        result = self.convert()
        self.assertIn("Analysis is incomplete", result.stderr)
        self.assertIn("unconfigured.cpp:1:1: No compilation command", result.stderr)
        self.assertNotIn("type:Unconfigured", pages(self.diagram)[0].vertices())

    def test_conflicting_configurations_are_not_merged(self):
        (self.project / "src" / "config.hpp").write_text(
            "struct Configuration {\n#ifdef FEATURE\n  int value;\n#endif\n};\n", encoding="utf-8"
        )
        cmds = []
        for file in ("left.cpp", "right.cpp"):
            (self.project / "src" / file).write_text('#include "config.hpp"\n', encoding="utf-8")
            flags = ["-std=c++17", "-DFEATURE"] if file == "left.cpp" else ["-std=c++17"]
            cmds.append(entry(COMPILER, self.project, "src/" + file, *flags))
        database(self.project, cmds)
        result = self.convert(status=3)
        self.assertIn("config.hpp:1:", result.stderr)
        self.assertIn("Conflicting definitions for Configuration", result.stderr)
        self.assertFalse(self.diagram.exists())

    def test_compile_database_errors(self):
        self.assertIn("Compilation database does not exist",
                      self.convert("--compile-db", self.root / "missing.json", status=2).stderr)
        self.db.write_text("{}", encoding="utf-8")
        self.assertIn(self.db.name, self.convert(status=2).stderr)
        self.db.write_text("[]", encoding="utf-8")
        self.assertIn("No C++ translation units", self.convert(status=2).stderr)
        self.assertFalse(self.diagram.exists())

    def test_command_strings_and_duplicate_entries(self):
        entries = json.loads(self.db.read_text(encoding="utf-8"))
        first = entries[0]
        compiler = str(Path(COMPILER)).replace(os.sep, "/")
        first["command"] = f'"{compiler}" -std=c++17 -I"include" -c "src/order.cpp"'
        del first["arguments"]
        database(self.project, [*entries, first])
        self.assertIn("Processed 2 C++ files, 9 UML types", self.convert().stdout)

    def test_template_specializations_and_their_edges(self):
        source = self.root / "specializations"
        source.mkdir()
        (source / "templates.cpp").write_text("""
            namespace meta {
            template <class T> struct Holder { T item; };
            template <class T> struct Holder<T*> { T* item; };
            template <> struct Holder<int> { long item; };
            struct Use { Holder<int> explicit_; Holder<double*> partial; Holder<char> primary; };
            }
        """, encoding="utf-8")
        database(source, [entry(COMPILER, source, "templates.cpp", "-std=c++17")])
        self.run_cli("--source", source, "--output", self.diagram)
        (page,) = pages(self.diagram)
        self.assertEqual(len(page.vertices()), 4)
        partial = next(name for name in page.vertices() if name.startswith("type:meta::Holder<") and "*" in name)
        for target in ("type:meta::Holder<int>", partial, "type:meta::Holder"):
            self.assertTrue(any(edge.attrib["source"] == "type:meta::Use" and edge.attrib["target"] == target
                                for edge in page.edges().values()), target)


if __name__ == "__main__":
    unittest.main()
