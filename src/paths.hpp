#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace drawio {

namespace fs = std::filesystem;

fs::path pathFromUtf8(std::string_view value);
std::string pathText(const fs::path& path);
std::string readFile(const fs::path& path);
bool isWithin(const fs::path& path, const fs::path& directory);

void validateDiagramOutput(const fs::path& path, bool force, bool createDirectory);
void writeDiagramFile(const fs::path& path, bool force, bool createDirectory,
                      const std::function<std::string()>& contents);
void writeSourceFiles(const fs::path& directory, const std::map<fs::path, std::string>& files);

}
