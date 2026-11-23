#include "analyzer.hpp"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Attr.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/QualTypeNames.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Lex/Lexer.h>
#include <clang/Tooling/JSONCompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <tuple>

namespace drawio {
namespace {

namespace ct = clang::tooling;
using clang::SourceLocation;

bool sourceExtension(const fs::path& path) {
    std::string extension = pathText(path.extension());
    if (extension == ".C") {
        return true;
    }
    std::ranges::transform(extension, extension.begin(), [](unsigned char ch) { return std::tolower(ch); });
    return extension == ".cpp" || extension == ".cc" || extension == ".cxx" || extension == ".c++" || extension == ".cp";
}

bool ignoredDirectory(const fs::path& path) {
    std::string name = pathText(path.filename());
    return name == ".git" || name == ".vs" || name == "build" || name == "out" || name == "dist"
        || name == "node_modules" || name.starts_with("cmake-build-") || name.starts_with("build-");
}

bool excluded(const fs::path& path, const fs::path& source) {
    if (path == source || !isWithin(path, source)) {
        return true;
    }
    for (auto parent = path.parent_path(); parent != source; parent = parent.parent_path()) {
        if (ignoredDirectory(parent)) {
            return true;
        }
    }
    return false;
}

fs::path locateDatabase(const fs::path& source, fs::path database) {
    if (!database.empty()) {
        if (fs::is_directory(database)) {
            database /= "compile_commands.json";
        }
        if (!fs::is_regular_file(database)) {
            throw std::invalid_argument("Compilation database does not exist: " + pathText(database));
        }
        return fs::canonical(database);
    }
    for (auto directory = source;; directory = directory.parent_path()) {
        for (const auto& candidate : {directory / "compile_commands.json",
                                      directory / "build" / "compile_commands.json"}) {
            if (fs::is_regular_file(candidate)) {
                return fs::canonical(candidate);
            }
        }
        if (directory == directory.root_path()) {
            break;
        }
    }
    throw std::invalid_argument("No compile_commands.json found for " + pathText(source) + ". Use --compile-db.");
}

std::string location(const clang::SourceManager& manager, SourceLocation where) {
    auto position = manager.getPresumedLoc(manager.getExpansionLoc(where));
    if (position.isInvalid()) {
        return {};
    }
    return std::string(position.getFilename()) + ":" + std::to_string(position.getLine()) + ":"
           + std::to_string(position.getColumn());
}

class Diagnostics final : public clang::DiagnosticConsumer {
public:
    std::set<std::string> warnings;
    std::set<std::string> errors;

    void HandleDiagnostic(clang::DiagnosticsEngine::Level level, const clang::Diagnostic& diagnostic) override {
        clang::DiagnosticConsumer::HandleDiagnostic(level, diagnostic);
        if (level < clang::DiagnosticsEngine::Warning) {
            return;
        }
        llvm::SmallString<256> message;
        diagnostic.FormatDiagnostic(message);
        std::string prefix;
        if (diagnostic.hasSourceManager()) {
            prefix = location(diagnostic.getSourceManager(), diagnostic.getLocation());
        }
        if (!prefix.empty()) {
            prefix += ": ";
        }
        if (level >= clang::DiagnosticsEngine::Error) {
            errors.insert(prefix + "error: " + message.str().str());
        } else {
            warnings.insert(prefix + message.str().str());
        }
    }
};

class SelectedCommands final : public ct::CompilationDatabase {
public:
    std::map<std::string, std::vector<ct::CompileCommand>> byFile;

    std::vector<ct::CompileCommand> getCompileCommands(llvm::StringRef file) const override {
        std::error_code error;
        auto path = fs::weakly_canonical(pathFromUtf8(file.str()), error);
        auto entry = byFile.find(error ? file.str() : pathText(path));
        return entry == byFile.end() ? std::vector<ct::CompileCommand>{} : entry->second;
    }
};

SelectedCommands selectCommands(const ct::CompilationDatabase& database, const fs::path& source,
                                const fs::path& databaseFile) {
    SelectedCommands selection;
    for (auto command : database.getAllCompileCommands()) {
        auto directory = pathFromUtf8(command.Directory);
        if (directory.is_relative()) {
            directory = databaseFile.parent_path() / directory;
        }
        auto file = fs::weakly_canonical(directory / pathFromUtf8(command.Filename));
        if (!sourceExtension(file) || excluded(file, source)) {
            continue;
        }
        if (!fs::is_regular_file(file)) {
            throw AnalysisError(pathText(file) + ":1:1: error: Source listed in compile_commands.json is missing.");
        }
        if (!fs::is_directory(directory) || command.CommandLine.empty()) {
            throw std::invalid_argument("Invalid compile command for " + pathText(file));
        }
        command.Directory = pathText(fs::canonical(directory));
        command.Filename = pathText(file);
        selection.byFile[command.Filename].push_back(std::move(command));
    }
    for (auto& [file, commands] : selection.byFile) {
        auto key = [](const auto& command) { return std::tie(command.Directory, command.CommandLine); };
        std::ranges::sort(commands, [&](const auto& left, const auto& right) { return key(left) < key(right); });
        commands.erase(std::unique(commands.begin(), commands.end(),
                                   [&](const auto& left, const auto& right) { return key(left) == key(right); }),
                       commands.end());
    }
    return selection;
}

void findMissingCommands(const fs::path& source, const SelectedCommands& commands, Analysis& result) {
    for (auto current = fs::recursive_directory_iterator(source); current != fs::recursive_directory_iterator{};
         ++current) {
        auto status = current->symlink_status();
        if (fs::is_symlink(status) || (fs::is_directory(status) && ignoredDirectory(current->path()))) {
            current.disable_recursion_pending();
            continue;
        }
        if (!fs::is_regular_file(status) || !sourceExtension(current->path())) {
            continue;
        }
        if (!commands.byFile.contains(pathText(fs::canonical(current->path())))) {
            result.warnings.push_back(pathText(current->path()) + ":1:1: No compilation command; file skipped.");
            result.incomplete = true;
        }
    }
    std::ranges::sort(result.warnings);
}

Access access(clang::AccessSpecifier specifier, Access fallback = Access::Public) {
    switch (specifier) {
        case clang::AS_public: return Access::Public;
        case clang::AS_protected: return Access::Protected;
        case clang::AS_private: return Access::Private;
        case clang::AS_none: return fallback;
    }
    return fallback;
}

std::string oneLine(std::string text) {
    std::replace_if(text.begin(), text.end(), [](char ch) { return ch == '\n' || ch == '\r' || ch == '\t'; }, ' ');
    return trim(text);
}

const clang::Expr* constraintExpression(const auto& constraint) {
    if constexpr (requires { constraint.ConstraintExpr; }) {
        return constraint.ConstraintExpr;
    } else {
        return constraint;
    }
}

struct Definition {
    UmlType type;
    std::string location;
};

struct Collected {
    std::map<std::string, Definition> definitions;
    std::set<Edge> edges;
    std::set<std::string> conflicts;

    void merge(UmlType type, std::string where) {
        auto name = type.qualifiedName();
        auto found = definitions.find(name);
        if (found == definitions.end()) {
            definitions.emplace(std::move(name), Definition{std::move(type), std::move(where)});
            return;
        }
        auto& previous = found->second;
        // A constructor definition may be visible in only one translation unit.
        UmlType combined = previous.type;
        combined.includes.insert(type.includes.begin(), type.includes.end());
        type.includes = combined.includes;
        combined.reverseLimitations.insert(type.reverseLimitations.begin(), type.reverseLimitations.end());
        type.reverseLimitations = combined.reverseLimitations;
        if (type.operations.size() == combined.operations.size()) {
            for (std::size_t i = 0; i < type.operations.size(); ++i) {
                auto& before = combined.operations[i];
                auto& after = type.operations[i];
                if (!before.initializers && after.initializers) {
                    before.initializers = after.initializers;
                    if (before.parameters.size() == after.parameters.size()) {
                        for (std::size_t p = 0; p < before.parameters.size(); ++p) {
                            before.parameters[p].name = after.parameters[p].name;
                        }
                    }
                } else if (before.initializers && !after.initializers) {
                    after.initializers = before.initializers;
                    if (before.parameters.size() == after.parameters.size()) {
                        for (std::size_t p = 0; p < before.parameters.size(); ++p) {
                            after.parameters[p].name = before.parameters[p].name;
                        }
                    }
                }
                // An out-of-line = default is only visible in its owning translation unit.
                for (auto* operation : {&before, &after}) {
                    auto* other = operation == &before ? &after : &before;
                    if (!hasModifier(*operation, "default-outside") && hasModifier(*other, "default-outside")) {
                        operation->modifiers.push_back("default-outside");
                    }
                }
            }
        }
        if (previous.location != where || combined != type) {
            conflicts.insert(where + ": error: Conflicting definitions for " + name + " (also at "
                             + previous.location + "). Select one consistent build configuration.");
        } else {
            previous.type = std::move(combined);
        }
    }
};

class AstModel {
public:
    AstModel(clang::ASTContext& context, const fs::path& source, Collected& collected)
        : context_(context), manager_(context.getSourceManager()), source_(source), collected_(collected),
          policy_(context.getPrintingPolicy()) {
        policy_.SuppressTagKeyword = true;
        policy_.SuppressUnwrittenScope = true;
        policy_.SuppressDefaultTemplateArgs = true;
        policy_.CleanUglifiedParameters = true;
    }

    void visit(const clang::DeclContext& scope) {
        for (const auto* declaration : scope.decls()) {
            if (const auto* space = llvm::dyn_cast<clang::NamespaceDecl>(declaration)) {
                if (!space->isAnonymousNamespace()) {
                    visit(*space);
                }
            } else if (const auto* linkage = llvm::dyn_cast<clang::LinkageSpecDecl>(declaration)) {
                visit(*linkage);
            } else if (const auto* exported = llvm::dyn_cast<clang::ExportDecl>(declaration)) {
                visit(*exported);
            } else if (const auto* templated = llvm::dyn_cast<clang::ClassTemplateDecl>(declaration)) {
                record(*templated->getTemplatedDecl());
            } else if (const auto* node = llvm::dyn_cast<clang::CXXRecordDecl>(declaration)) {
                record(*node);
            } else if (const auto* node = llvm::dyn_cast<clang::EnumDecl>(declaration)) {
                enumeration(*node);
            }
        }
    }

private:
    std::optional<fs::path> file(clang::FileID id) {
        auto [entry, inserted] = files_.try_emplace(id.getHashValue());
        if (inserted) {
            if (auto ref = manager_.getFileEntryRefForID(id)) {
                auto real = ref->getFileEntry().tryGetRealPathName();
                auto path = pathFromUtf8((real.empty() ? ref->getName() : real).str());
                std::error_code error;
                auto absolute = fs::weakly_canonical(path, error);
                if (!error && absolute.is_absolute()) {
                    entry->second = std::move(absolute);
                }
            }
        }
        return entry->second;
    }

    bool inProject(SourceLocation where) {
        auto path = file(manager_.getFileID(manager_.getExpansionLoc(where)));
        return path && !excluded(*path, source_);
    }

    std::string declarationLocation(SourceLocation where) {
        where = manager_.getExpansionLoc(where);
        auto path = file(manager_.getFileID(where));
        return pathText(path.value()) + ":" + std::to_string(manager_.getSpellingLineNumber(where)) + ":"
            + std::to_string(manager_.getSpellingColumnNumber(where));
    }

    std::string scopeName(const clang::DeclContext* scope) const {
        if (auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(scope)) {
            return tagName(*record);
        }
        if (auto* space = llvm::dyn_cast<clang::NamespaceDecl>(scope)) {
            return identity(scopeName(space->getParent()), space->getNameAsString());
        }
        return scope->isTranslationUnit() ? "" : scopeName(scope->getParent());
    }

    std::string specialization(const clang::TagDecl& declaration) const {
        const auto* node = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(&declaration);
        if (!node) {
            return {};
        }
        std::string text;
        llvm::raw_string_ostream output(text);
        node->getNameForDiagnostic(output, policy_, false);
        if (auto start = text.find('<'); start != std::string::npos) {
            return oneLine(text.substr(start));
        }
        return {};
    }

    std::string tagName(const clang::TagDecl& declaration) const {
        const auto* node = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(&declaration);
        if (node && node->getSpecializationKind() != clang::TSK_ExplicitSpecialization
            && !llvm::isa<clang::ClassTemplatePartialSpecializationDecl>(node)) {
            auto pattern = node->getSpecializedTemplateOrPartial();
            if (auto* partial = pattern.dyn_cast<clang::ClassTemplatePartialSpecializationDecl*>()) {
                return tagName(*partial);
            }
            return tagName(*node->getSpecializedTemplate()->getTemplatedDecl());
        }
        return identity(scopeName(declaration.getDeclContext()), declaration.getNameAsString())
               + specialization(declaration);
    }

    UmlType makeType(const clang::TagDecl& declaration, Kind kind) const {
        UmlType type;
        type.name = declaration.getNameAsString();
        auto* scope = declaration.getDeclContext();
        if (auto* owner = llvm::dyn_cast<clang::CXXRecordDecl>(scope)) {
            type.owner = tagName(*owner);
            do {
                scope = scope->getParent();
            } while (llvm::isa<clang::CXXRecordDecl>(scope));
        }
        type.nameSpace = scopeName(scope);
        type.kind = kind;
        type.access = access(declaration.getAccess());
        return type;
    }

    void expressionIncludes(const clang::Expr* expr, UmlType& owner) {
        auto include = [&](const clang::NamedDecl* declaration) {
            if (!inProject(declaration->getLocation())) {
                rememberInclude(*declaration, owner);
            }
        };
        std::vector<const clang::Stmt*> nodes{expr};
        while (!nodes.empty()) {
            const auto* node = nodes.back();
            nodes.pop_back();
            if (const auto* reference = llvm::dyn_cast<clang::DeclRefExpr>(node)) {
                include(reference->getDecl());
            } else if (const auto* lookup = llvm::dyn_cast<clang::UnresolvedLookupExpr>(node)) {
                for (const auto* declaration : lookup->decls()) {
                    include(declaration);
                }
            }
            for (const auto* child : node->children()) {
                if (child) {
                    nodes.push_back(child);
                }
            }
        }
    }

    std::string expression(const clang::Expr* expr, UmlType& owner) {
        if (!expr) {
            return {};
        }
        expressionIncludes(expr, owner);
        std::string text;
        llvm::raw_string_ostream output(text);
        expr->IgnoreImplicit()->printPretty(output, nullptr, policy_);
        return oneLine(std::move(text));
    }

    void addTemplates(const clang::TemplateParameterList& parameters, std::vector<std::string>& declarations,
                      UmlType& type) {
        for (const auto* parameter : parameters) {
            std::string text;
            llvm::raw_string_ostream output(text);
            parameter->print(output, policy_);
            declarations.push_back(oneLine(std::move(text)));
            if (const auto* value = llvm::dyn_cast<clang::NonTypeTemplateParmDecl>(parameter)) {
                typeSpelling(value->getType(), type, EdgeKind::Dependency);
            }
        }
        if (parameters.getRequiresClause()) {
            type.reverseLimitations.insert("requires clauses on template declarations");
        }
    }

    void rememberInclude(const clang::NamedDecl& declaration, UmlType& owner) {
        auto current = manager_.getFileID(manager_.getExpansionLoc(declaration.getLocation()));
        static const std::regex inclusion(R"regex(^\s*#\s*include\s*(<[^>]+>|"[^"]+"))regex");
        for (auto where = manager_.getIncludeLoc(current); where.isValid(); where = manager_.getIncludeLoc(current)) {
            current = manager_.getFileID(manager_.getExpansionLoc(where));
            auto path = file(current);
            if (!path || excluded(*path, source_)) {
                continue;
            }
            auto [buffer, offset] = manager_.getDecomposedExpansionLoc(where);
            auto text = manager_.getBufferData(buffer);
            auto start = text.take_front(offset).rfind('\n');
            auto end = text.find('\n', offset);
            auto line = text.slice(start == llvm::StringRef::npos ? 0 : start + 1, end).str();
            std::smatch parts;
            if (std::regex_search(line, parts, inclusion)) {
                owner.includes.insert(parts[1]);
            }
            break;
        }
    }

    void reference(const clang::TagDecl& target, UmlType& owner, EdgeKind kind) {
        if (target.getIdentifier() && inProject(target.getLocation())) {
            auto source = owner.qualifiedName();
            auto name = tagName(target);
            if (source != name || kind == EdgeKind::Association) {
                collected_.edges.insert({std::move(source), std::move(name), kind});
            }
        } else if (!inProject(target.getLocation())) {
            rememberInclude(target, owner);
        }
    }

    void memberPointer(const auto& member, UmlType& owner, EdgeKind kind, std::set<const clang::Type*>& seen) {
        if constexpr (requires { member.getClass(); }) {
            walkType(clang::QualType(member.getClass(), 0), owner, kind, seen);
        } else if (const auto* target = member.getQualifier().getAsRecordDecl()) {
            reference(*target, owner, kind);
        }
        walkType(member.getPointeeType(), owner, kind, seen);
    }

    void walkType(clang::QualType type, UmlType& owner, EdgeKind kind, std::set<const clang::Type*>& seen) {
        if (type.isNull() || !seen.insert(type.getTypePtr()).second) {
            return;
        }
        const auto* raw = type.getTypePtr();
        auto argument = [&](const auto& recurse, const clang::TemplateArgument& item) -> void {
            if (item.getKind() == clang::TemplateArgument::Type) {
                walkType(item.getAsType(), owner, kind, seen);
            } else if (item.getKind() == clang::TemplateArgument::Pack) {
                for (const auto& value : item.pack_elements()) {
                    recurse(recurse, value);
                }
            }
        };
        if (const auto* tag = llvm::dyn_cast<clang::TagType>(raw)) {
            auto* declaration = tag->getDecl();
            reference(*declaration, owner, kind);
            if (const auto* instance = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(declaration)) {
                if (kind != EdgeKind::Generalization) {
                    for (const auto& item : instance->getTemplateArgs().asArray()) {
                        argument(argument, item);
                    }
                }
            }
        } else if (const auto* specialization = llvm::dyn_cast<clang::TemplateSpecializationType>(raw)) {
            if (const auto* templated = specialization->getTemplateName().getAsTemplateDecl()) {
                if (const auto* record = llvm::dyn_cast<clang::TagDecl>(templated->getTemplatedDecl())) {
                    if (specialization->isDependentType()) {
                        reference(*record, owner, kind);
                    }
                } else if (!inProject(templated->getLocation())) {
                    rememberInclude(*templated, owner);
                }
            }
            if (kind != EdgeKind::Generalization) {
                for (const auto& item : specialization->template_arguments()) {
                    argument(argument, item);
                }
            }
        } else if (const auto* alias = llvm::dyn_cast<clang::TypedefType>(raw)) {
            if (!inProject(alias->getDecl()->getLocation())) {
                rememberInclude(*alias->getDecl(), owner);
            }
        } else if (const auto* array = llvm::dyn_cast<clang::ArrayType>(raw)) {
            walkType(array->getElementType(), owner, kind, seen);
        } else if (const auto* function = llvm::dyn_cast<clang::FunctionProtoType>(raw)) {
            walkType(function->getReturnType(), owner, kind, seen);
            for (auto parameter : function->getParamTypes()) {
                walkType(parameter, owner, kind, seen);
            }
        } else if (const auto* member = llvm::dyn_cast<clang::MemberPointerType>(raw)) {
            memberPointer(*member, owner, kind, seen);
        }
        walkType(raw->getPointeeType(), owner, kind, seen);
        auto unsugared = type.getSingleStepDesugaredType(context_);
        if (unsugared != type) {
            walkType(unsugared, owner, kind, seen);
        }
    }

    std::string typeSpelling(clang::QualType type, UmlType& owner, EdgeKind kind, bool declarator = false) {
        std::set<const clang::Type*> seen;
        walkType(type, owner, kind, seen);
        // Desugar a local typedef when it names a type directly. External aliases retain their headers.
        while (const auto* alias = type->getAs<clang::TypedefType>()) {
            if (!inProject(alias->getDecl()->getLocation())) {
                break;
            }
            type = alias->desugar().withCVRQualifiers(type.getCVRQualifiers());
        }
        auto qualified = clang::TypeName::getFullyQualifiedType(type, context_, true);
        std::string spelling = oneLine(qualified.getAsString(policy_));
        if (!declarator) {
            return spelling;
        }
        const std::string name = "__drawio_declarator__";
        std::string named;
        llvm::raw_string_ostream stream(named);
        qualified.print(stream, policy_, name);
        named = oneLine(std::move(named));
        if (named == spelling + " " + name) {
            return spelling;
        }
        if (auto position = named.find(name); position != std::string::npos) {
            named.replace(position, name.size(), "$");
        }
        return named;
    }

    std::vector<std::string> qualifiers(const clang::CXXMethodDecl& method, UmlType& owner) {
        std::vector<std::string> result;
        auto add = [&](bool condition, std::string_view modifier) {
            if (condition) {
                result.emplace_back(modifier);
            }
        };
        add(method.isStatic(), "static");
        add(method.isVirtual(), "virtual");
        add(method.isInlineSpecified(), "inline");
        bool writtenConstexpr = method.isConstexpr() && !method.isConsteval();
        if (writtenConstexpr && method.isDefaulted()) {
            // The same defaulted member can acquire implicit constexpr in a newer language mode.
            auto range = clang::CharSourceRange::getCharRange(method.getBeginLoc(), method.getNameInfo().getBeginLoc());
            auto prefix = clang::Lexer::getSourceText(range, manager_, context_.getLangOpts()).str();
            static const std::regex keyword(R"(\bconstexpr\b)");
            writtenConstexpr = std::regex_search(prefix, keyword);
        }
        add(writtenConstexpr, "constexpr");
        add(method.isConsteval(), "consteval");
        clang::ExplicitSpecifier explicitness;
        if (const auto* constructor = llvm::dyn_cast<clang::CXXConstructorDecl>(&method)) {
            explicitness = constructor->getExplicitSpecifier();
        } else if (const auto* conversion = llvm::dyn_cast<clang::CXXConversionDecl>(&method)) {
            explicitness = conversion->getExplicitSpecifier();
        }
        if (auto* condition = explicitness.getExpr()) {
            result.push_back("explicit(" + expression(condition, owner) + ")");
        } else {
            add(explicitness.isExplicit(), "explicit");
        }
        add(method.isConst(), "const");
        add(method.isVolatile(), "volatile");
        add(method.getRefQualifier() == clang::RQ_LValue, "&");
        add(method.getRefQualifier() == clang::RQ_RValue, "&&");
        if (method.getExceptionSpecSourceRange().isValid()) {
            const auto* prototype = method.getType()->castAs<clang::FunctionProtoType>();
            if (const auto* condition = prototype->getNoexceptExpr()) {
                result.push_back("noexcept(" + expression(condition, owner) + ")");
            } else if (clang::isNoexceptExceptionSpec(prototype->getExceptionSpecType())
                       || prototype->getExceptionSpecType() == clang::EST_DynamicNone) {
                result.emplace_back("noexcept");
            } else {
                owner.reverseLimitations.insert("legacy dynamic exception specifications");
            }
        }
        if (const auto* constraint = constraintExpression(method.getTrailingRequiresClause())) {
            result.push_back("requires " + expression(constraint, owner));
        }
        add(method.hasAttr<clang::OverrideAttr>(), "override");
        add(method.hasAttr<clang::FinalAttr>(), "final");
        add(method.isPureVirtual(), "pure");
        add(method.isDeletedAsWritten(), "delete");
        add(method.isExplicitlyDefaulted(), "default");
        if (!method.isExplicitlyDefaulted()) {
            if (const auto* definition = method.getDefinition(); definition && definition->isExplicitlyDefaulted()) {
                result.emplace_back("default-outside");
            }
        }
        return result;
    }

    std::optional<std::string> initializers(const clang::CXXConstructorDecl& declaration, UmlType& owner) {
        const auto* definition = llvm::dyn_cast_or_null<clang::CXXConstructorDecl>(declaration.getDefinition());
        if (!definition || definition->isDefaulted()) {
            return std::nullopt;
        }
        std::vector<std::string> written;
        for (const auto* item : definition->inits()) {
            if (!item->isWritten()) {
                continue;
            }
            std::string name = item->isAnyMemberInitializer()
                ? item->getAnyMember()->getNameAsString()
                : typeSpelling(item->getTypeSourceInfo()->getType(), owner, EdgeKind::Dependency);
            expressionIncludes(item->getInit(), owner);
            auto range = clang::CharSourceRange::getTokenRange(item->getLParenLoc(), item->getRParenLoc());
            auto operands = oneLine(clang::Lexer::getSourceText(range, manager_, context_.getLangOpts()).str());
            if (name.empty() || operands.empty()) {
                owner.reverseLimitations.insert("constructor initializer without reusable spelling");
                continue;
            }
            written.push_back(name + operands + (item->isPackExpansion() ? "..." : ""));
        }
        return join(written, ", ");
    }

    Operation operation(const clang::CXXMethodDecl& method, UmlType& owner,
                        const clang::TemplateParameterList* parameters) {
        Operation result;
        result.name = method.getNameAsString();
        result.access = access(method.getAccess());
        const auto* constructor = llvm::dyn_cast<clang::CXXConstructorDecl>(&method);
        if (constructor) {
            result.name = owner.name;
            result.initializers = initializers(*constructor, owner);
        } else if (llvm::isa<clang::CXXDestructorDecl>(method)) {
            result.name = "~" + owner.name;
        } else if (const auto* conversion = llvm::dyn_cast<clang::CXXConversionDecl>(&method)) {
            result.name = "operator " + typeSpelling(conversion->getConversionType(), owner, EdgeKind::Dependency);
        } else {
            result.result = typeSpelling(method.getReturnType(), owner, EdgeKind::Dependency);
            const auto* autoType = method.getReturnType()->getContainedAutoType();
            if (autoType && !autoType->isDeduced()) {
                owner.reverseLimitations.insert("return type deduced from a method body");
            }
        }
        result.modifiers = qualifiers(method, owner);
        if (parameters) {
            addTemplates(*parameters, result.templates, owner);
        }
        std::set<std::string> names;
        const auto* namedFrom = result.initializers ? method.getDefinition() : &method;
        for (const auto* argument : namedFrom->parameters()) {
            names.insert(argument->getNameAsString());
        }
        for (unsigned i = 0; i < method.getNumParams(); ++i) {
            const auto* argument = method.getParamDecl(i);
            std::string name = namedFrom->getParamDecl(i)->getNameAsString();
            if (name.empty()) {
                name = "arg" + std::to_string(i + 1);
                while (!names.insert(name).second) {
                    name += "_";
                }
            }
            std::string defaultValue;
            if (argument->hasDefaultArg() && !argument->hasUninstantiatedDefaultArg()) {
                defaultValue = expression(argument->getDefaultArg(), owner);
            }
            result.parameters.push_back({std::move(name),
                typeSpelling(argument->getOriginalType(), owner, EdgeKind::Dependency, true), std::move(defaultValue)});
        }
        if (method.isVariadic()) {
            result.parameters.push_back({"", "...", ""});
        }
        return result;
    }

    void members(const clang::CXXRecordDecl& declaration, UmlType& type) {
        for (const auto* member : declaration.decls()) {
            if (member->isImplicit()) {
                continue;
            }
            if (const auto* field = llvm::dyn_cast<clang::FieldDecl>(member)) {
                auto name = field->getNameAsString();
                if (name.empty() && !field->isBitField()) {
                    type.reverseLimitations.insert("anonymous fields");
                    continue;
                }
                Field modeled{access(field->getAccess()), std::move(name),
                              typeSpelling(field->getType(), type, EdgeKind::Association, true), {},
                              expression(field->getBitWidth(), type), expression(field->getInClassInitializer(), type)};
                if (field->isMutable()) {
                    modeled.modifiers.push_back("mutable");
                }
                type.fields.push_back(std::move(modeled));
            } else if (const auto* variable = llvm::dyn_cast<clang::VarDecl>(member)) {
                if (!variable->isStaticDataMember()) {
                    continue;
                }
                const auto* init = variable->getInit();
                Field modeled{access(variable->getAccess()), variable->getNameAsString(),
                              typeSpelling(variable->getType(), type, EdgeKind::Association, true), {"static"},
                              "", expression(init, type)};
                if (variable->isConstexpr()) {
                    modeled.modifiers.push_back("constexpr");
                } else if (variable->isInlineSpecified()) {
                    modeled.modifiers.push_back("inline");
                }
                if (variable->getTLSKind() != clang::VarDecl::TLS_None) {
                    modeled.modifiers.push_back("thread_local");
                }
                type.fields.push_back(std::move(modeled));
            } else if (const auto* methods = llvm::dyn_cast<clang::FunctionTemplateDecl>(member)) {
                if (const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(methods->getTemplatedDecl())) {
                    type.operations.push_back(operation(*method, type, methods->getTemplateParameters()));
                }
            } else if (const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(member)) {
                if (method->getDeclContext() == &declaration
                    && method->getTemplateSpecializationKind() == clang::TSK_Undeclared) {
                    type.operations.push_back(operation(*method, type, nullptr));
                }
            }
        }
    }

    void record(const clang::CXXRecordDecl& declaration) {
        if (!declaration.getIdentifier() || !declaration.isThisDeclarationADefinition()
            || declaration.isImplicit() || !inProject(declaration.getLocation())
            || !visited_.insert(&declaration).second) {
            return;
        }
        auto* partial = llvm::dyn_cast<clang::ClassTemplatePartialSpecializationDecl>(&declaration);
        auto* instance = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(&declaration);
        if (instance && !partial && instance->getSpecializationKind() != clang::TSK_ExplicitSpecialization) {
            return;
        }
        auto kind = declaration.isUnion() ? Kind::Union : declaration.isStruct() ? Kind::Struct : Kind::Class;
        auto type = makeType(declaration, kind);
        type.abstract = declaration.isAbstract();
        type.final = declaration.hasAttr<clang::FinalAttr>();
        type.specialization = specialization(declaration);
        if (auto* templated = declaration.getDescribedClassTemplate()) {
            addTemplates(*templated->getTemplateParameters(), type.templates, type);
        } else if (partial) {
            addTemplates(*partial->getTemplateParameters(), type.templates, type);
        }
        for (const auto& base : declaration.bases()) {
            std::string specification(accessName(access(base.getAccessSpecifier())));
            specification += base.isVirtual() ? " virtual " : " ";
            specification += typeSpelling(base.getType(), type, EdgeKind::Generalization);
            if (base.isPackExpansion()) {
                specification += "...";
            }
            type.bases.push_back(std::move(specification));
            if (const auto* specialization = base.getType()->getAs<clang::TemplateSpecializationType>()) {
                std::set<const clang::Type*> seen;
                for (const auto& argument : specialization->template_arguments()) {
                    if (argument.getKind() == clang::TemplateArgument::Type) {
                        walkType(argument.getAsType(), type, EdgeKind::Dependency, seen);
                    }
                }
            }
        }
        members(declaration, type);
        collected_.merge(std::move(type), declarationLocation(declaration.getLocation()));
        visit(declaration);
    }

    void enumeration(const clang::EnumDecl& declaration) {
        if (!declaration.getIdentifier() || !declaration.isThisDeclarationADefinition()
            || !inProject(declaration.getLocation())) {
            return;
        }
        auto type = makeType(declaration, declaration.isScoped() ? Kind::ScopedEnum : Kind::Enum);
        if (declaration.isFixed()) {
            type.underlying = typeSpelling(declaration.getIntegerType(), type, EdgeKind::Dependency);
        }
        for (const auto* item : declaration.enumerators()) {
            std::string value;
            if (const auto* init = item->getInitExpr()) {
                if (init->isValueDependent()) {
                    value = expression(init, type);
                } else {
                    llvm::SmallString<32> number;
                    item->getInitVal().toString(number, 10);
                    value = number.str().str();
                    if (item->getInitVal().isUnsigned() && item->getInitVal().getActiveBits() >= 63) {
                        value += "ULL";
                    }
                }
            }
            type.enumerators.push_back({item->getNameAsString(), std::move(value)});
        }
        collected_.merge(std::move(type), declarationLocation(declaration.getLocation()));
    }

    clang::ASTContext& context_;
    const clang::SourceManager& manager_;
    const fs::path& source_;
    Collected& collected_;
    clang::PrintingPolicy policy_;
    std::map<unsigned, std::optional<fs::path>> files_;
    std::set<const clang::CXXRecordDecl*> visited_;
};

class Consumer final : public clang::ASTConsumer {
public:
    Consumer(const fs::path& source, Collected& collected) : source_(source), collected_(collected) {}

    void HandleTranslationUnit(clang::ASTContext& context) override {
        if (!context.getDiagnostics().hasErrorOccurred() && context.getLangOpts().CPlusPlus) {
            AstModel(context, source_, collected_).visit(*context.getTranslationUnitDecl());
        }
    }

private:
    const fs::path& source_;
    Collected& collected_;
};

class Action final : public clang::ASTFrontendAction {
public:
    Action(const fs::path& source, Collected& collected) : source_(source), collected_(collected) {}

    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance& compiler,
                                                         llvm::StringRef input) override {
        if (!compiler.getLangOpts().CPlusPlus) {
            collected_.conflicts.insert(input.str() + ":1:1: error: Compilation command did not select C++.");
        }
        return std::make_unique<Consumer>(source_, collected_);
    }

private:
    const fs::path& source_;
    Collected& collected_;
};

class ActionFactory final : public ct::FrontendActionFactory {
public:
    ActionFactory(const fs::path& source, Collected& collected) : source_(source), collected_(collected) {}

    std::unique_ptr<clang::FrontendAction> create() override {
        return std::make_unique<Action>(source_, collected_);
    }

private:
    const fs::path& source_;
    Collected& collected_;
};

std::vector<std::string> analysisArguments(const std::vector<std::string>& original, const fs::path& resources) {
    auto command = original;
    // libTooling reads the flags without running the compiler executable. Do not load project plugins.
    for (std::size_t i = 1; i < command.size(); ++i) {
        std::string_view flag = command[i];
        if (flag == "-fplugin" || flag.starts_with("-fplugin=") || flag.starts_with("-fplugin-arg-")
            || (flag == "-Xclang" && i + 1 < command.size()
                && (command[i + 1] == "-load" || command[i + 1] == "-plugin" || command[i + 1] == "-add-plugin"
                    || command[i + 1].starts_with("-plugin-arg-")))) {
            throw AnalysisError("Clang plugins are unsupported in compilation commands.");
        }
    }
    if (std::ranges::none_of(command, [](std::string_view arg) {
        return arg == "-resource-dir" || arg.starts_with("-resource-dir=")
               || arg.starts_with("/clang:-resource-dir");
    })) {
        command.insert(command.begin() + 1, "-resource-dir=" + pathText(resources));
    }
    return command;
}

}

Analysis analyze(const fs::path& selected, const fs::path& compileDatabase, const fs::path& executable) {
    if (!fs::is_directory(selected)) {
        throw std::invalid_argument("Source directory does not exist or is not a directory: " + pathText(selected));
    }
    auto source = fs::canonical(selected);
    auto databaseFile = locateDatabase(source, compileDatabase);
    std::string databaseError;
    auto json = ct::JSONCompilationDatabase::loadFromFile(pathText(databaseFile), databaseError,
                                                         ct::JSONCommandLineSyntax::AutoDetect);
    if (!json) {
        throw std::invalid_argument(pathText(databaseFile) + ": " + databaseError);
    }
    auto database = ct::inferTargetAndDriverMode(ct::expandResponseFiles(std::move(json), llvm::vfs::getRealFileSystem()));
    auto commands = selectCommands(*database, source, databaseFile);
    if (commands.byFile.empty()) {
        throw std::invalid_argument("No C++ translation units in " + pathText(databaseFile)
                                    + " match " + pathText(source) + ".");
    }

    Analysis result;
    findMissingCommands(source, commands, result);
    std::vector<std::string> files;
    for (const auto& [file, entries] : commands.byFile) {
        files.push_back(file);
    }
    auto resources = executable.parent_path().parent_path() / "lib" / "clang" / DRAWIO_CLANG_RESOURCE_VERSION;
    if (!fs::is_directory(resources / "include")) {
        resources = pathFromUtf8(DRAWIO_CLANG_RESOURCES);
    }
    for (const auto& [file, entries] : commands.byFile) {
        for (const auto& entry : entries) {
            try {
                analysisArguments(entry.CommandLine, resources);
            } catch (const AnalysisError& issue) {
                throw AnalysisError(file + ":1:1: error: " + issue.what());
            }
        }
    }
    Diagnostics diagnostics;
    Collected collected;
    for (const auto& file : files) {
        std::size_t errorCount = diagnostics.errors.size();
        ct::ClangTool tool(commands, {file});
        tool.setPrintErrorMessage(false);
        tool.appendArgumentsAdjuster([&](const auto& flags, llvm::StringRef) {
            return analysisArguments(flags, resources);
        });
        tool.setDiagnosticConsumer(&diagnostics);
        ActionFactory factory(source, collected);
        int status = tool.run(&factory);
        if (status != 0 && diagnostics.errors.size() == errorCount) {
            diagnostics.errors.insert(file + ":1:1: error: Clang could not analyze this translation unit.");
        }
    }
    diagnostics.errors.insert(collected.conflicts.begin(), collected.conflicts.end());
    if (!diagnostics.errors.empty()) {
        throw AnalysisError(join(std::vector<std::string>(diagnostics.errors.begin(), diagnostics.errors.end()), "\n"));
    }
    result.fileCount = files.size();
    result.warnings.insert(result.warnings.end(), diagnostics.warnings.begin(), diagnostics.warnings.end());
    for (auto& [name, definition] : collected.definitions) {
        result.model.types.push_back(std::move(definition.type));
    }
    for (const auto& edge : collected.edges) {
        if (!collected.definitions.contains(edge.source) || !collected.definitions.contains(edge.target)) {
            continue;
        }
        if (edge.kind == EdgeKind::Dependency
            && (collected.edges.contains({edge.source, edge.target, EdgeKind::Association})
                || collected.edges.contains({edge.source, edge.target, EdgeKind::Generalization}))) {
            continue;
        }
        result.model.edges.push_back(edge);
    }
    if (result.model.types.empty()) {
        result.warnings.push_back("No named UML types found.");
    }
    return result;
}

}
