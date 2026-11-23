#include "cpp_syntax.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace drawio {
namespace {

bool digitSeparator(std::string_view text, std::size_t at) {
    if (at == 0 || at + 1 == text.size() || !std::isalnum(static_cast<unsigned char>(text[at + 1]))) {
        return false;
    }
    auto start = at;
    while (start > 0 && (std::isalnum(static_cast<unsigned char>(text[start - 1]))
                         || text[start - 1] == '\'' || text[start - 1] == '_')) {
        --start;
    }
    return std::isdigit(static_cast<unsigned char>(text[start]));
}

class Nesting {
public:
    bool top() const {
        return groups_.empty() && quote_ == 0;
    }

    void read(std::string_view text, std::size_t i) {
        char ch = text[i];
        if (quote_) {
            if (escape_) {
                escape_ = false;
            } else if (ch == '\\') {
                escape_ = true;
            } else if (ch == quote_) {
                quote_ = 0;
            }
            return;
        }
        if (ch == '"' || (ch == '\'' && !digitSeparator(text, i))) {
            quote_ = ch;
        } else if (ch == '(' || ch == '[' || ch == '{') {
            groups_.push_back(ch == '(' ? ')' : ch == '[' ? ']' : '}');
        } else if (ch == '<' && (groups_.empty() || groups_.back() == '>')) {
            groups_.push_back('>');
        } else if (!groups_.empty() && groups_.back() == ch) {
            groups_.pop_back();
        } else if (ch == ')' || ch == ']' || ch == '}') {
            throw std::invalid_argument("Mismatched delimiters in C++ declaration");
        }
    }

    void finish() const {
        if (!top()) {
            throw std::invalid_argument("Unterminated group or literal in C++ declaration");
        }
    }

private:
    std::vector<char> groups_;
    char quote_ = 0;
    bool escape_ = false;
};

bool initial(unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_' || ch >= 0x80;
}

bool continuation(unsigned char ch) {
    return initial(ch) || (ch >= '0' && ch <= '9');
}

}

bool identifier(std::string_view name) {
    static const std::set<std::string_view> reserved = {
        "alignas", "alignof", "asm", "auto", "bool", "break", "case", "catch", "char", "char8_t", "char16_t",
        "char32_t", "class", "const", "consteval", "constexpr", "constinit", "continue", "decltype", "default",
        "delete", "do", "double", "else", "enum", "explicit", "export", "extern", "false", "float", "for",
        "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "nullptr",
        "operator", "private", "protected", "public", "register", "return", "short", "signed", "sizeof",
        "static", "static_assert", "struct", "switch", "template", "this", "thread_local", "throw", "true",
        "try", "typedef", "typename", "union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t",
        "while", "and", "and_eq", "bitand", "bitor", "compl", "not", "not_eq", "or", "or_eq", "xor", "xor_eq",
        "co_await", "co_return", "co_yield", "concept", "requires", "const_cast", "dynamic_cast",
        "reinterpret_cast", "static_cast", "typeid",
    };
    return !name.empty() && initial(static_cast<unsigned char>(name.front()))
        && std::ranges::all_of(name.substr(1), continuation) && !reserved.contains(name);
}

std::vector<std::string> scopeParts(std::string_view name) {
    std::vector<std::string> parts;
    while (true) {
        auto separator = name.find("::");
        auto part = name.substr(0, separator);
        if (!identifier(part)) {
            throw std::invalid_argument("Invalid C++ name '" + std::string(name) + "'");
        }
        parts.emplace_back(part);
        if (separator == std::string_view::npos) {
            return parts;
        }
        name.remove_prefix(separator + 2);
    }
}

std::size_t topLevel(std::string_view text, char character) {
    Nesting nesting;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (nesting.top() && text[i] == character) {
            return i;
        }
        nesting.read(text, i);
    }
    nesting.finish();
    return std::string_view::npos;
}

std::size_t closeGroup(std::string_view text, std::size_t open) {
    Nesting nesting;
    for (std::size_t i = open; i < text.size(); ++i) {
        nesting.read(text, i);
        if (nesting.top()) {
            return i;
        }
    }
    throw std::invalid_argument("Unterminated group in '" + std::string(text) + "'");
}

std::vector<std::string> splitList(std::string_view text) {
    if (trim(text).empty()) {
        return {};
    }
    std::vector<std::string> values;
    Nesting nesting;
    std::size_t start = 0;
    auto append = [&](std::size_t end) {
        auto value = trim(text.substr(start, end - start));
        if (value.empty()) {
            throw std::invalid_argument("Empty item in C++ declaration list");
        }
        values.push_back(std::move(value));
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (nesting.top() && text[i] == ',') {
            append(i);
            start = i + 1;
        } else {
            nesting.read(text, i);
        }
    }
    nesting.finish();
    append(text.size());
    return values;
}

std::string withoutDefault(std::string_view declaration) {
    return trim(declaration.substr(0, topLevel(declaration, '=')));
}

std::string templateName(std::string_view declaration) {
    auto prefix = withoutDefault(declaration);
    auto last = prefix.size();
    while (last && !continuation(static_cast<unsigned char>(prefix[last - 1]))) {
        --last;
    }
    auto first = last;
    while (first && continuation(static_cast<unsigned char>(prefix[first - 1]))) {
        --first;
    }
    auto name = prefix.substr(first, last - first);
    return identifier(name) ? name : std::string{};
}

std::string checkedFragment(std::string_view text, std::string_view role) {
    auto value = trim(text);
    if (value.empty()) {
        throw std::invalid_argument(std::string(role) + " is missing");
    }
    if (value.find_first_of("\n\r") != std::string::npos || topLevel(value, ';') != std::string::npos) {
        throw std::invalid_argument(std::string(role) + " must be a single C++ fragment");
    }
    return value;
}

}
