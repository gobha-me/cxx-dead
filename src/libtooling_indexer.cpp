#include "cxx_dead/indexer.h"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/Mangle.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Lex/Lexer.h>
#include <clang/Tooling/ArgumentsAdjusters.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cxx_dead {

namespace {

std::filesystem::path absolute_normalized(std::filesystem::path path,
                                          const std::filesystem::path& base) {
    if (path.empty())
        return {};
    if (path.is_relative())
        path = base / path;
    return std::filesystem::absolute(path).lexically_normal();
}

bool path_is_within(const std::filesystem::path& child, const std::filesystem::path& parent) {
    if (child.empty() || parent.empty())
        return false;
    const auto relative = child.lexically_relative(parent);
    if (relative.empty())
        return child == parent;
    const auto first = *relative.begin();
    return first != ".." && !relative.is_absolute();
}

SymbolScope symbol_scope(const IndexOptions& options, const std::filesystem::path& file) {
    if (std::ranges::any_of(options.excluded_paths,
                            [&](const auto& excluded) { return path_is_within(file, excluded); })) {
        return SymbolScope::Excluded;
    }
    if (!path_is_within(file, options.project_root))
        return SymbolScope::ExternalOpaque;
    if (std::ranges::any_of(options.report_paths, [&](const auto& report_path) {
            return path_is_within(file, report_path);
        })) {
        return SymbolScope::Reportable;
    }
    return SymbolScope::Indexed;
}

SymbolKind symbol_kind(const clang::FunctionDecl& declaration) {
    if (llvm::isa<clang::CXXConstructorDecl>(declaration))
        return SymbolKind::Constructor;
    if (llvm::isa<clang::CXXDestructorDecl>(declaration))
        return SymbolKind::Destructor;
    if (llvm::isa<clang::CXXMethodDecl>(declaration))
        return SymbolKind::Method;
    return SymbolKind::Function;
}

class TranslationUnitCollector {
  public:
    TranslationUnitCollector(const CompileCommand& command, const IndexOptions& options,
                             clang::ASTContext& context, Graph& graph)
        : command_(command), options_(options), context_(context),
          source_manager_(context.getSourceManager()), graph_(graph),
          mangle_context_(context.createMangleContext()) {}

    std::optional<SymbolId> add_declaration(const clang::FunctionDecl* declaration,
                                            bool referenced = false) {
        const auto* method = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(declaration);
        const bool lambda_call_operator = method != nullptr && method->getParent()->isLambda() &&
                                          method->getOverloadedOperator() == clang::OO_Call;
        if (declaration == nullptr || (declaration->isImplicit() && !lambda_call_operator))
            return std::nullopt;
        const auto qualified = qualified_name(*declaration);
        if (!options_.ast_filter.empty() &&
            qualified.find(options_.ast_filter) == std::string::npos) {
            return std::nullopt;
        }
        const auto source = symbol_source(*declaration);
        const auto file =
            (source.expansion.has_value() ? *source.expansion : source.spelling).location.file;
        const auto scope = symbol_scope(options_, file);
        if (scope == SymbolScope::Excluded)
            return std::nullopt;
        if (referenced && !options_.ast_filter.empty() && scope == SymbolScope::ExternalOpaque) {
            return std::nullopt;
        }
        if (!referenced && scope == SymbolScope::ExternalOpaque)
            return std::nullopt;

        auto key = mangled_name(*declaration);
        const bool namespace_static =
            method == nullptr && declaration->getStorageClass() == clang::SC_Static;
        const bool internal = namespace_static || key.starts_with("_ZL") ||
                              key.find("GLOBAL__N") != std::string::npos ||
                              qualified.find("(anonymous namespace)") != std::string::npos;
        if (key.empty())
            key = qualified + "|" + signature(*declaration);
        if (internal)
            key += "@" + command_.file.string();

        auto class_name = std::string{};
        if (method != nullptr) {
            if (method->getParent()->isLambda())
                class_name = lambda_owner_scope(*declaration);
            else
                class_name = method->getParent()->getQualifiedNameAsString();
        }
        const auto kind = symbol_kind(*declaration);
        const bool template_pattern =
            declaration->getDescribedFunctionTemplate() != nullptr &&
            declaration->getTemplateSpecializationKind() == clang::TSK_Undeclared;
        const bool has_body =
            declaration->doesThisDeclarationHaveABody() || declaration->isExplicitlyDefaulted();
        const auto id = graph_.add_or_merge_symbol({
            .key = std::move(key),
            .name = declaration->getNameAsString(),
            .qualified_name = qualified,
            .class_name = std::move(class_name),
            .signature = signature(*declaration),
            .source = source,
            .kind = kind,
            .scope = scope,
            .defined = has_indexed_body(scope) && has_body && !template_pattern,
            .internal_linkage = internal,
            .is_virtual = llvm::isa<clang::CXXMethodDecl>(declaration) &&
                          llvm::cast<clang::CXXMethodDecl>(declaration)->isVirtual(),
        });
        if (declaration->getNameAsString() == "main" && graph_.symbols()[id].defined) {
            graph_.add_root(
                id, RootKind::ApplicationEntryPoint,
                {.provider = "application_policy",
                 .reason = "defined function named main is an application entry point"});
        }
        return id;
    }

    void add_virtual_edges(const clang::CXXMethodDecl& declaration) {
        if (symbol_kind(declaration) != SymbolKind::Method)
            return;
        const auto derived = add_declaration(&declaration);
        if (!derived.has_value())
            return;
        for (const auto* overridden : declaration.overridden_methods()) {
            const auto base = add_declaration(overridden, true);
            if (base.has_value()) {
                graph_.add_edge(*base, *derived, EdgeKind::VirtualDispatch,
                                {.provider = "class_hierarchy",
                                 .reason = "known override of a virtual method"});
            }
        }
    }

    void add_use(std::optional<SymbolId> caller, const clang::FunctionDecl* target, EdgeKind kind,
                 bool global_initializer) {
        const auto target_id = add_declaration(target, true);
        if (!target_id.has_value())
            return;
        if (caller.has_value()) {
            std::string reason;
            switch (kind) {
            case EdgeKind::DirectCall:
                reason = "direct call expression";
                break;
            case EdgeKind::Constructs:
                reason = "constructor or destructor use";
                break;
            case EdgeKind::VirtualDispatch:
                reason = "virtual dispatch";
                break;
            }
            graph_.add_edge(*caller, *target_id, kind,
                            {.provider = "clang_ast", .reason = std::move(reason)});
        } else if (global_initializer) {
            graph_.add_root(*target_id, RootKind::GlobalInitializer,
                            {.provider = "clang_ast",
                             .reason = "use from a namespace-scope variable initializer"});
        }
    }

    void add_escape(std::optional<SymbolId> caller, const clang::FunctionDecl* target) {
        const auto target_id = add_declaration(target, true);
        if (!target_id.has_value())
            return;
        graph_.add_escape(
            *target_id, EscapeKind::AddressTaken,
            {.provider = "clang_ast", .reason = "function referenced outside a callee position"},
            caller);
    }

    void add_construction(std::optional<SymbolId> caller,
                          const clang::CXXConstructorDecl* constructor, bool global_initializer) {
        if (constructor == nullptr)
            return;
        std::vector<const clang::CXXRecordDecl*> pending{constructor->getParent()};
        std::unordered_set<const clang::CXXRecordDecl*> visited;
        while (!pending.empty()) {
            const auto* record = pending.back();
            pending.pop_back();
            if (record == nullptr || !visited.insert(record->getCanonicalDecl()).second)
                continue;
            for (const auto* candidate : record->ctors())
                add_use(caller, candidate, EdgeKind::Constructs, global_initializer);
            add_use(caller, record->getDestructor(), EdgeKind::Constructs, global_initializer);
            for (const auto& base : record->bases()) {
                if (const auto* base_record = base.getType()->getAsCXXRecordDecl())
                    pending.push_back(base_record);
            }
        }
    }

    [[nodiscard]] const IndexOptions& options() const {
        return options_;
    }

    [[nodiscard]] bool matches_filter(const clang::NamedDecl& declaration) const {
        return options_.ast_filter.empty() || declaration.getQualifiedNameAsString().find(
                                                  options_.ast_filter) != std::string::npos;
    }

    [[nodiscard]] SymbolScope declaration_scope(const clang::FunctionDecl& declaration) {
        const auto source = symbol_source(declaration);
        return symbol_scope(
            options_,
            (source.expansion.has_value() ? *source.expansion : source.spelling).location.file);
    }

  private:
    SourcePoint source_point(clang::SourceLocation location, bool spelling) const {
        if (location.isInvalid())
            return {};
        const auto resolved = spelling ? source_manager_.getSpellingLoc(location)
                                       : source_manager_.getExpansionLoc(location);
        if (resolved.isInvalid() || !resolved.isFileID())
            return {};
        const auto file_id = source_manager_.getFileID(resolved);
        const auto file = source_manager_.getFileEntryRefForID(file_id);
        if (!file.has_value())
            return {};
        const auto file_path = absolute_normalized(file->getName().str(), command_.directory);
        return {
            .file = file_path,
            .line = source_manager_.getSpellingLineNumber(resolved),
            .column = source_manager_.getSpellingColumnNumber(resolved),
            .offset = source_manager_.getFileOffset(resolved),
            .token_length =
                clang::Lexer::MeasureTokenLength(resolved, source_manager_, context_.getLangOpts()),
        };
    }

    SourceExtent source_extent(const clang::FunctionDecl& declaration, bool spelling) const {
        const auto range = declaration.getSourceRange();
        SourceExtent extent{
            .location = source_point(declaration.getLocation(), spelling),
            .begin = source_point(range.getBegin(), spelling),
            .end = source_point(range.getEnd(), spelling),
        };
        if (extent.location.file.empty())
            extent.location = extent.begin;
        if (extent.begin.file.empty())
            extent.begin = extent.location;
        if (extent.end.file.empty())
            extent.end = extent.begin;
        return extent;
    }

    SymbolSource symbol_source(const clang::FunctionDecl& declaration) const {
        SymbolSource result{.spelling = source_extent(declaration, true),
                            .expansion = std::nullopt};
        const auto location = declaration.getLocation();
        const auto range = declaration.getSourceRange();
        if (location.isMacroID() || range.getBegin().isMacroID() || range.getEnd().isMacroID())
            result.expansion = source_extent(declaration, false);
        return result;
    }

    static std::string lambda_owner_scope(const clang::FunctionDecl& declaration) {
        std::vector<std::string> components;
        for (const auto* context = declaration.getDeclContext(); context != nullptr;) {
            const auto* context_declaration = clang::Decl::castFromDeclContext(context);
            if (const auto* namespace_declaration =
                    llvm::dyn_cast<clang::NamespaceDecl>(context_declaration)) {
                components.push_back(namespace_declaration->isAnonymousNamespace()
                                         ? "(anonymous namespace)"
                                         : namespace_declaration->getNameAsString());
            } else if (const auto* record =
                           llvm::dyn_cast<clang::CXXRecordDecl>(context_declaration);
                       record != nullptr && !record->isLambda() && !record->getName().empty()) {
                components.push_back(record->getNameAsString());
            }
            if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(context_declaration))
                context = function->getLexicalDeclContext();
            else
                context = context->getParent();
        }
        std::ranges::reverse(components);
        std::string result;
        for (const auto& component : components) {
            if (component.empty())
                continue;
            if (!result.empty())
                result += "::";
            result += component;
        }
        return result;
    }

    static std::string qualified_name(const clang::FunctionDecl& declaration) {
        if (const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(&declaration);
            method != nullptr && method->getParent()->isLambda()) {
            const auto owner = lambda_owner_scope(declaration);
            return owner.empty() ? declaration.getNameAsString()
                                 : owner + "::" + declaration.getNameAsString();
        }
        return declaration.getQualifiedNameAsString();
    }

    std::string signature(const clang::FunctionDecl& declaration) const {
        clang::PrintingPolicy policy(context_.getLangOpts());
        return declaration.getType().getAsString(policy);
    }

    std::string mangled_name(const clang::FunctionDecl& declaration) const {
        if (declaration.getNameAsString() == "main" || declaration.isExternC())
            return declaration.getNameAsString();
        llvm::SmallString<128> storage;
        llvm::raw_svector_ostream output(storage);
        if (const auto* constructor = llvm::dyn_cast<clang::CXXConstructorDecl>(&declaration)) {
            mangle_context_->mangleName(clang::GlobalDecl(constructor, clang::Ctor_Complete),
                                        output);
        } else if (const auto* destructor =
                       llvm::dyn_cast<clang::CXXDestructorDecl>(&declaration)) {
            mangle_context_->mangleName(clang::GlobalDecl(destructor, clang::Dtor_Complete),
                                        output);
        } else if (mangle_context_->shouldMangleDeclName(&declaration)) {
            mangle_context_->mangleName(&declaration, output);
        }
        return std::string(storage);
    }

    const CompileCommand& command_;
    const IndexOptions& options_;
    clang::ASTContext& context_;
    clang::SourceManager& source_manager_;
    Graph& graph_;
    std::unique_ptr<clang::MangleContext> mangle_context_;
};

class UseVisitor : public clang::RecursiveASTVisitor<UseVisitor> {
  public:
    explicit UseVisitor(TranslationUnitCollector& collector) : collector_(collector) {}

    bool shouldVisitTemplateInstantiations() const {
        return true;
    }

    bool shouldVisitImplicitCode() const {
        return true;
    }

    bool TraverseFunctionDecl(clang::FunctionDecl* declaration) {
        return traverse_callable(declaration, [&] {
            return clang::RecursiveASTVisitor<UseVisitor>::TraverseFunctionDecl(declaration);
        });
    }

    bool TraverseCXXMethodDecl(clang::CXXMethodDecl* declaration) {
        return traverse_callable(declaration, [&] {
            return clang::RecursiveASTVisitor<UseVisitor>::TraverseCXXMethodDecl(declaration);
        });
    }

    bool TraverseCXXConstructorDecl(clang::CXXConstructorDecl* declaration) {
        return traverse_callable(declaration, [&] {
            return clang::RecursiveASTVisitor<UseVisitor>::TraverseCXXConstructorDecl(declaration);
        });
    }

    bool TraverseCXXDestructorDecl(clang::CXXDestructorDecl* declaration) {
        return traverse_callable(declaration, [&] {
            return clang::RecursiveASTVisitor<UseVisitor>::TraverseCXXDestructorDecl(declaration);
        });
    }

    bool TraverseCXXConversionDecl(clang::CXXConversionDecl* declaration) {
        return traverse_callable(declaration, [&] {
            return clang::RecursiveASTVisitor<UseVisitor>::TraverseCXXConversionDecl(declaration);
        });
    }

    bool TraverseVarDecl(clang::VarDecl* declaration) {
        if (declaration != nullptr && declaration->isFileVarDecl() && declaration->hasInit() &&
            declaration->getDeclContext()->isTranslationUnit()) {
            if (!collector_.matches_filter(*declaration))
                return true;
            const auto previous_caller = caller_;
            const bool previous_initializer = global_initializer_;
            caller_.reset();
            global_initializer_ = true;
            const bool result = TraverseStmt(declaration->getInit());
            caller_ = previous_caller;
            global_initializer_ = previous_initializer;
            return result;
        }
        return clang::RecursiveASTVisitor<UseVisitor>::TraverseVarDecl(declaration);
    }

    bool VisitCallExpr(clang::CallExpr* expression) {
        if (expression == nullptr)
            return true;
        if (const auto* target = expression->getDirectCallee())
            collector_.add_use(caller_, target, EdgeKind::DirectCall, global_initializer_);
        if (const auto* callee = expression->getCallee())
            direct_callee_expressions_.insert(callee->IgnoreParenImpCasts());
        return true;
    }

    bool VisitCXXConstructExpr(clang::CXXConstructExpr* expression) {
        if (expression == nullptr)
            return true;
        collector_.add_construction(caller_, expression->getConstructor(), global_initializer_);
        return true;
    }

    bool VisitDeclRefExpr(clang::DeclRefExpr* expression) {
        if (expression == nullptr || direct_callee_expressions_.contains(expression))
            return true;
        if (const auto* target = llvm::dyn_cast<clang::FunctionDecl>(expression->getDecl()))
            collector_.add_escape(caller_, target);
        return true;
    }

    bool VisitMemberExpr(clang::MemberExpr* expression) {
        if (expression == nullptr || direct_callee_expressions_.contains(expression))
            return true;
        if (const auto* target = llvm::dyn_cast<clang::FunctionDecl>(expression->getMemberDecl()))
            collector_.add_escape(caller_, target);
        return true;
    }

  private:
    template <typename Declaration, typename Traverse>
    bool traverse_callable(Declaration* declaration, Traverse traverse) {
        if (declaration == nullptr)
            return true;
        const auto id = collector_.add_declaration(declaration);
        if (const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(declaration))
            collector_.add_virtual_edges(*method);
        if (declaration->getBody() == nullptr)
            return true;
        if (!visited_functions_.insert(declaration).second)
            return true;
        if (!id.has_value() || !has_indexed_body(collector_.declaration_scope(*declaration)))
            return true;
        if (declaration->getTemplateSpecializationKind() == clang::TSK_ImplicitInstantiation)
            return true;
        const auto previous = caller_;
        const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(declaration);
        const bool nested_lambda =
            method != nullptr && method->getParent()->isLambda() && previous.has_value();
        caller_ = nested_lambda ? previous : id;
        if (auto* constructor = llvm::dyn_cast<clang::CXXConstructorDecl>(declaration)) {
            for (const auto* initializer : constructor->inits()) {
                if (initializer != nullptr)
                    static_cast<void>(TraverseStmt(initializer->getInit()));
            }
        }
        const bool result = traverse();
        caller_ = previous;
        return result;
    }
    TranslationUnitCollector& collector_;
    std::optional<SymbolId> caller_;
    bool global_initializer_{false};
    std::unordered_set<const clang::FunctionDecl*> visited_functions_;
    std::unordered_set<const clang::Expr*> direct_callee_expressions_;
};

class FactConsumer : public clang::ASTConsumer {
  public:
    FactConsumer(const CompileCommand& command, const IndexOptions& options, Graph& graph)
        : command_(command), options_(options), graph_(graph) {}

    void HandleTranslationUnit(clang::ASTContext& context) override {
        TranslationUnitCollector collector(command_, options_, context, graph_);
        UseVisitor uses(collector);
        uses.TraverseDecl(context.getTranslationUnitDecl());
    }

  private:
    const CompileCommand& command_;
    const IndexOptions& options_;
    Graph& graph_;
};

class FactAction : public clang::ASTFrontendAction {
  public:
    FactAction(const CompileCommand& command, const IndexOptions& options, Graph& graph)
        : command_(command), options_(options), graph_(graph) {}

    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance&,
                                                          llvm::StringRef) override {
        return std::make_unique<FactConsumer>(command_, options_, graph_);
    }

  private:
    const CompileCommand& command_;
    const IndexOptions& options_;
    Graph& graph_;
};

class FactActionFactory : public clang::tooling::FrontendActionFactory {
  public:
    FactActionFactory(const CompileCommand& command, const IndexOptions& options, Graph& graph)
        : command_(command), options_(options), graph_(graph) {}

    std::unique_ptr<clang::FrontendAction> create() override {
        return std::make_unique<FactAction>(command_, options_, graph_);
    }

  private:
    const CompileCommand& command_;
    const IndexOptions& options_;
    Graph& graph_;
};

class SingleCommandDatabase : public clang::tooling::CompilationDatabase {
  public:
    explicit SingleCommandDatabase(const CompileCommand& command)
        : command_{command.directory.string(), command.file.string(), command.arguments, ""} {}

    std::vector<clang::tooling::CompileCommand> getCompileCommands(llvm::StringRef) const override {
        return {command_};
    }

  private:
    clang::tooling::CompileCommand command_;
};

void add_manual_roots(IndexResult& result, const IndexOptions& options) {
    for (const auto& requested : options.manual_roots) {
        bool matched = false;
        for (SymbolId id = 0; id < result.graph.symbols().size(); ++id) {
            const auto& symbol = result.graph.symbols()[id];
            if (symbol.qualified_name == requested || symbol.key == requested) {
                result.graph.add_root(id, RootKind::Manual,
                                      {.provider = "command_line",
                                       .reason = "matched configured root: " + requested});
                matched = true;
            }
        }
        if (!matched)
            result.diagnostics.push_back("configured root did not match a symbol: " + requested);
    }
}

} // namespace

LibToolingIndexer::LibToolingIndexer(IndexOptions options) : options_(std::move(options)) {
    options_.project_root = std::filesystem::absolute(options_.project_root).lexically_normal();
    if (options_.report_paths.empty())
        options_.report_paths.push_back(options_.project_root);
    for (auto& report_path : options_.report_paths) {
        report_path = std::filesystem::absolute(report_path).lexically_normal();
        if (!path_is_within(report_path, options_.project_root)) {
            throw std::invalid_argument("report path is outside the project root: " +
                                        report_path.string());
        }
    }
    for (auto& excluded : options_.excluded_paths)
        excluded = std::filesystem::absolute(excluded).lexically_normal();
}

IndexResult LibToolingIndexer::index(const std::vector<CompileCommand>& commands) const {
    if (commands.empty())
        throw std::runtime_error("compilation database contains no commands");
    IndexResult result;
    result.frontend = IndexFrontend::LibTooling;
    for (const auto& command : commands) {
        Graph translation_unit_facts;
        SingleCommandDatabase database(command);
        clang::tooling::ClangTool tool(database, {command.file.string()});
        tool.appendArgumentsAdjuster(clang::tooling::getClangStripOutputAdjuster());
        tool.appendArgumentsAdjuster(clang::tooling::getClangStripDependencyFileAdjuster());
        tool.appendArgumentsAdjuster(clang::tooling::getClangSyntaxOnlyAdjuster());
        tool.appendArgumentsAdjuster(
            [](const clang::tooling::CommandLineArguments& arguments, llvm::StringRef) {
                auto adjusted = arguments;
                adjusted.emplace_back("-Wno-error");
                adjusted.emplace_back("-Wno-unknown-warning-option");
                return adjusted;
            });
        FactActionFactory factory(command, options_, translation_unit_facts);
        const int exit_code = tool.run(&factory);
        if (exit_code != 0) {
            throw std::runtime_error("Clang LibTooling indexing failed for " +
                                     command.file.string() + " (exit " + std::to_string(exit_code) +
                                     ")");
        }
        result.fact_bytes += graph_fact_bytes(translation_unit_facts);
        merge_graph(result.graph, translation_unit_facts);
        ++result.translation_units;
    }
    add_manual_roots(result, options_);
    if (!options_.ast_filter.empty())
        result.diagnostics.push_back("Clang AST name filter active: " + options_.ast_filter);
    result.graph.sort_roots();
    if (result.graph.roots().empty()) {
        throw std::runtime_error(
            "no application roots found; include main() or provide a matching --root symbol");
    }
    return result;
}

bool libtooling_available() {
    return true;
}

} // namespace cxx_dead
