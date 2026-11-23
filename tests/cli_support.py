import json
import shutil
import subprocess
from pathlib import Path
from xml.etree import ElementTree


FIXTURE = Path(__file__).resolve().parent / "fixtures" / "catalog"


def invoke(executable, *args, **kwargs):
    return subprocess.run(
        [str(executable), *(str(arg) for arg in args)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        encoding="utf-8",
        errors="replace",
        timeout=60,
        **kwargs,
    )


def database(project, entries):
    folder = project / "build"
    folder.mkdir(exist_ok=True)
    output = folder / "compile_commands.json"
    output.write_text(json.dumps(entries, indent=2), encoding="utf-8")
    return output


def entry(compiler, project, file, *flags):
    return {
        "directory": project.as_posix(),
        "file": file,
        "arguments": [str(compiler), *flags, "-c", file, "-o", "unused object.o"],
    }


def catalog(root, compiler):
    project = root / "Catalog project"
    shutil.copytree(FIXTURE, project)
    define = "-DWITH_AUDIT=1"
    entries = [
        entry(compiler, project, "src/order.cpp", "-std=c++17", "-Iinclude"),
        entry(compiler, project, "src/audit.cpp", "-std=c++20", "-Iinclude", "-Iinclude options", define),
    ]
    return project, database(project, entries)


class Page:
    def __init__(self, element):
        self.name = element.attrib["name"]
        self.cells = {}
        self.labels = {}
        for node in element.find("./mxGraphModel/root"):
            if node.tag in ("UserObject", "object"):
                key = node.attrib["id"]
                self.cells[key] = node.find("mxCell")
                self.labels[key] = node.attrib.get("label", "")
            else:
                key = node.attrib["id"]
                self.cells[key] = node
                self.labels[key] = node.attrib.get("value", "")

    def vertices(self):
        return {key: cell for key, cell in self.cells.items() if key.startswith("type:")}

    def edges(self):
        return {key: cell for key, cell in self.cells.items() if cell.attrib.get("edge") == "1"}

    def bounds(self, key):
        cell = self.cells[key]
        item = cell.find("mxGeometry")
        x, y, width, height = (int(item.attrib[name]) for name in ("x", "y", "width", "height"))
        if cell.attrib["parent"].startswith("namespace:"):
            dx, dy, _, _ = self.bounds(cell.attrib["parent"])
            x += dx
            y += dy
        return x, y, x + width, y + height


def pages(path):
    return [Page(node) for node in ElementTree.parse(path).getroot().findall("diagram")]
