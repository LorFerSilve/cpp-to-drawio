#include "cpp_skeletons.hpp"
#include "cpp_syntax.hpp"
#include "skeleton_doxygen.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <stdexcept>

namespace drawio {
namespace {

std::string normalized(std::string text) {
    std::erase_if(text, [](unsigned char ch) { return std::isspace(ch); });
    if (text.starts_with("::")) {
        text.erase(0, 2);
    }
    return text;
}

std::string_view typeKeyword(Kind kind) {
    switch (kind) {
        case Kind::Class: return "class";
        case Kind::Struct: return "struct";
        case Kind::Union: return "union";
        case Kind::Enum: return "enum";
        case Kind::ScopedEnum: return "enum class";
    }
    return "class";
}

std::string templateHead(const std::vector<std::string>& parameters) {
    if (parameters.empty()) {
        return {};
    }
    return "template <" + join(parameters, ", ") + ">\n";
}

std::string namespaceOpen(std::string_view name) {
    return name.empty() ? "" : "namespace " + std::string(name) + " {\n\n";
}

std::string namespaceClose(std::string_view name) {
    return name.empty() ? "" : "\n} // namespace " + std::string(name) + "\n";
}

std::string enumBase(const UmlType& type) {
    return type.underlying.empty() ? "" : " : " + type.underlying;
}

bool definedInHeader(const UmlType& type, const Operation& operation, bool inTemplate) {
    return inTemplate || !type.templates.empty() || !operation.templates.empty() || hasModifier(operation, "inline")
        || hasModifier(operation, "constexpr") || hasModifier(operation, "consteval");
}

bool hasBody(const Operation& operation) {
    return !hasModifier(operation, "default") && !hasModifier(operation, "delete") && !hasModifier(operation, "pure");
}

std::string_view prefix(const std::string& modifier, char delimiter) {
    return std::string_view(modifier).substr(0, modifier.find(delimiter));
}

std::string signaturesKey(const Operation& operation) {
    std::vector<std::string> parts;
    for (const auto& parameter : operation.templates) {
        parts.push_back(normalized(withoutDefault(parameter)));
    }
    std::string key = "<" + join(parts, ",") + ">" + operation.name;
    parts.clear();
    for (const auto& parameter : operation.parameters) {
        parts.push_back(normalized(parameter.type));
    }
    key += "(" + join(parts, ",") + ")";
    for (std::string_view modifier : {"const", "volatile", "&", "&&"}) {
        if (hasModifier(operation, modifier)) {
            key += " ";
            key += modifier;
        }
    }
    for (const auto& modifier : operation.modifiers) {
        if (modifier.starts_with("requires ")) {
            key += " " + modifier;
        }
    }
    return key;
}

std::string renderSignature(const Operation& operation, std::string_view owner = {}) {
    bool inClass = owner.empty();
    std::vector<std::string> parameters;
    for (const auto& parameter : operation.parameters) {
        auto value = withName(parameter.type, parameter.name);
        if (inClass && !parameter.defaultValue.empty()) {
            value += " = " + parameter.defaultValue;
        }
        parameters.push_back(std::move(value));
    }
    std::string declarator = identity(owner, operation.name) + "(" + join(parameters, ", ") + ")";
    std::string prefixes;
    if (inClass) {
        for (const auto& modifier : operation.modifiers) {
            auto word = prefix(modifier, '(');
            if (word == "static" || word == "inline" || word == "virtual" || word == "constexpr"
                || word == "consteval" || word == "explicit") {
                prefixes += modifier + " ";
            }
        }
    }
    for (std::string_view word : {"const", "volatile", "&", "&&", "noexcept"}) {
        for (const auto& modifier : operation.modifiers) {
            if (prefix(modifier, '(') == word) {
                declarator += " " + modifier;
            }
        }
    }
    if (!operation.result.empty()) {
        if (!inClass || operation.result.find_first_of("([$") != std::string::npos) {
            declarator = "auto " + declarator + " -> " + withName(operation.result, "");
        } else {
            declarator = withName(operation.result, declarator);
        }
    }
    for (const auto& modifier : operation.modifiers) {
        if (modifier.starts_with("requires ") || (inClass && (modifier == "override" || modifier == "final"))) {
            declarator += " " + modifier;
        }
    }
    return prefixes + declarator;
}

std::string fieldDeclaration(const Field& field, std::string_view owner = {}) {
    std::string text;
    for (const auto& modifier : field.modifiers) {
        if (owner.empty() || modifier == "thread_local") {
            text += modifier + " ";
        }
    }
    text += withName(field.type, identity(owner, field.name));
    if (!field.bitWidth.empty()) {
        text += " : " + field.bitWidth;
    }
    if (owner.empty() && !field.initializer.empty()) {
        text += field.initializer.starts_with('{') ? " " : " = ";
        text += field.initializer;
    }
    return text;
}

bool wordCharacter(unsigned char ch) {
    return std::isalnum(ch) || ch == '_' || ch >= 0x80;
}

std::vector<std::string> namesIn(std::string_view text) {
    std::vector<std::string> names;
    for (std::size_t i = 0; i < text.size();) {
        char ch = text[i];
        if (ch == '\'' || ch == '"') {
            while (++i < text.size() && text[i] != ch) {
                if (text[i] == '\\' && i + 1 < text.size()) {
                    ++i;
                }
            }
            if (i < text.size()) {
                ++i;
            }
        } else if (wordCharacter(ch) || text.substr(i).starts_with("::")) {
            auto start = i;
            if (text.substr(i).starts_with("::")) {
                i += 2;
            }
            do {
                if (i != start && text.substr(i).starts_with("::")) {
                    i += 2;
                }
                while (i < text.size() && wordCharacter(static_cast<unsigned char>(text[i]))) {
                    ++i;
                }
            } while (text.substr(i).starts_with("::") && i + 2 < text.size() && wordCharacter(text[i + 2]));
            names.emplace_back(text.substr(start, i - start));
        } else {
            ++i;
        }
    }
    return names;
}

bool simpleIndirection(const std::string& spelling) {
    static const std::regex indirect(
        R"(^(?:(?:const|volatile)\s+)*(?:::)?[^\s:*&<>,()\[\]$]+(?:::[^\s:*&<>,()\[\]$]+)*(?:\s+(?:const|volatile))*\s*[*&]+(?:\s*(?:const|volatile|[*&$]))*$)");
    return std::regex_match(spelling, indirect);
}

bool hasBodyInHeader(const UmlType& owner, const Operation& operation, bool inTemplate) {
    return hasBody(operation) && definedInHeader(owner, operation, inTemplate);
}

bool canForwardDeclare(const UmlType& type) {
    return type.templates.empty() && (type.kind != Kind::Enum || !type.underlying.empty());
}

struct Unit {
    const UmlType* type;
    fs::path header;
    std::set<std::string> includes;
    std::set<std::string> complete;
    std::set<std::string> forward;
    bool inlineStub = false;
    bool sourceStub = false;
};

struct Base {
    std::string spelling;
    std::string name;
    const UmlType* target;
};

class Generator {
public:
    Generator(const Diagram& diagram, std::string_view author) : diagram_(diagram), author_(author) {
        for (const auto& type : diagram.model.types) {
            types_.emplace(type.qualifiedName(), &type);
        }
    }

    Skeletons generate() {
        index();
        readBases();
        for (auto& [name, unit] : units_) {
            dependencies(*unit.type, unit, {});
        }
        std::set<std::string> active;
        std::set<std::string> visited;
        for (const auto& [name, unit] : units_) {
            checkCycles(name, active, visited);
        }
        for (const auto& [name, unit] : units_) {
            writeUnit(unit);
        }
        return std::move(result_);
    }

private:
    std::invalid_argument error(const UmlType& type, const std::string& text) const {
        return std::invalid_argument(diagram_.locations.at(type.qualifiedName()) + ": " + text);
    }

    const UmlType& rootType(const UmlType& type) const {
        const auto* root = &type;
        while (!root->owner.empty()) {
            root = types_.at(root->owner);
        }
        return *root;
    }

    std::vector<const UmlType*> ancestry(const UmlType& type) const {
        std::vector<const UmlType*> result{&type};
        auto* parent = &type;
        while (!parent->owner.empty()) {
            parent = types_.at(parent->owner);
            result.push_back(parent);
        }
        std::ranges::reverse(result);
        return result;
    }

    void siblingDependency(const UmlType& from, const UmlType& to, bool needsDefinition) {
        auto source = ancestry(from);
        auto target = ancestry(to);
        auto [left, right] = std::ranges::mismatch(source, target);
        if (left == source.end() || right == target.end()) {
            return;
        }
        // A name such as Sibling::Nested also needs the enclosing sibling's definition.
        if (needsDefinition || right + 1 != target.end() || !canForwardDeclare(to)) {
            nestedComplete_[(*left)->qualifiedName()].insert((*right)->qualifiedName());
        }
    }

    std::vector<const UmlType*> definitionOrder(const UmlType& owner) const {
        std::vector<const UmlType*> result;
        std::set<std::string> active;
        std::set<std::string> visited;
        auto append = [&](const auto& recurse, const UmlType& type) -> void {
            auto name = type.qualifiedName();
            if (active.contains(name)) {
                throw error(type, "Cyclic complete-type dependencies between nested classes");
            }
            if (!visited.insert(name).second) {
                return;
            }
            active.insert(name);
            if (auto needs = nestedComplete_.find(name); needs != nestedComplete_.end()) {
                for (const auto& target : needs->second) {
                    recurse(recurse, *types_.at(target));
                }
            }
            active.erase(name);
            result.push_back(&type);
        };
        for (const auto* child : children_.at(owner.qualifiedName())) {
            append(append, *child);
        }
        return result;
    }

    void index() {
        std::set<std::string> paths;
        for (const auto& [name, type] : types_) {
            if (!type->reverseLimitations.empty()) {
                throw error(*type, "Cannot generate this declaration: "
                    + join(std::vector<std::string>(type->reverseLimitations.begin(), type->reverseLimitations.end()), ", "));
            }
            if (!type->owner.empty()) {
                if (!types_.contains(type->owner) || types_.at(type->owner)->nameSpace != type->nameSpace
                    || isEnum(types_.at(type->owner)->kind)) {
                    throw error(*type, "Missing or invalid enclosing class " + type->owner);
                }
                children_[type->owner].push_back(type);
            } else {
                fs::path path;
                for (const auto& part : scopeParts(name)) {
                    path /= pathFromUtf8(part);
                }
                path += ".hpp";
                std::string key = pathText(path);
#ifdef _WIN32
                std::ranges::transform(key, key.begin(), [](unsigned char ch) { return std::tolower(ch); });
#endif
                if (!paths.insert(key).second) {
                    throw error(*type, "Two types map to the same output file " + pathText(path));
                }
                units_.emplace(name, Unit{type, std::move(path), {}, {}, {}});
            }
            validateMembers(*type);
        }
    }

    void validateMembers(const UmlType& type) {
        std::set<std::string> members;
        for (const auto& entry : type.enumerators) {
            if (!members.insert(entry.name).second) {
                throw error(type, "Duplicate enumerator '" + entry.name + "'");
            }
        }
        for (const auto& field : type.fields) {
            if (!field.name.empty() && !members.insert(field.name).second) {
                throw error(type, "Duplicate field '" + field.name + "'");
            }
            if ((hasModifier(field, "static") && (!field.bitWidth.empty() || hasModifier(field, "mutable")))
                || (hasModifier(field, "constexpr") && (!hasModifier(field, "static") || field.initializer.empty()))
                || ((hasModifier(field, "inline") || hasModifier(field, "thread_local")) && !hasModifier(field, "static"))) {
                throw error(type, "Invalid field modifiers for '" + field.name + "'");
            }
        }
        auto& operations = operations_[type.qualifiedName()];
        std::map<std::string, const Operation*> signatures;
        for (const auto& operation : type.operations) {
            auto signature = signaturesKey(operation);
            auto [existing, inserted] = signatures.emplace(signature, &operation);
            if (!inserted) {
                if (*existing->second != operation) {
                    throw error(type, "Conflicting declarations of operation " + signature);
                }
                result_.warnings.push_back(diagram_.locations.at(type.qualifiedName())
                    + ": Repeated identical operation " + operation.name + "; generated once.");
                continue;
            }
            unsigned terminal = hasModifier(operation, "pure") + hasModifier(operation, "delete")
                + hasModifier(operation, "default") + hasModifier(operation, "default-outside");
            if (terminal > 1 || (hasModifier(operation, "static") && (hasModifier(operation, "virtual")
                || hasModifier(operation, "const") || hasModifier(operation, "volatile")
                || hasModifier(operation, "&") || hasModifier(operation, "&&")))) {
                throw error(type, "Incompatible modifiers on '" + operation.name + "'");
            }
            operations.push_back(&operation);
        }
    }

    const UmlType* lookup(std::string_view name, const UmlType& owner) const {
        bool global = name.starts_with("::");
        if (global) {
            name.remove_prefix(2);
        }
        auto scope = global ? std::string{} : owner.qualifiedName();
        while (true) {
            if (auto found = types_.find(identity(scope, name)); found != types_.end()) {
                return found->second;
            }
            if (scope.empty()) {
                return nullptr;
            }
            auto separator = scope.rfind("::");
            scope.resize(separator == scope.npos ? 0 : separator);
        }
    }

    Base base(std::string spelling, const UmlType& owner) const {
        std::string type = spelling;
        bool accessed = false;
        bool virtualBase = false;
        while (true) {
            auto word = type.substr(0, type.find_first_of(" \t"));
            if (word == "public" || word == "protected" || word == "private") {
                if (accessed) {
                    throw error(owner, "Repeated access specifier in base '" + spelling + "'");
                }
                accessed = true;
            } else if (word == "virtual") {
                if (virtualBase) {
                    throw error(owner, "Repeated virtual in base '" + spelling + "'");
                }
                virtualBase = true;
            } else {
                break;
            }
            type = trim(std::string_view(type).substr(word.size()));
        }
        if (type.empty()) {
            throw error(owner, "Missing base type");
        }
        auto names = namesIn(type);
        const UmlType* target = names.empty() ? nullptr : lookup(names.front(), owner);
        return {std::move(spelling), std::move(type), target};
    }

    void readBases() {
        for (const auto& [name, type] : types_) {
            auto& entries = bases_[name];
            std::map<std::string, std::string> seen;
            for (const auto& declaration : type->bases) {
                auto item = base(declaration, *type);
                if (item.target == type || isEnum(type->kind) || type->kind == Kind::Union
                    || (item.target && (isEnum(item.target->kind) || item.target->kind == Kind::Union || item.target->final))) {
                    throw error(*type, "Invalid base '" + declaration + "'");
                }
                auto [entry, inserted] = seen.emplace(normalized(item.name), normalized(item.spelling));
                if (inserted) {
                    entries.push_back(std::move(item));
                } else if (entry->second != normalized(item.spelling)) {
                    throw error(*type, "Conflicting clauses for base '" + item.name + "'");
                }
            }
        }
        for (const auto& edge : diagram_.model.edges) {
            auto& entries = bases_.at(edge.source);
            if (std::ranges::any_of(entries, [&](const Base& base) {
                return base.target && base.target->qualifiedName() == edge.target;
            })) {
                continue;
            }
            const auto* parent = types_.at(edge.target);
            const auto* child = types_.at(edge.source);
            if (!parent->templates.empty() || isEnum(parent->kind) || isEnum(child->kind)
                || parent->kind == Kind::Union || child->kind == Kind::Union || parent->final) {
                throw std::invalid_argument(diagram_.edgeLocations.at(edge)
                    + ": Invalid inheritance arrow. Use a typed base clause for a template and supply its arguments.");
            }
            entries.push_back(base("public ::" + edge.target, *child));
        }
    }

    void dependencies(const UmlType& type, Unit& unit, std::set<std::string> parameters) {
        unit.includes.insert(type.includes.begin(), type.includes.end());
        auto add = [&](std::string_view fragment, bool needsDefinition, const std::set<std::string>& shadowed) {
            for (auto name : namesIn(fragment)) {
                if (shadowed.contains(name)) {
                    continue;
                }
                const UmlType* target = lookup(name, type);
                // A scoped enum value or static member needs the declaring type's definition.
                bool complete = needsDefinition;
                while (!target) {
                    auto separator = name.rfind("::");
                    if (separator == name.npos || separator == 0) {
                        break;
                    }
                    name.resize(separator);
                    target = lookup(name, type);
                    complete = true;
                }
                if (!target) {
                    continue;
                }
                const auto& top = rootType(*target);
                if (&top == unit.type) {
                    siblingDependency(type, *target, complete);
                    continue;
                }
                auto key = top.qualifiedName();
                if (complete || !target->owner.empty() || !canForwardDeclare(*target)) {
                    unit.complete.insert(key);
                } else {
                    unit.forward.insert(key);
                }
            }
        };
        bool inTemplate = !parameters.empty();
        for (const auto& parameter : type.templates) {
            parameters.insert(templateName(parameter));
            add(parameter, true, parameters);
        }
        for (const auto& base : bases_.at(type.qualifiedName())) {
            add(base.name, true, parameters);
        }
        add(type.underlying, true, parameters);
        for (const auto& entry : type.enumerators) {
            add(entry.value, true, parameters);
        }
        for (const auto& field : type.fields) {
            add(field.type, !simpleIndirection(field.type), parameters);
            add(field.initializer, true, parameters);
            add(field.bitWidth, true, parameters);
        }
        for (const auto* operation : operations_.at(type.qualifiedName())) {
            auto shadowed = parameters;
            for (const auto& parameter : operation->templates) {
                shadowed.insert(templateName(parameter));
                add(parameter, true, shadowed);
            }
            bool header = hasBodyInHeader(type, *operation, inTemplate);
            auto useStub = hasBody(*operation) && !hasModifier(*operation, "default-outside")
                && operation->name != type.name && operation->name != "~" + type.name && operation->result != "void"
                && !hasModifier(*operation, "constexpr") && !hasModifier(*operation, "consteval");
            unit.inlineStub |= header && useStub;
            unit.sourceStub |= !header && useStub;
            add(operation->result, header && !simpleIndirection(operation->result), shadowed);
            if (operation->name.starts_with("operator ") && operation->result.empty()) {
                add(std::string_view(operation->name).substr(9), header, shadowed);
            }
            for (const auto& parameter : operation->parameters) {
                add(parameter.type, header && !simpleIndirection(parameter.type), shadowed);
                add(parameter.defaultValue, true, shadowed);
            }
            for (const auto& modifier : operation->modifiers) {
                if (modifier.starts_with("noexcept(") || modifier.starts_with("explicit(") || modifier.starts_with("requires ")) {
                    add(modifier, true, shadowed);
                }
            }
        }
        for (const auto* child : children_[type.qualifiedName()]) {
            dependencies(*child, unit, parameters);
        }
    }

    void checkCycles(const std::string& name, std::set<std::string>& active, std::set<std::string>& visited) const {
        if (active.contains(name)) {
            throw error(*types_.at(name), "Cyclic complete-type dependencies involving " + name);
        }
        if (visited.insert(name).second) {
            active.insert(name);
            for (const auto& target : units_.at(name).complete) {
                checkCycles(target, active, visited);
            }
            active.erase(name);
        }
    }

    std::string forwardDeclaration(const UmlType& type) const {
        return std::string(typeKeyword(type.kind)) + " " + type.name + enumBase(type) + ";";
    }

    std::string initialization(const UmlType& owner, const Operation& operation) const {
        if (operation.name != owner.name || operation.initializers) {
            return operation.initializers.value_or("");
        }
        auto parameterFor = [&](const std::string& name, const std::string& type) -> std::string {
            std::vector<std::string> matches;
            for (const auto& parameter : operation.parameters) {
                if (normalized(parameter.type) == normalized(type)) {
                    if (parameter.name == name) {
                        return name;
                    }
                    matches.push_back(parameter.name);
                }
            }
            return matches.size() == 1 ? matches.front() : "";
        };
        std::vector<std::string> items;
        for (const auto& base : bases_.at(owner.qualifiedName())) {
            if (!base.target) {
                continue;
            }
            const auto& parent = *base.target;
            std::vector<const Operation*> choices;
            bool declared = false;
            for (const auto* candidate : operations_.at(parent.qualifiedName())) {
                if (candidate->name == parent.name) {
                    declared = true;
                    if (candidate->access != Access::Private && !hasModifier(*candidate, "delete")) {
                        choices.push_back(candidate);
                    }
                }
            }
            auto optional = [](const auto& parameter) { return !parameter.defaultValue.empty() || parameter.name.empty(); };
            if (!declared || std::ranges::any_of(choices, [&](const auto* candidate) {
                return std::ranges::all_of(candidate->parameters, optional);
            })) {
                continue;
            }
            std::set<std::string> possible;
            for (const auto* candidate : choices) {
                std::vector<std::string> arguments;
                bool matched = true;
                for (const auto& parameter : candidate->parameters) {
                    if (optional(parameter)) {
                        break;
                    }
                    auto argument = parameterFor(parameter.name, parameter.type);
                    if (argument.empty()) {
                        matched = false;
                        break;
                    }
                    arguments.push_back(std::move(argument));
                }
                if (matched) {
                    possible.insert(base.name + "(" + join(arguments, ", ") + ")");
                }
            }
            if (possible.size() != 1) {
                throw error(owner, "Specify {init ...} for the constructor of '" + owner.name
                    + "' to initialize " + base.name);
            }
            items.push_back(*possible.begin());
        }
        for (const auto& field : owner.fields) {
            if (hasModifier(field, "static") || !field.initializer.empty()) {
                continue;
            }
            auto spelling = normalized(withName(field.type, ""));
            if (!spelling.ends_with('&')) {
                continue;
            }
            auto argument = parameterFor(field.name, field.type);
            if (argument.empty()) {
                throw error(owner, "Specify {init ...} to initialize reference field '" + field.name + "'");
            }
            items.push_back(field.name + "(" + argument + ")");
        }
        return join(items, ", ");
    }

    std::string body(const UmlType& owner, const Operation& operation, const std::string& indent) const {
        auto init = initialization(owner, operation);
        std::string text;
        if (!init.empty()) {
            text += "\n" + indent + "    : " + init;
        }
        text += " {\n" + indent + "    // TODO Implement this operation and its contract.\n";
        if (operation.name != owner.name && operation.name != "~" + owner.name && operation.result != "void") {
            if (hasModifier(operation, "constexpr") || hasModifier(operation, "consteval")) {
                auto result = operation.result.empty() ? operation.name.substr(9) : operation.result;
                if (normalized(result).ends_with('&')) {
                    auto found = std::ranges::find_if(operation.parameters, [&](const auto& parameter) {
                        return normalized(parameter.type) == normalized(result) && !parameter.name.empty();
                    });
                    if (found == operation.parameters.end()) {
                        throw error(owner, "A constexpr reference result needs a matching argument for a TODO stub");
                    }
                    text += indent + "    return static_cast<" + result + ">(" + found->name + ");\n";
                } else {
                    text += indent + "    return {};\n";
                }
            } else {
                text += indent + "    std::terminate();\n";
            }
        }
        return text + indent + "}\n";
    }

    void writeType(const UmlType& type, std::string& out, std::string& source, const std::string& indent,
                   const std::string& enclosing, bool inTemplate) {
        auto definitionName = identity(enclosing, type.name);
        bool templated = inTemplate || !type.templates.empty();
        out += typeComment(type, author_, indent);
        if (!type.templates.empty()) {
            out += indent + templateHead(type.templates);
        }
        out += indent + std::string(typeKeyword(type.kind)) + " " + type.name;
        const std::string inside = indent + "    ";
        if (isEnum(type.kind)) {
            out += enumBase(type) + " {\n";
            for (const auto& entry : type.enumerators) {
                out += memberComment("enumerator", entry.name, inside) + inside + entry.name;
                if (!entry.value.empty()) {
                    out += " = " + entry.value;
                }
                out += ",\n";
            }
            out += indent + "};\n";
            return;
        }
        if (type.final) {
            out += " final";
        }
        std::vector<std::string> bases;
        for (const auto& base : bases_.at(type.qualifiedName())) {
            bases.push_back(base.spelling);
        }
        if (!bases.empty()) {
            out += " : " + join(bases, ", ");
        }
        out += " {\n";
        auto currentAccess = type.kind == Kind::Class ? Access::Private : Access::Public;
        auto access = [&](Access value) {
            if (value != currentAccess) {
                out += indent + std::string(accessName(value)) + ":\n";
                currentAccess = value;
            }
        };
        auto nested = definitionOrder(type);
        for (const auto* child : nested) {
            if (canForwardDeclare(*child)) {
                access(child->access);
                out += inside + forwardDeclaration(*child) + "\n";
            }
        }
        for (const auto* child : nested) {
            access(child->access);
            writeType(*child, out, source, inside, definitionName, templated);
            out += "\n";
        }
        for (const auto& field : type.fields) {
            access(field.access);
            out += memberComment("field", field.name, inside);
            out += inside + fieldDeclaration(field) + ";\n";
            if (hasModifier(field, "static") && !hasModifier(field, "inline") && !hasModifier(field, "constexpr")) {
                if (templated || normalized(withName(field.type, "")).ends_with('&')) {
                    source += "// TODO Define " + definitionName + "::" + field.name + " if it is odr-used.\n\n";
                } else {
                    source += fieldDeclaration(field, definitionName) + (field.initializer.empty() ? "{};\n\n" : ";\n\n");
                }
            }
        }
        for (const auto* operation : operations_.at(type.qualifiedName())) {
            access(operation->access);
            out += operationComment(type, *operation, inside);
            if (!operation->templates.empty()) {
                out += inside + templateHead(operation->templates);
            }
            out += inside + renderSignature(*operation);
            if (hasModifier(*operation, "pure")) {
                out += " = 0;\n";
                if (operation->name == "~" + type.name) {
                    if (templated) {
                        throw error(type, "A pure virtual destructor in a template needs an out-of-class definition");
                    }
                    source += renderSignature(*operation, definitionName) + " = default;\n\n";
                }
            } else if (hasModifier(*operation, "delete")) {
                out += " = delete;\n";
            } else if (hasModifier(*operation, "default")
                || (templated && hasModifier(*operation, "default-outside"))) {
                out += " = default;\n";
            } else if (definedInHeader(type, *operation, templated)) {
                out += body(type, *operation, inside);
            } else {
                out += ";\n";
                source += renderSignature(*operation, definitionName);
                source += hasModifier(*operation, "default-outside") ? " = default;\n" : body(type, *operation, "");
                source += "\n";
            }
        }
        out += indent + "};\n";
    }

    static std::string include(const fs::path& target, const fs::path& from) {
        return "#include \"" + pathText(target.lexically_relative(from.parent_path().empty() ? "." : from.parent_path()))
            + "\"\n";
    }

    void writeUnit(const Unit& unit) {
        const auto& type = *unit.type;
        std::string header = "#pragma once\n";
        auto includes = unit.includes;
        if (unit.inlineStub) {
            includes.insert("<exception>");
        }
        for (const auto& operand : includes) {
            header += "#include " + operand + "\n";
        }
        for (const auto& name : unit.complete) {
            header += include(units_.at(name).header, unit.header);
        }
        header += "\n";
        std::string source = "#include \"" + pathText(unit.header.filename()) + "\"\n";
        if (unit.sourceStub) {
            source += "#include <exception>\n";
        }
        for (const auto& name : unit.forward) {
            if (unit.complete.contains(name)) {
                continue;
            }
            const auto& target = *types_.at(name);
            header += namespaceOpen(target.nameSpace) + forwardDeclaration(target) + "\n"
                + namespaceClose(target.nameSpace) + "\n";
            source += include(units_.at(name).header, unit.header);
        }
        header += namespaceOpen(type.nameSpace);
        std::string definitions;
        writeType(type, header, definitions, "", "", false);
        header += namespaceClose(type.nameSpace);
        if (!definitions.empty()) {
            source += "\n" + namespaceOpen(type.nameSpace) + definitions + namespaceClose(type.nameSpace);
        }
        result_.files.emplace(unit.header, std::move(header));
        auto implementation = unit.header;
        implementation.replace_extension(".cpp");
        result_.files.emplace(std::move(implementation), std::move(source));
    }

    const Diagram& diagram_;
    std::string_view author_;
    Skeletons result_;
    std::map<std::string, const UmlType*> types_;
    std::map<std::string, std::vector<const UmlType*>> children_;
    std::map<std::string, std::set<std::string>> nestedComplete_;
    std::map<std::string, std::vector<const Operation*>> operations_;
    std::map<std::string, std::vector<Base>> bases_;
    std::map<std::string, Unit> units_;
};

}

Skeletons generateSkeletons(const Diagram& diagram, std::string_view author) {
    return Generator(diagram, author).generate();
}

}
