#include "drawio_writer.hpp"
#include "diagram_layout.hpp"

#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>
#include <tinyxml2.h>

#include <algorithm>
#include <cmath>
#include <map>

namespace drawio {
namespace {

using tinyxml2::XMLElement;

constexpr std::string_view FONT = "fontSize=12;fontFamily=monospace;";
constexpr int LINE_HEIGHT = 18;
constexpr std::string_view COMPARTMENT = "text;html=1;whiteSpace=wrap;align=left;verticalAlign=top;"
    "spacingTop=4;spacingLeft=10;spacingRight=10;fillColor=none;strokeColor=none;rotatable=0;";

std::string escapeHtml(std::string_view text) {
    std::string escaped;
    for (char ch : text) {
        switch (ch) {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            default: escaped += ch; break;
        }
    }
    return escaped;
}

struct Line {
    std::string text;
    bool bold = false;
    bool italic = false;
    bool underlined = false;
};

struct Caption {
    std::vector<Line> lines;

    std::string html() const {
        std::string result;
        for (const auto& line : lines) {
            std::string text = escapeHtml(line.text);
            if (line.bold) {
                text = "<b>" + text + "</b>";
            }
            if (line.italic) {
                text = "<i>" + text + "</i>";
            }
            if (line.underlined) {
                text = "<u>" + text + "</u>";
            }
            result += "<div style=\"line-height:18px;overflow-wrap:anywhere\">" + text + "</div>";
        }
        return result;
    }

    int width() const {
        std::size_t longest = 0;
        for (const auto& line : lines) {
            longest = std::max(longest, line.text.size());
        }
        return static_cast<int>(std::min<std::size_t>(90, longest)) * 8 + 28;
    }

    int height(int width) const {
        int columns = std::max(1, (width - 28) / 8);
        int rows = 0;
        for (const auto& line : lines) {
            rows += std::max(1, static_cast<int>(std::ceil(static_cast<double>(line.text.size()) / columns)));
        }
        return 8 + LINE_HEIGHT * std::max(1, rows);
    }
};

std::string stereotype(Kind kind) {
    switch (kind) {
        case Kind::Class: return {};
        case Kind::Struct: return "«struct»";
        case Kind::Union: return "«union»";
        case Kind::Enum: return "«enumeration»";
        case Kind::ScopedEnum: return "«enum class»";
    }
    return {};
}

std::string colors(const UmlType& type) {
    if (isEnum(type.kind)) {
        return "fillColor=#fef3c7;strokeColor=#9d773a;";
    }
    if (type.abstract) {
        return "fillColor=#ede9fe;strokeColor=#7962a8;";
    }
    if (type.kind == Kind::Struct || type.kind == Kind::Union) {
        return "fillColor=#dcfce7;strokeColor=#52866a;";
    }
    return "fillColor=#dbeafe;strokeColor=#5478a9;";
}

std::string templates(const std::vector<std::string>& parameters) {
    return parameters.empty() ? "" : "<" + join(parameters, ", ") + ">";
}

std::string decorations(const std::vector<std::string>& modifiers) {
    std::string result;
    for (const auto& modifier : modifiers) {
        result += " {" + modifier + "}";
    }
    return result;
}

Caption title(const UmlType& type, bool compact) {
    Caption result;
    auto role = stereotype(type.kind);
    if (!role.empty()) {
        result.lines.push_back({std::move(role)});
    }
    std::string name = std::string(1, accessMarker(type.access)) + " " + type.displayName();
    if (!compact && !type.templates.empty()) {
        if (type.specialization.empty()) {
            name += templates(type.templates);
        } else {
            name = std::string(1, accessMarker(type.access)) + " " + templates(type.templates) + " " + type.displayName();
        }
    }
    if (type.final) {
        name += " {final}";
    }
    result.lines.push_back({std::move(name), true, type.abstract});
    if (!compact && !type.nameSpace.empty()) {
        result.lines.push_back({"namespace " + type.nameSpace});
    }
    if (!compact && !type.underlying.empty()) {
        result.lines.push_back({"underlying " + type.underlying});
    }
    return result;
}

Caption fieldLabels(const UmlType& type) {
    Caption result;
    for (const auto& entry : type.enumerators) {
        result.lines.push_back({entry.name + (entry.value.empty() ? "" : " = " + entry.value)});
    }
    for (const auto& field : type.fields) {
        std::string text = std::string(1, accessMarker(field.access)) + " " + field.name + ": " + field.type;
        text += decorations(field.modifiers);
        if (!field.bitWidth.empty()) {
            text += " {bits " + field.bitWidth + "}";
        }
        if (!field.initializer.empty()) {
            text += " {init " + field.initializer + "}";
        }
        result.lines.push_back({std::move(text), false, false, hasModifier(field, "static")});
    }
    return result;
}

Caption methodLabels(const UmlType& type) {
    Caption result;
    for (const auto& operation : type.operations) {
        std::string text = std::string(1, accessMarker(operation.access)) + " ";
        if (!operation.templates.empty()) {
            text += templates(operation.templates) + " ";
        }
        std::vector<std::string> parameters;
        for (const auto& parameter : operation.parameters) {
            parameters.push_back(parameter.name.empty() ? parameter.type : parameter.name + ": " + parameter.type);
            if (!parameter.defaultValue.empty()) {
                parameters.back() += " = " + parameter.defaultValue;
            }
        }
        text += operation.name + "(" + join(parameters, ", ") + ")";
        if (!operation.result.empty()) {
            text += ": " + operation.result;
        }
        text += decorations(operation.modifiers);
        if (operation.initializers && !operation.initializers->empty()) {
            text += " {init " + *operation.initializers + "}";
        }
        result.lines.push_back({std::move(text), false, hasModifier(operation, "pure"),
                                hasModifier(operation, "static")});
    }
    return result;
}

struct ClassLabels {
    Caption header;
    Caption fields;
    Caption methods;

    Size size() const {
        int width = std::max({184, header.width(), fields.width(), methods.width()});
        return {width, header.height(width) + fields.height(width) + methods.height(width) + 1};
    }
};

XMLElement* element(tinyxml2::XMLNode* parent, const char* tag,
                    std::initializer_list<std::pair<const char*, std::string>> attributes = {}) {
    auto* node = parent->GetDocument()->NewElement(tag);
    for (const auto& [key, value] : attributes) {
        node->SetAttribute(key, value.c_str());
    }
    parent->InsertEndChild(node);
    return node;
}

XMLElement* page(XMLElement* file, const std::string& id, const std::string& name) {
    auto* diagram = element(file, "diagram", {{"id", id}, {"name", name}});
    auto* graph = element(diagram, "mxGraphModel", {{"grid", "1"}, {"gridSize", "10"}, {"page", "0"}});
    auto* root = element(graph, "root");
    element(root, "mxCell", {{"id", "0"}});
    element(root, "mxCell", {{"id", "1"}, {"parent", "0"}});
    return root;
}

void geometry(XMLElement* cell, Point position, Size size) {
    auto* node = element(cell, "mxGeometry", {{"as", "geometry"}});
    node->SetAttribute("x", position.x);
    node->SetAttribute("y", position.y);
    node->SetAttribute("width", size.width);
    node->SetAttribute("height", size.height);
}

XMLElement* vertex(XMLElement* root, const std::string& id, const std::string& parent, const std::string& value,
                   const std::string& style, Point position, Size size) {
    auto* cell = element(root, "mxCell", {{"id", id}, {"parent", parent}, {"value", value},
                                         {"style", style}, {"vertex", "1"}});
    geometry(cell, position, size);
    return cell;
}

std::string encodeList(const auto& values) {
    llvm::json::Array array;
    for (const auto& value : values) {
        array.push_back(value);
    }
    std::string text;
    llvm::raw_string_ostream output(text);
    output << llvm::json::Value(std::move(array));
    return text;
}

void drawType(XMLElement* root, const UmlType& type, const ClassLabels& labels, const Box& box) {
    auto name = type.qualifiedName();
    int width = box.size.width;
    int headerHeight = labels.header.height(width);
    int fieldsHeight = labels.fields.height(width);
    int dividerY = headerHeight + fieldsHeight;
    std::string style = "swimlane;html=1;whiteSpace=wrap;rounded=0;horizontal=1;collapsible=0;"
        "childLayout=stackLayout;horizontalStack=0;resizeParent=0;resizeLast=1;container=1;dropTarget=0;"
        "align=center;verticalAlign=middle;swimlaneFillColor=#ffffff;spacing=0;startSize=";
    style += std::to_string(headerHeight) + ";" + std::string(FONT) + colors(type);
    auto* object = element(root, "UserObject", {{"id", "type:" + name}, {"label", labels.header.html()},
        {"cppOwner", type.owner}, {"cppBases", encodeList(type.bases)}, {"cppIncludes", encodeList(type.includes)},
        {"cppLimitations", encodeList(type.reverseLimitations)}, {"cppSpecialization", type.specialization}});
    auto* cell = element(object, "mxCell", {{"style", style}, {"vertex", "1"}, {"parent", "1"}});
    geometry(cell, box.position, box.size);
    std::string compartment = std::string(COMPARTMENT) + std::string(FONT);
    vertex(root, "fields:" + name, "type:" + name, labels.fields.html(), compartment,
           {0, headerHeight}, {width, fieldsHeight})->SetAttribute("connectable", "0");
    vertex(root, "divider:" + name, "type:" + name, "", "line;strokeWidth=1;editable=0;rotatable=0;" + colors(type),
           {0, dividerY}, {width, 1})->SetAttribute("connectable", "0");
    vertex(root, "methods:" + name, "type:" + name, labels.methods.html(), compartment,
           {0, dividerY + 1}, {width, labels.methods.height(width)})->SetAttribute("connectable", "0");
}

void drawEdges(XMLElement* root, const std::vector<Edge>& edges, const auto& route) {
    using Pair = std::pair<std::string, std::string>;
    auto pair = [](const Edge& edge) -> Pair { return std::minmax(edge.source, edge.target); };
    std::map<Pair, int> totals;
    std::map<Pair, int> used;
    for (const auto& edge : edges) {
        ++totals[pair(edge)];
    }
    for (const auto& edge : edges) {
        auto key = pair(edge);
        auto path = route(edge, ++used[key] / (totals.at(key) + 1.0));
        std::string style = "edgeStyle=orthogonalEdgeStyle;orthogonalLoop=1;rounded=0;jettySize=auto;"
                            "startArrow=none;endSize=12;strokeColor=#475569;strokeWidth=1.4;";
        style += "exitX=" + std::to_string(path.exit.x) + ";exitY=" + std::to_string(path.exit.y)
               + ";entryX=" + std::to_string(path.entry.x) + ";entryY=" + std::to_string(path.entry.y) + ";";
        if (edge.source == edge.target) {
            style += "loopStyle=loopEdgeStyle;direction=east;segment=36;";
        }
        switch (edge.kind) {
            case EdgeKind::Generalization: style += "endArrow=block;endFill=0;dashed=0;"; break;
            case EdgeKind::Association: style += "endArrow=none;dashed=0;"; break;
            case EdgeKind::Dependency: style += "endArrow=open;endFill=0;dashed=1;"; break;
        }
        auto edgeId = "edge:" + edge.source + ">" + edge.target + ":" + std::string(edgeName(edge.kind));
        auto* cell = element(root, "mxCell", {{"id", edgeId}, {"parent", "1"}, {"edge", "1"}, {"style", style},
                                             {"source", "type:" + edge.source}, {"target", "type:" + edge.target}});
        auto* coords = element(cell, "mxGeometry", {{"relative", "1"}, {"as", "geometry"}});
        if (!path.bends.empty()) {
            auto* points = element(coords, "Array", {{"as", "points"}});
            for (auto point : path.bends) {
                auto* node = element(points, "mxPoint");
                node->SetAttribute("x", point.x);
                node->SetAttribute("y", point.y);
            }
        }
    }
}

using Types = std::vector<const UmlType*>;

void detail(XMLElement* root, const Types& types, const std::vector<Edge>& edges) {
    std::map<std::string, ClassLabels> labels;
    std::map<std::string, Size> sizes;
    for (const auto* type : types) {
        auto name = type->qualifiedName();
        auto [entry, inserted] = labels.emplace(name, ClassLabels{title(*type, false), fieldLabels(*type),
                                                                   methodLabels(*type)});
        sizes.emplace(name, entry->second.size());
    }
    DiagramLayout layout(sizes, edges);
    drawEdges(root, edges, [&](const Edge& edge, double lane) { return layout.route(edge, lane); });
    for (const auto* type : types) {
        auto name = type->qualifiedName();
        drawType(root, *type, labels.at(name), layout.box(name));
    }
}

std::string displayNamespace(const std::string& name) {
    return name.empty() ? "(global namespace)" : name;
}

constexpr int NAMESPACE_HEADER = 30;

void overview(XMLElement* file, const UmlModel& model) {
    auto* root = page(file, "overview", "Overview");
    std::map<std::string, Types> namespaces;
    std::map<std::string, std::string> namespaceOf;
    for (const auto& type : model.types) {
        namespaces[type.nameSpace].push_back(&type);
        namespaceOf.emplace(type.qualifiedName(), type.nameSpace);
    }
    std::map<std::string, std::vector<Edge>> internal;
    std::vector<Edge> external;
    for (const auto& edge : model.edges) {
        const auto& from = namespaceOf.at(edge.source);
        const auto& to = namespaceOf.at(edge.target);
        if (from == to) {
            internal[from].push_back(edge);
        } else {
            external.push_back({from, to, edge.kind});
        }
    }
    std::map<std::string, Caption> headers;
    std::map<std::string, DiagramLayout> layouts;
    std::map<std::string, Size> frameSizes;
    for (const auto& [name, types] : namespaces) {
        std::map<std::string, Size> sizes;
        for (const auto* type : types) {
            auto key = type->qualifiedName();
            auto [header, inserted] = headers.emplace(key, title(*type, true));
            int width = std::max(152, header->second.width());
            sizes.emplace(key, Size{width, std::max(40, header->second.height(width))});
        }
        auto [layout, inserted] = layouts.try_emplace(name, sizes, internal[name], true);
        Size size = layout->second.bounds();
        size.width = std::max(size.width, static_cast<int>(displayNamespace(name).size()) * 8 + 32);
        size.height += NAMESPACE_HEADER;
        frameSizes.emplace(name, size);
    }
    DiagramLayout frames(frameSizes, external);
    drawEdges(root, model.edges, [&](const Edge& edge, double lane) {
        const auto& from = namespaceOf.at(edge.source);
        const auto& to = namespaceOf.at(edge.target);
        const auto& frame = frames.box(from);
        if (from == to) {
            auto path = layouts.at(from).route(edge, lane);
            for (auto& point : path.bends) {
                point.x += frame.position.x;
                point.y += frame.position.y + NAMESPACE_HEADER;
            }
            return path;
        }
        int offset = laneOffset(edge);
        auto bend = [&](const std::string& group, const std::string& type) {
            const auto& outer = frames.box(group);
            const auto& layout = layouts.at(group);
            const auto& box = layout.box(type);
            return Point{outer.position.x + box.position.x + static_cast<int>(std::round(box.size.width * lane)),
                         outer.position.y + NAMESPACE_HEADER + layout.aboveRow(box.row, offset)};
        };
        auto start = bend(from, edge.source);
        auto end = bend(to, edge.target);
        int left = frames.leftOf(frame, offset);
        int right = frames.leftOf(frames.box(to), offset);
        int above = frames.aboveRow(frame.row, offset);
        return Route{{lane, 0}, {lane, 0}, {start, {left, start.y}, {left, above}, {right, above},
                                           {right, end.y}, end}};
    });
    for (const auto& [name, types] : namespaces) {
        auto id = "namespace:" + name;
        const auto& frame = frames.box(name);
        std::string style = "swimlane;html=1;horizontal=1;collapsible=0;container=1;dropTarget=0;"
            "fillColor=#e2e8f0;strokeColor=#94a3b8;swimlaneFillColor=none;startSize="
            + std::to_string(NAMESPACE_HEADER) + ";" + std::string(FONT);
        vertex(root, id, "1", "<b>" + escapeHtml(displayNamespace(name)) + "</b>", style,
               frame.position, frame.size)->SetAttribute("connectable", "0");
        for (const auto* type : types) {
            auto key = type->qualifiedName();
            const auto& box = layouts.at(name).box(key);
            auto position = box.position;
            position.y += NAMESPACE_HEADER;
            vertex(root, "type:" + key, id, headers.at(key).html(),
                   "rounded=0;whiteSpace=wrap;html=1;align=center;" + std::string(FONT) + colors(*type),
                   position, box.size);
        }
        detail(page(file, id, displayNamespace(name)), types, internal.at(name));
    }
}

}

std::string writeDrawio(const UmlModel& model, bool withOverview) {
    tinyxml2::XMLDocument xml;
    xml.InsertEndChild(xml.NewDeclaration("xml version=\"1.0\" encoding=\"UTF-8\""));
    auto* file = element(&xml, "mxfile", {{"compressed", "false"}});
    if (withOverview) {
        overview(file, model);
    } else {
        Types types;
        for (const auto& type : model.types) {
            types.push_back(&type);
        }
        detail(page(file, "uml", "UML Class Diagram"), types, model.edges);
    }
    tinyxml2::XMLPrinter output;
    xml.Print(&output);
    return output.CStr();
}

}
