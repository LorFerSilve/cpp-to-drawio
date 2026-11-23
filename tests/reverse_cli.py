import base64
import os
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from urllib.parse import quote
from xml.etree import ElementTree

from cli_support import catalog, database, entry, invoke


CLI, COMPILER, REVERSE = sys.argv[1:4]
del sys.argv[1:4]


def graph(cells):
    return '<mxGraphModel><root><mxCell id="0"/><mxCell id="1" parent="0"/>' + cells + "</root></mxGraphModel>"


def compressed_page(xml):
    deflater = zlib.compressobj(wbits=-zlib.MAX_WBITS)
    encoded = quote(xml, safe="~()*!.'-").encode()
    return base64.b64encode(deflater.compress(encoded) + deflater.flush()).decode()


class ReverseCli(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="drawio-to-cpp tests ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.diagram = self.root / "Architecture diagram.drawio"
        self.output = self.root / "generated sources"

    def run_cli(self, binary, *args, status=0):
        result = invoke(binary, *args)
        self.assertEqual(result.returncode, status, result.stdout + result.stderr)
        return result

    def reverse(self, *args, status=0, via_subcommand=False, output=None):
        options = ("--input", self.diagram, "--output", output or self.output, *args)
        if via_subcommand:
            return self.run_cli(CLI, "drawio-to-cpp", *options, status=status)
        return self.run_cli(REVERSE, *options, status=status)

    def compile(self, source, standard="c++17", driver=None):
        driver_file = self.root / "driver.cpp"
        driver_file.write_text(driver or "int main() {}\n", encoding="utf-8")
        executable = self.root / ("skeletons.exe" if os.name == "nt" else "skeletons")
        files = sorted(source.rglob("*.cpp"))
        self.assertTrue(files)
        self.run_cli(COMPILER, "-std=" + standard, "-Werror=return-type", "-I", source,
                     *files, driver_file, "-o", executable)

    def test_round_trip_exports_compile_and_link_in_both_page_modes(self):
        project, db = catalog(self.root, COMPILER)
        driver = """
            #include "shop/Order.hpp"
            #include "stock/Entry.hpp"
            #include "audit/Trail.hpp"
            #include <type_traits>
            #include <utility>
            static_assert(std::is_same_v<decltype(std::declval<shop::Order &>().total(3.5)), int>);
            static_assert(std::is_same_v<decltype(std::declval<shop::Shelf<stock::Entry>&>().at(0)), stock::Entry &>);
            static_assert(std::is_same_v<decltype(std::declval<shop::Shelf<stock::Entry>>().convert(3)), int>);
            static_assert(static_cast<unsigned>(stock::Availability::retired) == 5);
            int main() {
                stock::Entry entry{};
                shop::Order order(7, entry);
                return shop::Order::count + entry.code();
            }
        """
        for overview in (False, True):
            with self.subTest(overview=overview):
                options = ("--overview",) if overview else ()
                self.run_cli(CLI, "--source", project, "--output", self.diagram,
                             "--compile-db", db, "--force", *options)
                source = self.root / ("overview" if overview else "single diagram")
                report = self.reverse(output=source, via_subcommand=overview)
                self.assertIn("Generated 16 C++ files from 9 UML types", report.stdout)
                header = (source / "shop" / "Order.hpp").read_text(encoding="utf-8")
                implementation = (source / "shop" / "Order.cpp").read_text(encoding="utf-8")
                self.assertIn("virtual ", header)
                self.assertIn("Readable", header)
                self.assertIn("const & noexcept override", header)
                self.assertIn("int (*scorer_)(int)", header)
                self.assertIn("entry_(original)", implementation)
                self.assertIn("Order(original.sku, original)", implementation)
                self.assertIn("struct Audit", header)
                self.assertFalse((source / "shop" / "Audit.hpp").exists())
                self.compile(source, "c++20" if overview else "c++17", driver)

    def test_doxygen_uses_declaration_names_and_escapes_author(self):
        project, _ = catalog(self.root, COMPILER)
        self.run_cli(CLI, "--source", project, "--output", self.diagram)
        self.reverse("--author", "Élodie & team <CPP> */ @todo \\")
        shelf = (self.output / "shop" / "Shelf.hpp").read_text(encoding="utf-8")
        self.assertIn("@author Élodie &amp; team &lt;CPP&gt; *&#47; &#64;todo &#92;", shelf)
        self.assertIn("@tparam T ", shelf)
        self.assertIn("@tparam N ", shelf)
        self.assertIn("@tparam U ", shelf)
        self.assertIn("@param item ", shelf)
        self.assertIn("@return TODO", shelf)
        order = (self.output / "shop" / "Order.hpp").read_text(encoding="utf-8")
        for comment in order.split("/**")[1:]:
            text = comment.split("*/", 1)[0]
            if "this constructor" in text:
                self.assertIn("@param original ", text)
                self.assertNotIn("@return", text)
        availability = (self.output / "stock" / "Availability.hpp").read_text(encoding="utf-8")
        self.assertIn("TODO Describe the purpose of enumerator retired", availability)
        self.compile(self.output)

    def test_reads_compressed_handmade_uml_with_native_rows_and_inheritance(self):
        xml = graph("""
            <mxCell id="ns" value="namespace shop" style="swimlane;" parent="1" vertex="1"/>
            <mxCell id="Base" value="«struct»&lt;br&gt;+ stock::Base&amp;lt;class T&amp;gt;&lt;br&gt;
                # Base(id: int)&lt;br&gt;+ get(): T {const}" style="html=1;" parent="1" vertex="1"/>
            <mxCell id="Tag" value="«struct» + stock::Tag" parent="1" vertex="1"/>
            <object id="Order" label="+ Order&lt;div&gt;base public ::stock::Base&amp;lt;int&amp;gt;&lt;/div&gt;
                &lt;div&gt;#include &amp;lt;vector&amp;gt;&lt;/div&gt;">
              <mxCell parent="ns" style="swimlane;childLayout=stackLayout;html=1;" vertex="1"/>
            </object>
            <mxCell id="fields" value="- labels: std::vector&amp;lt;int&amp;gt;&lt;br&gt;&lt;u&gt;+ count: int&lt;/u&gt;"
                style="text;html=1;" parent="Order" vertex="1"/>
            <mxCell id="ctor" value="+ Order(id: int)" style="text;html=1;" parent="Order" vertex="1"/>
            <mxCell id="op" value="+ clear(): void" style="text;html=1;" parent="Order" vertex="1"/>
            <mxCell id="base" style="startArrow=blockThin;startFill=0;" source="Base" target="ctor" edge="1" parent="1"/>
            <mxCell id="tag" style="endArrow=block;endFill=0;" source="Order" target="Tag" edge="1" parent="1"/>
            <mxCell id="note" style="shape=note;html=1;" value="Note: discuss later" parent="1" vertex="1"/>
        """).replace("\n", "")
        self.diagram.write_text('<mxfile><diagram name="Orders">' + compressed_page(xml) + "</diagram></mxfile>",
                                encoding="utf-8")
        self.reverse()
        header = (self.output / "shop" / "Order.hpp").read_text(encoding="utf-8")
        implementation = (self.output / "shop" / "Order.cpp").read_text(encoding="utf-8")
        self.assertIn("public ::stock::Base<int>, public ::stock::Tag", header)
        self.assertIn("std::vector<int> labels;", header)
        self.assertIn("static int count;", header)
        self.assertIn("::stock::Base<int>(id)", implementation)
        self.assertNotIn("@author", header)
        self.assertNotIn("@return", header)
        self.compile(self.output, driver='#include "shop/Order.hpp"\nint main() { shop::Order order(5); }\n')

    def test_cyclic_references_use_forward_declarations(self):
        self.diagram.write_text(graph("""
            <mxCell id="A" value="«struct» + cycle::A&#10;- next: B *&#10;+ read(value: B): B" parent="1" vertex="1"/>
            <mxCell id="B" value="«struct» + cycle::B&#10;- next: A *&#10;+ read(value: A): A" parent="1" vertex="1"/>
        """), encoding="utf-8")
        self.reverse()
        first = (self.output / "cycle" / "A.hpp").read_text(encoding="utf-8")
        self.assertIn("struct B;", first)
        self.assertNotIn('#include "B.hpp"', first)
        self.compile(self.output)

    def test_nested_dependencies_and_cpp20_signatures_in_compressed_export(self):
        project = self.root / "advanced project"
        project.mkdir()
        (project / "advanced.cpp").write_text("""
            #include <utility>
            namespace advanced {
            struct Store {
                struct Link;
                struct Value { int data; Link* next; };
                struct Link { Value current; };
                Value value;
                explicit Store(Value input) noexcept : value(std::move(input)) {}
                int operator()(int limit = 1'000) const & noexcept(false) { return limit; }
                explicit operator bool() const noexcept { return true; }
                consteval static int version() { return 1; }
                auto size() const { return 0u; }
            };
            }
        """, encoding="utf-8")
        database(project, [entry(COMPILER, project, "advanced.cpp", "-std=c++20")])
        self.run_cli(CLI, "--source", project, "--output", self.diagram, "--overview")
        document = ElementTree.parse(self.diagram)
        for page in document.getroot():
            model = page.find("mxGraphModel")
            page.remove(model)
            page.text = compressed_page(ElementTree.tostring(model, encoding="unicode"))
        document.getroot().attrib["compressed"] = "true"
        document.write(self.diagram, encoding="utf-8")
        self.reverse()
        header = (self.output / "advanced" / "Store.hpp").read_text(encoding="utf-8")
        self.assertIn("#include <utility>", header)
        self.assertLess(header.index("struct Value {"), header.index("struct Link {"))
        self.assertIn("operator()(int limit = ", header)
        self.assertIn("const & noexcept(false)", header)
        self.assertIn("explicit operator bool()", header)
        self.assertIn("consteval", header)
        self.assertIn("unsigned int size() const;", header)
        self.compile(self.output, "c++20", """
            #include "advanced/Store.hpp"
            static_assert(advanced::Store::version() == 0);
            int main() { advanced::Store store({1, nullptr}); }
        """)

    def test_does_not_publish_any_files_on_conflict(self):
        self.diagram.write_text(graph("""
            <mxCell id="new" value="+ shop::Fresh" parent="1" vertex="1"/>
            <mxCell id="old" value="+ shop::Existing" parent="1" vertex="1"/>
        """), encoding="utf-8")
        existing = self.output / "shop" / "Existing.hpp"
        existing.parent.mkdir(parents=True)
        existing.write_bytes(b"// Keep me")
        result = self.reverse(status=4)
        self.assertIn("Existing.hpp", result.stderr)
        self.assertEqual(existing.read_bytes(), b"// Keep me")
        self.assertEqual(list(self.output.rglob("*.*")), [existing])
        alternate = self.root / "pre-existing output"
        alternate.mkdir()
        self.reverse(output=alternate)
        self.compile(alternate)

    def test_rejects_missing_detail_and_conflicting_boxes(self):
        project, _ = catalog(self.root, COMPILER)
        self.run_cli(CLI, "--source", project, "--output", self.diagram, "--overview")
        document = ElementTree.parse(self.diagram)
        document.getroot().remove(next(p for p in document.getroot() if p.attrib["name"] == "stock"))
        document.write(self.diagram, encoding="utf-8")
        self.assertIn("No detail box for stock::", self.reverse(status=3).stderr)
        self.assertFalse(self.output.exists())
        self.diagram.write_text(graph("""
            <mxCell id="a" value="+ shop::Order&#10;- id: int" parent="1" vertex="1"/>
            <mxCell id="b" value="+ shop::Order&#10;- id: long" parent="1" vertex="1"/>
        """), encoding="utf-8")
        self.assertIn("Conflicting declarations for shop::Order", self.reverse(status=3).stderr)
        self.assertFalse(self.output.exists())

    def test_invalid_uml_reports_page_and_cell(self):
        self.diagram.write_text('<mxfile><diagram name="Draft">' + graph(
            '<mxCell id="manual-type" value="+ shop::Order&#10;+ total()" parent="1" vertex="1"/>'
        ) + "</diagram></mxfile>", encoding="utf-8")
        result = self.reverse(status=3)
        self.assertIn("Page 'Draft', cell 'manual-type'", result.stderr)
        self.assertIn("Specify the return type of 'total'", result.stderr)
        self.assertFalse(self.output.exists())

    def test_rejects_non_uml_and_invalid_xml(self):
        self.diagram.write_text(graph('<mxCell id="flow" value="Start" parent="1" vertex="1"/>'), encoding="utf-8")
        self.assertIn("No UML class boxes found", self.reverse(status=3).stderr)
        self.diagram.write_text("<!DOCTYPE mxGraphModel>" + graph(""), encoding="utf-8")
        self.assertIn("DOCTYPE", self.reverse(status=3).stderr)
        self.diagram.write_text('<mxfile><diagram name="Broken">AAAA</diagram></mxfile>', encoding="utf-8")
        self.assertIn("Page 'Broken'", self.reverse(status=3).stderr)
        self.assertFalse(self.output.exists())

    def test_help_and_author_validation(self):
        for executable, extra in ((REVERSE, ()), (CLI, ("drawio-to-cpp",))):
            self.assertIn("--author", self.run_cli(executable, *extra, "--help").stdout)
        self.diagram.write_text(graph('<mxCell id="a" value="+ Example" parent="1" vertex="1"/>'), encoding="utf-8")
        self.assertIn("Missing name after --author", self.reverse("--author", status=2).stderr)
        self.assertIn("a name on one line", self.reverse("--author", "first\u2028second", status=2).stderr)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
