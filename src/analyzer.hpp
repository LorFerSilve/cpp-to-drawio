#pragma once

#include "paths.hpp"
#include "uml.hpp"

#include <cstddef>

namespace drawio {

struct Analysis {
    UmlModel model;
    std::size_t fileCount = 0;
    std::vector<std::string> warnings;
    bool incomplete = false;
};

class AnalysisError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

Analysis analyze(const fs::path& source, const fs::path& compileDatabase, const fs::path& executable);

}
