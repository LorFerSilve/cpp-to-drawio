#pragma once

#include "paths.hpp"
#include "uml.hpp"

#include <cstddef>

namespace drawio {

struct StaticAnalysisFinding {
    std::string file;
    unsigned line;
    unsigned column;
    std::string checker;
    std::string message;

    auto operator<=>(const StaticAnalysisFinding&) const = default;
};

struct Analysis {
    UmlModel model;
    std::size_t fileCount = 0;
    std::vector<std::string> warnings;
    std::vector<StaticAnalysisFinding> findings;
    bool incomplete = false;
};

class AnalysisError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

Analysis analyze(const fs::path& source, const fs::path& compileDatabase, const fs::path& executable,
                 bool runStaticAnalysis);

}
