#include "paths.hpp"

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace drawio {
namespace {

fs::file_status fileStatus(const fs::path& path) {
    std::error_code error;
    auto result = fs::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        throw fs::filesystem_error("Cannot inspect path", path, error);
    }
    return result;
}

class StagedFile {
public:
    StagedFile(const fs::path& directory, std::string_view contents) {
        llvm::SmallString<256> name;
        int fd = -1;
        auto error = llvm::sys::fs::createUniqueFile(pathText(directory / ".cpp-to-drawio-%%%%%%.tmp"), fd, name);
        if (error) {
            throw fs::filesystem_error("Cannot stage output", directory, error);
        }
        path_ = pathFromUtf8(std::string_view(name.data(), name.size()));
        llvm::raw_fd_ostream stream(fd, true);
        stream << contents;
        stream.close();
        if (stream.has_error()) {
            auto writeError = stream.error();
            stream.clear_error();
            remove();
            throw fs::filesystem_error("Cannot stage output", directory, writeError);
        }
    }

    StagedFile(const StagedFile&) = delete;
    StagedFile& operator=(const StagedFile&) = delete;

    ~StagedFile() {
        remove();
    }

    void publish(const fs::path& path, bool force) const {
        if (force) {
            fs::rename(path_, path);
            return;
        }
        std::error_code error;
        fs::create_hard_link(path_, path, error);
        if (error) {
            if (error == std::errc::file_exists || fs::exists(fileStatus(path))) {
                throw std::runtime_error("Output file exists: " + pathText(path) + ". Use --force to replace it.");
            }
            throw fs::filesystem_error("Cannot publish output", path, error);
        }
    }

    void rollback(const fs::path& path) const {
        std::error_code ignored;
        if (fs::equivalent(path_, path, ignored)) {
            fs::remove(path, ignored);
        }
    }

private:
    void remove() const {
        std::error_code ignored;
        fs::remove(path_, ignored);
    }

    fs::path path_;
};

void checkSourceTarget(const fs::path& root, const fs::path& relative) {
    auto directory = root;
    auto check = [](const fs::path& path) {
        auto status = fileStatus(path);
        if (fs::exists(status) && !fs::is_directory(status)) {
            throw std::runtime_error("Output path is not a plain directory: " + pathText(path));
        }
    };
    check(directory);
    auto canonicalRoot = fs::weakly_canonical(root);
    for (const auto& part : relative.parent_path()) {
        directory /= part;
        check(directory);
        auto real = fs::weakly_canonical(directory);
        if (!isWithin(real, canonicalRoot)) {
            throw std::runtime_error("Output path leaves the destination directory: " + pathText(directory));
        }
    }
    auto target = root / relative;
    if (fs::exists(fileStatus(target))) {
        throw std::runtime_error("Output file exists: " + pathText(target) + ". Choose a different output directory.");
    }
}

void createDirectories(const fs::path& directory, std::vector<fs::path>& created) {
    if (fs::is_directory(directory)) {
        return;
    }
    if (directory.empty() || directory == directory.parent_path()) {
        throw std::runtime_error("Output filesystem root is unavailable: " + pathText(directory));
    }
    createDirectories(directory.parent_path(), created);
    if (fs::create_directory(directory)) {
        created.push_back(directory);
    }
}

}

fs::path pathFromUtf8(std::string_view value) {
    return fs::path(std::u8string(value.begin(), value.end()));
}

std::string pathText(const fs::path& path) {
    auto utf8 = path.generic_u8string();
    return {utf8.begin(), utf8.end()};
}

std::string readFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream output;
    if (!input) {
        throw std::runtime_error("Cannot open " + pathText(path));
    }
    output << input.rdbuf();
    if (input.bad() || output.bad()) {
        throw std::runtime_error("Cannot read " + pathText(path));
    }
    return output.str();
}

bool isWithin(const fs::path& path, const fs::path& directory) {
    auto item = path.begin();
    for (const auto& part : directory) {
        if (item == path.end() || *item != part) {
            return false;
        }
        ++item;
    }
    return true;
}

void validateDiagramOutput(const fs::path& path, bool force, bool createDirectory) {
    auto parent = path.parent_path();
    if (!fs::exists(parent)) {
        if (!createDirectory) {
            throw std::runtime_error("Output directory does not exist: " + pathText(parent)
                                     + ". Use --create-output-dir to create it.");
        }
    } else if (!fs::is_directory(parent)) {
        throw std::runtime_error("Output parent is not a directory: " + pathText(parent));
    }
    auto target = fileStatus(path);
    if (fs::exists(target)) {
        if (!fs::is_regular_file(target)) {
            throw std::runtime_error("Output path is not a regular file: " + pathText(path));
        }
        if (!force) {
            throw std::runtime_error("Output file exists: " + pathText(path) + ". Use --force to replace it.");
        }
    }
}

void writeDiagramFile(const fs::path& path, bool force, bool createDirectory,
                      const std::function<std::string()>& contents) {
    std::string xml = contents();
    if (createDirectory) {
        fs::create_directories(path.parent_path());
    }
    StagedFile staged(path.parent_path(), xml);
    validateDiagramOutput(path, force, false);
    staged.publish(path, force);
}

void writeSourceFiles(const fs::path& directory, const std::map<fs::path, std::string>& files) {
    for (const auto& [relative, contents] : files) {
        checkSourceTarget(directory, relative);
    }
    struct Pending {
        fs::path relative;
        std::unique_ptr<StagedFile> file;
    };
    std::vector<fs::path> created;
    std::vector<Pending> staged;
    std::size_t published = 0;
    try {
        for (const auto& [relative, contents] : files) {
            checkSourceTarget(directory, relative);
            auto parent = (directory / relative).parent_path();
            createDirectories(parent, created);
            staged.push_back({relative, std::make_unique<StagedFile>(parent, contents)});
        }
        for (const auto& pending : staged) {
            checkSourceTarget(directory, pending.relative);
            pending.file->publish(directory / pending.relative, false);
            ++published;
        }
    } catch (...) {
        for (std::size_t i = 0; i < published; ++i) {
            staged[i].file->rollback(directory / staged[i].relative);
        }
        staged.clear();
        for (auto path = created.rbegin(); path != created.rend(); ++path) {
            std::error_code ignored;
            fs::remove(*path, ignored);
        }
        throw;
    }
}

}
