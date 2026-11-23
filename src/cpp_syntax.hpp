#pragma once

#include "uml.hpp"

namespace drawio {

bool identifier(std::string_view name);
std::vector<std::string> scopeParts(std::string_view name);
std::size_t topLevel(std::string_view text, char character);
std::size_t closeGroup(std::string_view text, std::size_t open);
std::vector<std::string> splitList(std::string_view text);
std::string templateName(std::string_view declaration);
std::string withoutDefault(std::string_view declaration);
std::string checkedFragment(std::string_view text, std::string_view role);

}
