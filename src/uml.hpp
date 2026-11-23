#pragma once

#include <compare>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace drawio {

enum class Kind { Class, Struct, Union, Enum, ScopedEnum };
enum class Access { Public, Protected, Private };
enum class EdgeKind { Generalization, Association, Dependency };

struct Field {
    Access access;
    std::string name;
    std::string type;
    std::vector<std::string> modifiers;
    std::string bitWidth;
    std::string initializer;

    bool operator==(const Field&) const = default;
};

struct Parameter {
    std::string name;
    std::string type;
    std::string defaultValue;

    bool operator==(const Parameter&) const = default;
};

struct Operation {
    Access access;
    std::string name;
    std::vector<std::string> templates;
    std::vector<Parameter> parameters;
    std::string result;
    std::vector<std::string> modifiers;
    std::optional<std::string> initializers;

    bool operator==(const Operation&) const = default;
};

struct Enumerator {
    std::string name;
    std::string value;

    bool operator==(const Enumerator&) const = default;
};

struct UmlType {
    std::string name;
    std::string nameSpace;
    std::string owner;
    Kind kind;
    Access access = Access::Public;
    std::vector<std::string> templates;
    std::string specialization;
    std::string underlying;
    bool abstract = false;
    bool final = false;
    std::vector<std::string> bases;
    std::set<std::string> includes;
    std::vector<Field> fields;
    std::vector<Operation> operations;
    std::vector<Enumerator> enumerators;
    std::set<std::string> reverseLimitations;

    std::string qualifiedName() const;
    std::string displayName() const;
    bool operator==(const UmlType&) const = default;
};

struct Edge {
    std::string source;
    std::string target;
    EdgeKind kind;

    auto operator<=>(const Edge&) const = default;
};

struct UmlModel {
    std::vector<UmlType> types;
    std::vector<Edge> edges;
};

std::string_view accessName(Access access);
char accessMarker(Access access);
std::string_view edgeName(EdgeKind kind);
bool isEnum(Kind kind);
bool hasModifier(const Field& field, std::string_view modifier);
bool hasModifier(const Operation& operation, std::string_view modifier);
std::string trim(std::string_view text);
std::string join(const std::vector<std::string>& values, std::string_view separator);
std::string withName(std::string_view type, std::string_view name);
std::string identity(std::string_view scope, std::string_view name);

}
