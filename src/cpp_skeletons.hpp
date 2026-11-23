#pragma once

#include "drawio_reader.hpp"

namespace drawio {

struct Skeletons {
    std::map<fs::path, std::string> files;
    std::vector<std::string> warnings;
};

Skeletons generateSkeletons(const Diagram& diagram, std::string_view author);

}
