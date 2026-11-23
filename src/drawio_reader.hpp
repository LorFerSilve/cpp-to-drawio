#pragma once

#include "paths.hpp"
#include "uml.hpp"

namespace drawio {

struct Diagram {
    UmlModel model;
    std::map<std::string, std::string> locations;
    std::map<Edge, std::string> edgeLocations;
};

Diagram readDrawio(const fs::path& input);

}
