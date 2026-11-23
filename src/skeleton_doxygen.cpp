#include "skeleton_doxygen.hpp"
#include "cpp_syntax.hpp"

namespace drawio {
namespace {

std::string escape(std::string_view text) {
    std::string result;
    for (std::size_t i = 0; i < text.size(); ++i) {
        switch (text[i]) {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '\\': result += "&#92;"; break;
            case '@': result += "&#64;"; break;
            case '/':
                result += i && text[i - 1] == '*' ? "&#47;" : "/";
                break;
            default: result += text[i]; break;
        }
    }
    return result;
}

void addTemplateTags(std::vector<std::string>& tags, const std::vector<std::string>& parameters) {
    for (const auto& parameter : parameters) {
        auto name = templateName(parameter);
        if (!name.empty()) {
            tags.push_back("@tparam " + name + " TODO Describe the template argument and its requirements.");
        }
    }
}

std::string comment(std::string headline, const std::vector<std::string>& tags, const std::string& indent) {
    std::string result = indent + "/**\n" + indent + " * " + std::move(headline) + "\n";
    if (!tags.empty()) {
        result += indent + " *\n";
        for (const auto& tag : tags) {
            result += indent + " * " + tag + "\n";
        }
    }
    return result + indent + " */\n";
}

}

std::string typeComment(const UmlType& type, std::string_view author, const std::string& indent) {
    std::vector<std::string> tags;
    addTemplateTags(tags, type.templates);
    if (!author.empty()) {
        tags.push_back("@author " + escape(author));
    }
    return comment("TODO Describe the role and invariants of " + escape(type.name) + ".", tags, indent);
}

std::string operationComment(const UmlType& type, const Operation& operation, const std::string& indent) {
    std::vector<std::string> tags;
    addTemplateTags(tags, operation.templates);
    for (const auto& parameter : operation.parameters) {
        if (!parameter.name.empty()) {
            tags.push_back("@param " + parameter.name + " TODO Describe this argument and its preconditions.");
        }
    }
    std::string summary = "TODO Describe the behavior and contract of " + escape(operation.name) + ".";
    if (operation.name == type.name) {
        summary = "TODO Describe how this constructor initializes " + escape(type.name) + ".";
    } else if (operation.name == "~" + type.name) {
        summary = "TODO Describe the cleanup performed by this destructor.";
    } else if (operation.result != "void") {
        tags.push_back("@return TODO Describe the returned value and its guarantees.");
    }
    return comment(std::move(summary), tags, indent);
}

std::string memberComment(std::string_view kind, const std::string& name, const std::string& indent) {
    std::string label = std::string(kind);
    if (!name.empty()) {
        label += " " + escape(name);
    }
    return comment("TODO Describe the purpose of " + label + ".", {}, indent);
}

}
