#pragma once

#include "uml.hpp"

namespace drawio {

std::string typeComment(const UmlType& type, std::string_view author, const std::string& indent);
std::string operationComment(const UmlType& type, const Operation& operation, const std::string& indent);
std::string memberComment(std::string_view kind, const std::string& name, const std::string& indent);

}
