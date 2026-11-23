#include "drawio_reader.hpp"
#include "cpp_syntax.hpp"

#include <llvm/Support/Base64.h>
#include <llvm/Support/JSON.h>
#include <tinyxml2.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <limits>
#include <memory>
#include <ranges>
#include <stdexcept>

namespace drawio {
namespace {

using tinyxml2::XMLElement;

std::string attribute(const XMLElement& node, const char* name) {
    if (auto* value = node.Attribute(name)) {
        return value;
    }
    return {};
}

std::unique_ptr<tinyxml2::XMLDocument> parseXml(std::string_view xml) {
    if (xml.find("<!DOCTYPE") != std::string_view::npos) {
        throw std::invalid_argument("DOCTYPE is not supported in Draw.io XML");
    }
    if (xml.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("Expected UTF-8 Draw.io XML");
    }
    auto document = std::make_unique<tinyxml2::XMLDocument>();
    if (document->Parse(xml.data(), xml.size()) != tinyxml2::XML_SUCCESS || !document->RootElement()) {
        const auto* reason = document->ErrorStr();
        throw std::invalid_argument("Invalid Draw.io XML: " + std::string(reason ? reason : "missing document element"));
    }
    return document;
}

std::string decodePage(std::string_view text) {
    std::string base64;
    std::ranges::copy_if(text, std::back_inserter(base64), [](unsigned char ch) { return !std::isspace(ch); });
    std::vector<char> compressed;
    if (auto error = llvm::decodeBase64(base64, compressed)) {
        throw std::invalid_argument("Invalid compressed Draw.io page: " + llvm::toString(std::move(error)));
    }
    if (compressed.empty() || compressed.size() > std::numeric_limits<uInt>::max()) {
        throw std::invalid_argument("Invalid compressed Draw.io page size");
    }
    z_stream stream{};
    stream.next_in = reinterpret_cast<Bytef*>(compressed.data());
    stream.avail_in = static_cast<uInt>(compressed.size());
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        throw std::runtime_error("Cannot initialize Draw.io decompression");
    }
    auto cleanup = [](z_stream* value) { inflateEnd(value); };
    std::unique_ptr<z_stream, decltype(cleanup)> active(&stream, cleanup);
    constexpr std::size_t MAX_PAGE = 64 * 1024 * 1024;
    std::array<char, 16384> buffer{};
    std::string encoded;
    int status;
    do {
        stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
        stream.avail_out = static_cast<uInt>(buffer.size());
        status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END) {
            throw std::invalid_argument("Invalid deflate data in Draw.io page");
        }
        encoded.append(buffer.data(), buffer.size() - stream.avail_out);
        if (encoded.size() > MAX_PAGE) {
            throw std::invalid_argument("Inflated Draw.io page exceeds 64 MiB");
        }
    } while (status != Z_STREAM_END);
    if (stream.avail_in) {
        throw std::invalid_argument("Trailing compressed Draw.io page data");
    }
    std::string xml;
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] != '%') {
            xml += encoded[i];
            continue;
        }
        unsigned byte = 0;
        if (encoded.size() - i < 3) {
            throw std::invalid_argument("Invalid URL encoding in compressed Draw.io page");
        }
        auto begin = encoded.data() + i + 1;
        auto result = std::from_chars(begin, begin + 2, byte, 16);
        if (result.ec != std::errc{} || result.ptr != begin + 2) {
            throw std::invalid_argument("Invalid URL encoding in compressed Draw.io page");
        }
        xml += static_cast<char>(byte);
        i += 2;
    }
    return xml;
}

struct Cell {
    std::string id;
    std::string parent;
    std::string source;
    std::string target;
    std::string label;
    bool vertex;
    bool edge;
    std::map<std::string, std::string> style;
    const XMLElement* properties;

    bool styled(const std::string& name, std::string_view expected) const {
        auto entry = style.find(name);
        return entry != style.end() && entry->second == expected;
    }
};

struct Line {
    std::string text;
    bool italic = false;
    bool underline = false;
};

std::map<std::string, std::string> readStyle(std::string_view text) {
    std::map<std::string, std::string> result;
    while (!text.empty()) {
        auto end = text.find(';');
        auto item = text.substr(0, end);
        auto equals = item.find('=');
        if (!item.empty()) {
            result[trim(item.substr(0, equals))] = equals == item.npos ? "1" : trim(item.substr(equals + 1));
        }
        if (end == text.npos) {
            break;
        }
        text.remove_prefix(end + 1);
    }
    return result;
}

void appendUtf8(std::string& text, unsigned code) {
    if (code <= 0x7f) {
        text += static_cast<char>(code);
    } else if (code <= 0x7ff) {
        text += static_cast<char>(0xc0 | (code >> 6));
        text += static_cast<char>(0x80 | (code & 0x3f));
    } else if (code <= 0xffff) {
        text += static_cast<char>(0xe0 | (code >> 12));
        text += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
        text += static_cast<char>(0x80 | (code & 0x3f));
    } else {
        text += static_cast<char>(0xf0 | (code >> 18));
        text += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
        text += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
        text += static_cast<char>(0x80 | (code & 0x3f));
    }
}

bool decodeEntity(std::string_view name, std::string& text) {
    static const std::map<std::string_view, unsigned> entities = {
        {"lt", '<'}, {"gt", '>'}, {"amp", '&'}, {"quot", '"'}, {"apos", '\''},
        {"nbsp", ' '}, {"laquo", 0xab}, {"raquo", 0xbb},
    };
    if (auto found = entities.find(name); found != entities.end()) {
        appendUtf8(text, found->second);
        return true;
    }
    if (!name.starts_with('#')) {
        return false;
    }
    name.remove_prefix(1);
    int base = 10;
    if (name.starts_with('x') || name.starts_with('X')) {
        base = 16;
        name.remove_prefix(1);
    }
    unsigned code = 0;
    auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), code, base);
    if (error != std::errc{} || end != name.data() + name.size() || code == 0 || code > 0x10ffff
        || (code >= 0xd800 && code <= 0xdfff)) {
        return false;
    }
    appendUtf8(text, code == 0xa0 ? ' ' : code);
    return true;
}

std::vector<Line> labelLines(const Cell& cell) {
    int font = 0;
    if (auto style = cell.style.find("fontStyle"); style != cell.style.end()) {
        const auto& value = style->second;
        std::from_chars(value.data(), value.data() + value.size(), font);
    }
    std::vector<Line> lines;
    std::string text;
    bool html = cell.styled("html", "1");
    int italic = 0;
    int underline = 0;
    bool lineItalic = false;
    bool lineUnderline = false;
    auto flush = [&] {
        auto value = trim(text);
        if (!value.empty()) {
            lines.push_back({std::move(value), lineItalic || (font & 2), lineUnderline || (font & 4)});
        }
        text.clear();
        lineItalic = lineUnderline = false;
    };
    const std::string_view label = cell.label;
    static const std::set<std::string> blocks = {"div", "br", "p", "li", "hr", "tr"};
    static const std::set<std::string> formatting =
        {"span", "font", "b", "strong", "i", "em", "u", "table", "tbody", "td", "ul", "ol", "sub", "sup"};
    for (std::size_t i = 0; i < label.size(); ++i) {
        char ch = label[i];
        if (ch == '\n' || ch == '\r') {
            flush();
            continue;
        }
        if (html && ch == '<') {
            auto end = label.find('>', i + 1);
            if (end != label.npos) {
                auto tag = label.substr(i + 1, end - i - 1);
                bool closing = tag.starts_with('/');
                if (closing) {
                    tag.remove_prefix(1);
                }
                auto name = std::string(tag.substr(0, tag.find_first_of(" \t/")));
                std::ranges::transform(name, name.begin(), [](unsigned char c) { return std::tolower(c); });
                if (blocks.contains(name) || formatting.contains(name)) {
                    if (blocks.contains(name)) {
                        flush();
                    }
                    int delta = closing ? -1 : 1;
                    if (name == "i" || name == "em") {
                        italic = std::max(0, italic + delta);
                    } else if (name == "u") {
                        underline = std::max(0, underline + delta);
                    }
                    i = end;
                    continue;
                }
            }
        }
        lineItalic |= italic > 0 && !std::isspace(static_cast<unsigned char>(ch));
        lineUnderline |= underline > 0 && !std::isspace(static_cast<unsigned char>(ch));
        if (html && ch == '&') {
            auto end = label.find(';', i + 1);
            if (end != label.npos && end - i < 14 && decodeEntity(label.substr(i + 1, end - i - 1), text)) {
                i = end;
                continue;
            }
        }
        text += ch;
    }
    flush();
    return lines;
}

bool startsWithAccess(std::string_view text) {
    return !text.empty() && std::string_view("+-#").find(text.front()) != text.npos;
}

bool isStereotype(std::string_view text) {
    return text.starts_with("«") || text.starts_with("<<");
}

Access visibility(std::string& text, Access fallback) {
    if (!startsWithAccess(text)) {
        return fallback;
    }
    char marker = text.front();
    text = trim(std::string_view(text).substr(1));
    return marker == '+' ? Access::Public : marker == '#' ? Access::Protected : Access::Private;
}

std::vector<std::string> decorations(std::string& text) {
    auto start = topLevel(text, '{');
    if (start == text.npos) {
        return {};
    }
    std::vector<std::string> result;
    for (auto remaining = trim(std::string_view(text).substr(start)); !remaining.empty();) {
        if (!remaining.starts_with('{')) {
            throw std::invalid_argument("Expected a UML modifier in braces near '" + remaining + "'");
        }
        auto end = closeGroup(remaining, 0);
        result.push_back(checkedFragment(std::string_view(remaining).substr(1, end - 1), "UML modifier"));
        remaining = trim(std::string_view(remaining).substr(end + 1));
    }
    text = trim(std::string_view(text).substr(0, start));
    return result;
}

std::vector<std::string> templateParameters(std::string_view text) {
    auto parameters = splitList(text);
    if (parameters.empty()) {
        throw std::invalid_argument("Expected at least one template parameter");
    }
    for (const auto& parameter : parameters) {
        checkedFragment(parameter, "Template parameter");
    }
    return parameters;
}

std::vector<std::string> propertyList(const Cell& cell, const char* attributeName) {
    if (!cell.properties || !cell.properties->Attribute(attributeName)) {
        return {};
    }
    auto decoded = llvm::json::parse(attribute(*cell.properties, attributeName));
    if (!decoded) {
        llvm::consumeError(decoded.takeError());
        throw std::invalid_argument(std::string("Invalid shape data ") + attributeName);
    }
    auto* array = decoded->getAsArray();
    if (!array) {
        throw std::invalid_argument(std::string("Expected an array in ") + attributeName);
    }
    std::vector<std::string> result;
    for (const auto& item : *array) {
        auto value = item.getAsString();
        if (!value) {
            throw std::invalid_argument(std::string("Expected text in ") + attributeName);
        }
        result.push_back(value->str());
    }
    return result;
}

std::string includeOperand(std::string text) {
    if (text.starts_with('<') && text.size() > 2 && text.find_first_of("<>\r\n", 1) == text.size() - 1) {
        if (text.ends_with('>')) {
            return text;
        }
    } else if (text.starts_with('"') && text.size() > 2 && text.ends_with('"')
               && text.find_first_of("\"\r\n", 1) == text.size() - 1) {
        return text;
    }
    throw std::invalid_argument("Specify an include as #include <header> or #include \"header\"");
}

std::pair<std::string, std::string> splitAssignment(std::string_view text) {
    auto at = topLevel(text, '=');
    return {trim(text.substr(0, at)), at == text.npos ? "" : checkedFragment(text.substr(at + 1), "Initializer")};
}

bool validOperationName(std::string_view name, const UmlType& type, bool& conversion) {
    if (name == "~" + type.name || identifier(name)) {
        return true;
    }
    if (!name.starts_with("operator")) {
        return false;
    }
    auto suffix = trim(name.substr(8));
    static const std::set<std::string> operators = {
        "()", "[]", "=", "+", "-", "*", "/", "%", "^", "&", "|", "~", "!", "<", ">", "+=", "-=", "*=", "/=",
        "%=", "^=", "&=", "|=", "<<", ">>", "<<=", ">>=", "==", "!=", "<=", ">=", "<=>", "&&", "||", "++", "--",
        ",", "->*", "->", "new", "new[]", "delete", "delete[]", "co_await",
    };
    if (operators.contains(suffix)) {
        return true;
    }
    conversion = name.starts_with("operator ") && !suffix.empty();
    return conversion;
}

Operation readOperation(const Line& line, const UmlType& owner, Access access, std::string text) {
    Operation operation;
    operation.access = access;
    if (text.starts_with('<')) {
        auto end = closeGroup(text, 0);
        operation.templates = templateParameters(std::string_view(text).substr(1, end - 1));
        text = trim(std::string_view(text).substr(end + 1));
    }
    std::size_t start = text.starts_with("operator") ? text.find('(') : topLevel(text, '(');
    if (text.starts_with("operator()") || text.starts_with("operator ()")) {
        start = text.find('(', start + 1);
    }
    if (start == text.npos) {
        throw std::invalid_argument("Expected a field as 'name: Type' or a method, found '" + text + "'");
    }
    auto end = closeGroup(text, start);
    operation.name = trim(std::string_view(text).substr(0, start));
    bool conversion = false;
    if (!validOperationName(operation.name, owner, conversion)) {
        throw std::invalid_argument("Invalid operation name '" + operation.name + "'");
    }
    if (conversion) {
        checkedFragment(std::string_view(operation.name).substr(9), "Conversion type");
    }
    auto suffix = trim(std::string_view(text).substr(end + 1));
    auto modifiers = decorations(suffix);
    static const std::set<std::string_view> simple = {"static", "virtual", "inline", "constexpr", "consteval",
        "explicit", "const", "volatile", "&", "&&", "noexcept", "override", "final", "pure", "delete", "default",
        "default-outside"};
    for (auto& modifier : modifiers) {
        if (modifier.starts_with("init ")) {
            if (operation.initializers) {
                throw std::invalid_argument("Repeated constructor initializer list");
            }
            auto init = checkedFragment(std::string_view(modifier).substr(5), "Constructor initializer list");
            splitList(init);
            operation.initializers = std::move(init);
        } else if (simple.contains(modifier) || modifier.starts_with("noexcept(") || modifier.starts_with("explicit(")
                   || modifier.starts_with("requires ")) {
            if (hasModifier(operation, modifier)) {
                throw std::invalid_argument("Repeated operation modifier '" + modifier + "'");
            }
            operation.modifiers.push_back(std::move(modifier));
        } else {
            throw std::invalid_argument("Unsupported operation modifier '{" + modifier + "}'");
        }
    }
    if (line.italic && !hasModifier(operation, "pure")) {
        operation.modifiers.emplace_back("pure");
    }
    if (hasModifier(operation, "pure") && !hasModifier(operation, "virtual")) {
        operation.modifiers.emplace_back("virtual");
    }
    if (line.underline && !hasModifier(operation, "static")) {
        operation.modifiers.emplace_back("static");
    }
    bool constructor = operation.name == owner.name;
    bool destructor = operation.name == "~" + owner.name;
    if (constructor || destructor || conversion) {
        if (!suffix.empty()) {
            throw std::invalid_argument("Constructors, destructors and conversion operators omit the UML return type");
        }
    } else {
        if (!suffix.starts_with(':')) {
            throw std::invalid_argument("Specify the return type of '" + operation.name + "' after ':' (or use void)");
        }
        operation.result = checkedFragment(std::string_view(suffix).substr(1), "Return type");
    }
    if (operation.initializers && !constructor) {
        throw std::invalid_argument("Only a constructor takes an initializer list");
    }
    std::set<std::string> names;
    auto parameters = splitList(std::string_view(text).substr(start + 1, end - start - 1));
    for (const auto& parameter : parameters) {
        if (parameter == "..." && &parameter == &parameters.back()) {
            operation.parameters.push_back({"", "...", ""});
            continue;
        }
        auto colon = parameter.find(':');
        auto name = trim(std::string_view(parameter).substr(0, colon));
        if (colon == parameter.npos || !identifier(name)) {
            throw std::invalid_argument("Specify a parameter as 'name: Type' in '" + operation.name + "'");
        }
        auto [type, value] = splitAssignment(std::string_view(parameter).substr(colon + 1));
        if (!names.insert(name).second) {
            throw std::invalid_argument("Duplicate parameter '" + name + "'");
        }
        operation.parameters.push_back({std::move(name), checkedFragment(type, "Parameter type"), std::move(value)});
    }
    if ((destructor || conversion) && !operation.parameters.empty()) {
        throw std::invalid_argument("Destructors and conversion operators cannot take parameters");
    }
    return operation;
}

void readMember(const Line& line, UmlType& type) {
    std::string text = line.text;
    Access access = visibility(text, type.kind == Kind::Class ? Access::Private : Access::Public);
    if (isEnum(type.kind)) {
        auto [name, value] = splitAssignment(text);
        if (!identifier(name)) {
            throw std::invalid_argument("Expected an enumerator name in '" + text + "'");
        }
        type.enumerators.push_back({std::move(name), std::move(value)});
        return;
    }
    auto colon = text.find(':');
    auto name = trim(std::string_view(text).substr(0, colon));
    if (colon == text.npos || (!name.empty() && !identifier(name))) {
        type.operations.push_back(readOperation(line, type, access, std::move(text)));
        return;
    }
    std::string spelling = trim(std::string_view(text).substr(colon + 1));
    auto modifiers = decorations(spelling);
    Field field{access, std::move(name), checkedFragment(spelling, "Field type"), {}, "", ""};
    for (const auto& modifier : modifiers) {
        if (modifier.starts_with("bits ") && field.bitWidth.empty()) {
            field.bitWidth = checkedFragment(std::string_view(modifier).substr(5), "Bit field width");
        } else if (modifier.starts_with("init ") && field.initializer.empty()) {
            field.initializer = checkedFragment(std::string_view(modifier).substr(5), "Field initializer");
        } else if (modifier == "static" || modifier == "constexpr" || modifier == "inline" || modifier == "mutable"
                   || modifier == "thread_local") {
            if (hasModifier(field, modifier)) {
                throw std::invalid_argument("Repeated field modifier '" + modifier + "'");
            }
            field.modifiers.push_back(modifier);
        } else {
            throw std::invalid_argument("Unsupported field modifier '{" + modifier + "}'");
        }
    }
    if (line.underline && !hasModifier(field, "static")) {
        field.modifiers.emplace_back("static");
    }
    if (field.name.empty() && field.bitWidth.empty()) {
        throw std::invalid_argument("A field must have a name unless it is an unnamed bit field");
    }
    type.fields.push_back(std::move(field));
}

UmlType readType(const Cell& cell, std::vector<Line> header, const std::vector<Line>& rows, std::string enclosing) {
    if (header.empty()) {
        throw std::invalid_argument("The class name is missing");
    }
    UmlType type;
    type.kind = Kind::Class;
    std::size_t title = 0;
    if (isStereotype(header.front().text)) {
        auto& text = header.front().text;
        bool guillemets = text.starts_with("«");
        auto start = std::string_view(text).substr(2);
        auto end = start.find(guillemets ? "»" : ">>");
        if (end == start.npos) {
            throw std::invalid_argument("Unterminated type stereotype");
        }
        auto kind = trim(start.substr(0, end));
        std::ranges::transform(kind, kind.begin(), [](unsigned char ch) { return std::tolower(ch); });
        if (kind == "struct") {
            type.kind = Kind::Struct;
        } else if (kind == "union") {
            type.kind = Kind::Union;
        } else if (kind == "enum" || kind == "enumeration") {
            type.kind = Kind::Enum;
        } else if (kind == "enum class" || kind == "enum struct") {
            type.kind = Kind::ScopedEnum;
        } else if (kind == "abstract" || kind == "interface") {
            type.abstract = true;
        } else if (kind != "class") {
            throw std::invalid_argument("Unsupported type stereotype '" + kind + "'");
        }
        auto rest = trim(start.substr(end + 2));
        if (rest.empty()) {
            ++title;
        } else {
            text = std::move(rest);
        }
    }
    if (title == header.size()) {
        throw std::invalid_argument("The class name is missing");
    }
    std::string name = header[title].text;
    type.abstract |= header[title].italic;
    type.access = visibility(name, Access::Public);
    for (const auto& modifier : decorations(name)) {
        if (modifier == "final") {
            type.final = true;
        } else if (modifier == "abstract") {
            type.abstract = true;
        } else {
            throw std::invalid_argument("Unsupported type modifier '{" + modifier + "}'");
        }
    }
    if (cell.properties) {
        type.owner = attribute(*cell.properties, "cppOwner");
        type.specialization = attribute(*cell.properties, "cppSpecialization");
    }
    if (!type.specialization.empty()) {
        throw std::invalid_argument("C++ template specializations currently support forward conversion only");
    }
    if (auto open = topLevel(name, '<'); open != name.npos) {
        auto end = closeGroup(name, open);
        if (end + 1 != name.size()) {
            throw std::invalid_argument("Invalid type template declaration '" + name + "'");
        }
        type.templates = templateParameters(std::string_view(name).substr(open + 1, end - open - 1));
        name = trim(std::string_view(name).substr(0, open));
    }
    for (const auto& base : propertyList(cell, "cppBases")) {
        type.bases.push_back(checkedFragment(base, "Base class"));
    }
    for (const auto& include : propertyList(cell, "cppIncludes")) {
        type.includes.insert(includeOperand(include));
    }
    for (const auto& limitation : propertyList(cell, "cppLimitations")) {
        type.reverseLimitations.insert(limitation);
    }
    std::size_t body = title + 1;
    for (; body < header.size(); ++body) {
        const auto& text = header[body].text;
        if (text.starts_with("namespace ")) {
            enclosing = trim(std::string_view(text).substr(10));
        } else if (text.starts_with("base ") || text.starts_with("extends ")) {
            auto bases = splitList(std::string_view(text).substr(text.starts_with("base ") ? 5 : 8));
            for (auto& base : bases) {
                type.bases.push_back(checkedFragment(base, "Base class"));
            }
        } else if (text.starts_with("underlying ")) {
            type.underlying = checkedFragment(std::string_view(text).substr(11), "Underlying enum type");
        } else if (text.starts_with("#include ")) {
            type.includes.insert(includeOperand(trim(std::string_view(text).substr(9))));
        } else {
            break;
        }
    }
    auto parts = scopeParts(name);
    type.name = parts.back();
    parts.pop_back();
    if (!enclosing.empty()) {
        scopeParts(enclosing);
    }
    if (!type.owner.empty()) {
        scopeParts(type.owner);
        auto expected = identity(type.owner, type.name);
        if (identity(enclosing, name) != expected && name != expected && name != type.name) {
            throw std::invalid_argument("Type title disagrees with enclosing type " + type.owner);
        }
    } else if (enclosing.empty()) {
        enclosing = join(parts, "::");
    } else if (!parts.empty()) {
        auto scopes = join(parts, "::");
        if (scopes != enclosing) {
            type.owner = scopes.starts_with(enclosing + "::") ? scopes : identity(enclosing, scopes);
        }
    }
    type.nameSpace = std::move(enclosing);
    if (!isEnum(type.kind) && !type.underlying.empty()) {
        throw std::invalid_argument("Only an enum can have an underlying type");
    }
    for (; body < header.size(); ++body) {
        readMember(header[body], type);
    }
    for (const auto& row : rows) {
        readMember(row, type);
    }
    return type;
}

struct TypeBox {
    UmlType type;
    std::string location;
    bool compact;
};

class ModelReader {
public:
    void readPage(const XMLElement& graph, const std::string& id, const std::string& pageName) {
        const auto* root = graph.FirstChildElement("root");
        if (std::string_view(graph.Name()) != "mxGraphModel" || !root || root->NextSiblingElement("root")) {
            throw std::invalid_argument("Expected one root in the mxGraphModel");
        }
        std::vector<Cell> cells;
        std::map<std::string, std::size_t> byId;
        std::map<std::string, std::vector<std::size_t>> children;
        std::vector<std::vector<Line>> labels;
        auto where = [&](const std::string& key) { return "Page '" + pageName + "', cell '" + key + "'"; };
        for (auto* element = root->FirstChildElement(); element; element = element->NextSiblingElement()) {
            const XMLElement* node = element;
            const XMLElement* properties = nullptr;
            std::string_view tag = element->Name();
            auto key = attribute(*element, "id");
            if (tag == "UserObject" || tag == "object") {
                node = element->FirstChildElement("mxCell");
                if (!node || node->NextSiblingElement("mxCell")) {
                    throw std::invalid_argument(where(key) + ": Expected one mxCell in object");
                }
                properties = element;
            } else if (tag != "mxCell") {
                continue;
            }
            if (key.empty() || !byId.emplace(key, cells.size()).second) {
                throw std::invalid_argument(where(key) + ": Missing or duplicate mxCell ID");
            }
            std::string value = properties && properties->Attribute("label") ? attribute(*properties, "label")
                                                                            : attribute(*node, "value");
            Cell cell{std::move(key), attribute(*node, "parent"), attribute(*node, "source"),
                attribute(*node, "target"), std::move(value), attribute(*node, "vertex") == "1",
                attribute(*node, "edge") == "1", readStyle(attribute(*node, "style")), properties};
            if (cell.vertex) {
                children[cell.parent].push_back(cells.size());
            }
            labels.push_back(cell.vertex ? labelLines(cell) : std::vector<Line>{});
            cells.push_back(std::move(cell));
        }
        auto ancestry = [&](std::string key) {
            std::set<std::string> seen;
            std::vector<std::size_t> result;
            for (auto current = byId.find(key); current != byId.end(); current = byId.find(key)) {
                if (!seen.insert(key).second) {
                    throw std::invalid_argument("Cyclic parent chain at '" + key + "'");
                }
                result.push_back(current->second);
                key = cells[current->second].parent;
            }
            return result;
        };
        std::map<std::string, std::string> classes;
        for (std::size_t i = 0; i < cells.size(); ++i) {
            const auto& cell = cells[i];
            if (!cell.vertex || !isClass(cell, labels[i], children.contains(cell.id))) {
                continue;
            }
            try {
                std::string enclosing;
                for (auto parent : ancestry(cell.parent)) {
                    if (cells[parent].id.starts_with("namespace:")) {
                        enclosing = cells[parent].id.substr(10);
                        break;
                    }
                    if (!labels[parent].empty() && labels[parent].front().text.starts_with("namespace ")) {
                        enclosing = trim(std::string_view(labels[parent].front().text).substr(10));
                        break;
                    }
                }
                auto rows = children[cell.id];
                std::set<std::size_t> seen{i};
                std::vector<Line> members;
                for (std::size_t next = 0; next < rows.size(); ++next) {
                    auto child = rows[next];
                    if (!seen.insert(child).second) {
                        throw std::invalid_argument("Cyclic shape nesting");
                    }
                    if (!cells[child].id.starts_with("type:")) {
                        const auto& lines = labels[child];
                        members.insert(members.end(), lines.begin(), lines.end());
                        auto nested = children.find(cells[child].id);
                        if (nested != children.end()) {
                            rows.insert(rows.end(), nested->second.begin(), nested->second.end());
                        }
                    }
                }
                bool compact = cell.id.starts_with("type:") && (id == "overview" || cell.parent.starts_with("namespace:"));
                // Compact exports carry identity in their IDs and may omit template declarations.
                if (compact && cell.id.find('<') != cell.id.npos) {
                    throw std::invalid_argument("C++ template specializations currently support forward conversion only");
                }
                auto type = readType(cell, labels[i], members, enclosing);
                auto key = type.qualifiedName();
                classes.emplace(cell.id, key);
                TypeBox box{std::move(type), where(cell.id), compact};
                auto previous = types_.find(key);
                if (previous == types_.end()) {
                    types_.emplace(std::move(key), std::move(box));
                } else if (previous->second.compact && !compact) {
                    previous->second = std::move(box);
                } else if (!previous->second.compact && !compact && previous->second.type != box.type) {
                    throw std::invalid_argument("Conflicting declarations for " + key + " (also in "
                                                + previous->second.location + ")");
                }
            } catch (const std::invalid_argument& error) {
                throw std::invalid_argument(where(cell.id) + ": " + error.what());
            }
        }
        auto owner = [&](const std::string& key) -> std::string {
            for (auto child : ancestry(key)) {
                if (auto type = classes.find(cells[child].id); type != classes.end()) {
                    return type->second;
                }
            }
            return {};
        };
        for (const auto& cell : cells) {
            if (!cell.edge) {
                continue;
            }
            bool start = triangle(cell, "start");
            bool end = triangle(cell, "end");
            if (!start && !end) {
                continue;
            }
            auto from = owner(start ? cell.target : cell.source);
            auto to = owner(start ? cell.source : cell.target);
            if ((start && end) || from.empty() || to.empty() || from == to) {
                throw std::invalid_argument(where(cell.id)
                    + ": Connect the inheritance arrow between two different UML classes with one triangle at the base");
            }
            edges_.try_emplace(Edge{std::move(from), std::move(to), EdgeKind::Generalization}, where(cell.id));
        }
    }

    Diagram finish() {
        if (types_.empty()) {
            throw std::invalid_argument("No UML class boxes found. Use a UML class shape or a box with a typed label.");
        }
        Diagram result;
        for (auto& [name, box] : types_) {
            if (box.compact) {
                throw std::invalid_argument(box.location + ": No detail box for " + name);
            }
            result.locations.emplace(name, std::move(box.location));
            result.model.types.push_back(std::move(box.type));
        }
        for (const auto& [edge, location] : edges_) {
            result.model.edges.push_back(edge);
        }
        result.edgeLocations = std::move(edges_);
        return result;
    }

private:
    static bool triangle(const Cell& cell, const std::string& side) {
        return (cell.styled(side + "Arrow", "block") || cell.styled(side + "Arrow", "blockThin"))
            && cell.styled(side + "Fill", "0");
    }

    static bool isClass(const Cell& cell, const std::vector<Line>& lines, bool hasRows) {
        if (cell.id.starts_with("type:")) {
            return true;
        }
        if (lines.empty() || cell.id.starts_with("namespace:") || cell.id.starts_with("fields:")
            || cell.id.starts_with("methods:") || cell.style.contains("line") || cell.styled("shape", "note")) {
            return false;
        }
        const auto& title = lines.front().text;
        auto colon = title.find(':');
        if (title.starts_with("namespace ") || title.starts_with("#include ") || title.find('(') != title.npos
            || (colon != title.npos && !std::string_view(title).substr(colon).starts_with("::"))) {
            return false;
        }
        bool text = cell.style.contains("text") || cell.styled("shape", "text");
        if (text && lines.size() == 1) {
            return false;
        }
        bool swimlane = cell.style.contains("swimlane") || cell.styled("shape", "swimlane");
        return (swimlane && (cell.styled("childLayout", "stackLayout") || hasRows)) || cell.styled("shape", "umlClass")
            || startsWithAccess(title) || isStereotype(title)
            || std::ranges::any_of(lines | std::views::drop(1), [](const auto& line) {
                return startsWithAccess(line.text) || line.text.find_first_of(":(") != line.text.npos;
            });
    }

    std::map<std::string, TypeBox> types_;
    std::map<Edge, std::string> edges_;
};

}

Diagram readDrawio(const fs::path& input) {
    auto document = parseXml(readFile(input));
    auto* root = document->RootElement();
    ModelReader reader;
    std::string_view tag = root->Name();
    if (tag == "mxGraphModel") {
        reader.readPage(*root, "", pathText(input.filename()));
    } else if (tag == "mxfile") {
        int number = 0;
        for (auto* page = root->FirstChildElement("diagram"); page; page = page->NextSiblingElement("diagram")) {
            ++number;
            auto name = attribute(*page, "name");
            if (name.empty()) {
                name = "Page " + std::to_string(number);
            }
            const auto* graph = page->FirstChildElement("mxGraphModel");
            std::unique_ptr<tinyxml2::XMLDocument> inflated;
            try {
                if (graph && graph->NextSiblingElement("mxGraphModel")) {
                    throw std::invalid_argument("Expected one mxGraphModel");
                }
                if (!graph) {
                    auto text = page->GetText();
                    if (!text) {
                        throw std::invalid_argument("The diagram page has no model");
                    }
                    inflated = parseXml(decodePage(text));
                    graph = inflated->RootElement();
                }
            } catch (const std::exception& error) {
                throw std::invalid_argument("Page '" + name + "': " + error.what());
            }
            reader.readPage(*graph, attribute(*page, "id"), name);
        }
        if (number == 0) {
            throw std::invalid_argument("The Draw.io file contains no diagram pages");
        }
    } else {
        throw std::invalid_argument("Expected a Draw.io mxfile or mxGraphModel document");
    }
    return reader.finish();
}

}
