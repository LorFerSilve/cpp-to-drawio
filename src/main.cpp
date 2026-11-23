#include "analyzer.hpp"
#include "cpp_skeletons.hpp"
#include "drawio_writer.hpp"

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <stdexcept>
#include <string_view>

namespace drawio {
namespace {

constexpr std::string_view HELP = R"(C++ to Draw.io
Usage: cpp-to-drawio --source <directory> --output <file.drawio> [options]
       drawio-to-cpp --input <diagram.drawio> --output <directory> [--author <name>]
       cpp-to-drawio --help

  --source <directory>  C++ project or source directory to analyze
  --output <file>       .drawio destination
  --compile-db <path>   compile_commands.json or its directory
  --create-output-dir   Create missing destination directories after analysis
  --overview            Create a namespace overview and detail pages
  --force               Allow replacing an existing output file
  --help                Show this help

Quote paths containing spaces.
You can also run the reverse command as cpp-to-drawio drawio-to-cpp.
Exit codes: 0 success, 2 invalid arguments or input, 3 analysis failure, 4 output failure.
)";

constexpr std::string_view REVERSE_HELP = R"(Usage: drawio-to-cpp --input <diagram.drawio> --output <directory> [--author <name>]

  --input   Editable Draw.io UML class diagram.
  --output  Directory for namespaces and .hpp/.cpp files. Created if needed.
            Existing files are never overwritten.
  --author  Name to include in each type's Doxygen comment.
  --help    Show this help.

Exit codes: 0 success, 2 invalid arguments or input, 3 invalid XML or UML, 4 output failure.
)";

using Options = std::map<std::string_view, std::string_view>;

Options parse(int argc, char** argv, int start, std::initializer_list<std::string_view> values,
              std::initializer_list<std::string_view> switches) {
    Options options;
    for (int i = start; i < argc; ++i) {
        std::string_view option = argv[i];
        auto [entry, inserted] = options.try_emplace(option);
        if (!inserted) {
            throw std::invalid_argument("Repeated argument " + std::string(option) + ".");
        }
        if (std::ranges::find(values, option) != values.end()) {
            if (++i >= argc || trim(argv[i]).empty() || std::string_view(argv[i]).starts_with("--")) {
                throw std::invalid_argument(std::string(option == "--author" ? "Missing name after " : "Missing path after ")
                                            + std::string(option) + ".");
            }
            entry->second = argv[i];
        } else if (std::ranges::find(switches, option) == switches.end()) {
            throw std::invalid_argument("Unexpected argument " + std::string(option) + ".");
        }
    }
    return options;
}

fs::path absolutePath(std::string_view value) {
    return fs::absolute(pathFromUtf8(value)).lexically_normal();
}

int forward(int argc, char** argv, const fs::path& executable) {
    Options options;
    fs::path source;
    fs::path output;
    fs::path database;
    try {
        options = parse(argc, argv, 1, {"--source", "--output", "--compile-db"},
                        {"--force", "--overview", "--create-output-dir"});
        if (!options.contains("--source") || !options.contains("--output")) {
            throw std::invalid_argument("Both --source and --output are required.");
        }
        source = absolutePath(options.at("--source"));
        output = absolutePath(options.at("--output"));
        if (options.contains("--compile-db")) {
            database = absolutePath(options.at("--compile-db"));
        }
        std::string extension = pathText(output.extension());
        std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return std::tolower(c); });
        if (extension != ".drawio") {
            throw std::invalid_argument("--output must name a .drawio file.");
        }
    } catch (const std::exception& error) {
        llvm::errs() << "Argument error: " << error.what() << "\nUse --help to see the usage.\n";
        return 2;
    }
    bool force = options.contains("--force");
    bool createDirectory = options.contains("--create-output-dir");
    try {
        validateDiagramOutput(output, force, createDirectory);
    } catch (const std::exception& error) {
        llvm::errs() << "Output error: " << error.what() << '\n';
        return 4;
    }

    Analysis result;
    try {
        result = analyze(source, database, executable);
    } catch (const AnalysisError& error) {
        llvm::errs() << "Analysis failed. No diagram was written.\n" << error.what() << '\n';
        return 3;
    } catch (const std::exception& error) {
        llvm::errs() << "Source or compilation database error: " << error.what() << '\n';
        return 2;
    }
    for (const auto& warning : result.warnings) {
        llvm::errs() << "Warning: " << warning << '\n';
    }
    if (result.incomplete) {
        llvm::errs() << "Warning: Analysis is incomplete. See the diagnostics above.\n";
    }
    try {
        writeDiagramFile(output, force, createDirectory, [&] {
            return writeDrawio(result.model, options.contains("--overview"));
        });
    } catch (const std::exception& error) {
        llvm::errs() << "Cannot write " << pathText(output) << ": " << error.what() << '\n';
        return 4;
    }
    llvm::outs() << "Processed " << result.fileCount << " C++ files, " << result.model.types.size()
                 << " UML types, " << result.model.edges.size() << " relationships, " << result.warnings.size()
                 << " warnings.\nWrote " << pathText(output) << '\n';
    return 0;
}

int reverse(int argc, char** argv, int start) {
    if (argc == start + 1 && std::string_view(argv[start]) == "--help") {
        llvm::outs() << REVERSE_HELP;
        return 0;
    }
    fs::path input;
    fs::path output;
    std::string author;
    try {
        auto options = parse(argc, argv, start, {"--input", "--output", "--author"}, {});
        if (!options.contains("--input") || !options.contains("--output")) {
            throw std::invalid_argument("Both --input and --output are required.");
        }
        input = absolutePath(options.at("--input"));
        output = absolutePath(options.at("--output"));
        if (!fs::is_regular_file(input)) {
            throw std::invalid_argument("Input is not a Draw.io file: " + pathText(input));
        }
        if (options.contains("--author")) {
            author = trim(options.at("--author"));
            if (std::ranges::any_of(options.at("--author"), [](unsigned char ch) { return ch < 32 || ch == 127; })
                || author.find("\xe2\x80\xa8") != author.npos || author.find("\xe2\x80\xa9") != author.npos) {
                throw std::invalid_argument("--author must contain a name on one line.");
            }
        }
    } catch (const std::exception& error) {
        llvm::errs() << "Argument error: " << error.what() << "\nUse drawio-to-cpp --help to see the usage.\n";
        return 2;
    }
    Skeletons skeletons;
    std::size_t typeCount = 0;
    try {
        auto diagram = readDrawio(input);
        typeCount = diagram.model.types.size();
        skeletons = generateSkeletons(diagram, author);
    } catch (const std::exception& error) {
        llvm::errs() << "Cannot convert " << pathText(input) << ": " << error.what() << '\n';
        return 3;
    }
    for (const auto& warning : skeletons.warnings) {
        llvm::errs() << "Warning: " << warning << '\n';
    }
    try {
        writeSourceFiles(output, skeletons.files);
    } catch (const std::exception& error) {
        llvm::errs() << "Output error: " << error.what() << '\n';
        return 4;
    }
    llvm::outs() << "Generated " << skeletons.files.size() << " C++ files from " << typeCount
                 << " UML types.\nWrote " << pathText(output) << '\n';
    return 0;
}

}
}

int main(int argc, char** argv) {
    llvm::InitLLVM init(argc, argv);
    auto program = drawio::pathText(drawio::pathFromUtf8(argv[0]).stem());
#ifdef _WIN32
    std::ranges::transform(program, program.begin(), [](unsigned char ch) { return std::tolower(ch); });
#endif
    if (program == "drawio-to-cpp") {
        return drawio::reverse(argc, argv, 1);
    }
    if (argc > 1 && std::string_view(argv[1]) == "drawio-to-cpp") {
        return drawio::reverse(argc, argv, 2);
    }
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        llvm::outs() << drawio::HELP;
        return 0;
    }
    auto executable = llvm::sys::fs::getMainExecutable(argv[0], nullptr);
    return drawio::forward(argc, argv, drawio::pathFromUtf8(executable));
}
