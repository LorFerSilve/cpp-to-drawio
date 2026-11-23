#include "uml.hpp"

#include <algorithm>

namespace drawio {

std::string identity(std::string_view scope, std::string_view name) {
    if (scope.empty()) {
        return std::string(name);
    }
    return std::string(scope) + "::" + std::string(name);
}

std::string UmlType::qualifiedName() const {
    return identity(owner.empty() ? nameSpace : owner, name) + specialization;
}

std::string UmlType::displayName() const {
    std::string qualified = qualifiedName();
    return nameSpace.empty() ? qualified : qualified.substr(nameSpace.size() + 2);
}

std::string_view accessName(Access access) {
    switch (access) {
        case Access::Public: return "public";
        case Access::Protected: return "protected";
        case Access::Private: return "private";
    }
    return {};
}

char accessMarker(Access access) {
    switch (access) {
        case Access::Public: return '+';
        case Access::Protected: return '#';
        case Access::Private: return '-';
    }
    return '+';
}

std::string_view edgeName(EdgeKind kind) {
    switch (kind) {
        case EdgeKind::Generalization: return "generalization";
        case EdgeKind::Association: return "association";
        case EdgeKind::Dependency: return "dependency";
    }
    return {};
}

bool isEnum(Kind kind) {
    return kind == Kind::Enum || kind == Kind::ScopedEnum;
}

bool hasModifier(const Field& field, std::string_view modifier) {
    return std::ranges::find(field.modifiers, modifier) != field.modifiers.end();
}

bool hasModifier(const Operation& operation, std::string_view modifier) {
    return std::ranges::find(operation.modifiers, modifier) != operation.modifiers.end();
}

std::string trim(std::string_view text) {
    std::size_t first = text.find_first_not_of(" \t\r\n\f\v");
    if (first == std::string_view::npos) {
        return {};
    }
    return std::string(text.substr(first, text.find_last_not_of(" \t\r\n\f\v") - first + 1));
}

std::string join(const std::vector<std::string>& values, std::string_view separator) {
    std::string result;
    bool first = true;
    for (const auto& value : values) {
        if (!first) {
            result += separator;
        }
        result += value;
        first = false;
    }
    return result;
}

std::string withName(std::string_view type, std::string_view name) {
    std::string result(type);
    if (auto marker = result.find('$'); marker != std::string::npos) {
        result.replace(marker, 1, name);
    } else if (!name.empty()) {
        if (!result.empty()) {
            result += ' ';
        }
        result += name;
    }
    return result;
}

}
