#include "cxx_dead/indexer.h"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/Mangle.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Index/USRGeneration.h>
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
#include <unordered_map>
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

std::string identity_path(const std::filesystem::path& path,
                          const std::filesystem::path& project_root) {
    if (path.empty())
        return {};
    if (path_is_within(path, project_root))
        return path.lexically_relative(project_root).generic_string();
    return path.lexically_normal().generic_string();
}

std::string fallback_identity_anchor(std::string_view qualified, std::string_view signature,
                                     const SymbolSource& source,
                                     const std::filesystem::path& project_root) {
    const auto& location =
        (source.expansion.has_value() ? *source.expansion : source.spelling).location;
    return std::string(qualified) + "|" + std::string(signature) + "|" +
           identity_path(location.file, project_root) + ":" + std::to_string(location.offset);
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
                             clang::ASTContext& context, Graph& graph,
                             std::vector<std::string>& diagnostics)
        : command_(command), options_(options), context_(context),
          source_manager_(context.getSourceManager()), graph_(graph),
          mangle_context_(context.createMangleContext()), diagnostics_(diagnostics) {}

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

        const auto linkage_name = mangled_name(*declaration);
        const bool namespace_static =
            method == nullptr && declaration->getStorageClass() == clang::SC_Static;
        const bool internal = namespace_static || linkage_name.starts_with("_ZL") ||
                              linkage_name.find("GLOBAL__N") != std::string::npos ||
                              qualified.find("(anonymous namespace)") != std::string::npos;
        const bool translation_unit_local =
            internal || lambda_call_operator || has_function_context(*declaration);
        const bool template_pattern =
            declaration->getDescribedFunctionTemplate() != nullptr &&
            declaration->getTemplateSpecializationKind() == clang::TSK_Undeclared;
        const auto declaration_signature = signature(*declaration);
        auto identity = make_symbol_identity(
            options_.configuration_id, usr(*declaration),
            template_pattern ? std::string{} : linkage_name,
            translation_unit_local ? identity_path(command_.file, options_.project_root)
                                   : std::string{},
            fallback_identity_anchor(qualified, declaration_signature, source,
                                     options_.project_root));
        const auto key = stable_symbol_key(identity);

        auto class_name = std::string{};
        if (method != nullptr) {
            if (method->getParent()->isLambda())
                class_name = lambda_owner_scope(*declaration);
            else
                class_name = method->getParent()->getQualifiedNameAsString();
        }
        const auto kind = symbol_kind(*declaration);
        const bool has_body =
            declaration->doesThisDeclarationHaveABody() || declaration->isExplicitlyDefaulted();
        const auto id = graph_.add_or_merge_symbol({
            .key = key,
            .identity = std::move(identity),
            .name = declaration->getNameAsString(),
            .qualified_name = qualified,
            .class_name = std::move(class_name),
            .signature = declaration_signature,
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
                 bool global_initializer, std::string reason = {}) {
        const auto target_id = add_declaration(target, true);
        if (!target_id.has_value())
            return;
        if (caller.has_value()) {
            if (reason.empty()) {
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
                case EdgeKind::CallbackRegistration:
                    reason = "configured callback registration";
                    break;
                case EdgeKind::Provider:
                    reason = "configured provider edge";
                    break;
                }
            }
            graph_.add_edge(*caller, *target_id, kind,
                            {.provider = "clang_ast", .reason = std::move(reason)});
        } else if (global_initializer) {
            graph_.add_root(*target_id, RootKind::GlobalInitializer,
                            {.provider = "clang_ast",
                             .reason = "use from a namespace-scope variable initializer"});
        }
    }

    void add_escape(std::optional<SymbolId> caller, const clang::FunctionDecl* target,
                    EscapeKind kind = EscapeKind::AddressTaken,
                    std::string reason = "function referenced outside a callee position") {
        const auto target_id = add_declaration(target, true);
        if (!target_id.has_value())
            return;
        graph_.add_escape(*target_id, kind, {.provider = "clang_ast", .reason = std::move(reason)},
                          caller);
    }

    [[nodiscard]] bool matches_rule(const clang::FunctionDecl* declaration,
                                    const CallbackRegistrationRule& rule) {
        const auto id = add_declaration(declaration, true);
        if (!id.has_value())
            return false;
        return matches(graph_.symbols()[*id], rule.callee);
    }

    void add_registration(std::optional<SymbolId> caller, const clang::FunctionDecl* target,
                          bool global_initializer, const CallbackRegistrationRule& rule) {
        const auto target_id = add_declaration(target, true);
        if (!target_id.has_value())
            return;
        auto evidence = rule.evidence;
        if (evidence.provider.empty())
            evidence.provider = "callback_registration";
        if (evidence.reason.empty()) {
            evidence.reason = "configured callback argument " +
                              std::to_string(rule.argument_index) + " of " + describe(rule.callee);
        }
        if (caller.has_value()) {
            graph_.add_edge(*caller, *target_id, EdgeKind::CallbackRegistration, evidence);
        } else if (global_initializer) {
            evidence.reason += " from a namespace-scope initializer";
            graph_.add_root(*target_id, RootKind::CallbackRegistration, std::move(evidence));
        }
    }

    void add_construction(std::optional<SymbolId> caller,
                          const clang::CXXConstructorDecl* constructor, bool global_initializer) {
        if (constructor == nullptr)
            return;
        add_use(caller, constructor, EdgeKind::Constructs, global_initializer);
        add_use(caller, constructor->getParent()->getDestructor(), EdgeKind::Constructs,
                global_initializer);
    }

    void add_factory_construction(std::optional<SymbolId> caller, const clang::CallExpr& call,
                                  const clang::FunctionDecl* target, bool global_initializer) {
        if (!call.isPRValue())
            return;
        const auto canonical = call.getType().getCanonicalType();
        const auto* record = canonical->getAsCXXRecordDecl();
        const auto* specialization =
            llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(record);
        if (specialization == nullptr)
            return;
        const auto pointer_name = specialization->getSpecializedTemplate()->getNameAsString();
        if (pointer_name != "unique_ptr" && pointer_name != "shared_ptr")
            return;
        const auto& arguments = specialization->getTemplateArgs();
        if (arguments.size() == 0 || arguments[0].getKind() != clang::TemplateArgument::Type)
            return;
        const auto element_type = arguments[0].getAsType().getCanonicalType();
        if (element_type->isArrayType())
            return;
        const auto* element = element_type->getAsCXXRecordDecl();
        if (element == nullptr)
            return;

        const auto helper = target == nullptr ? std::string{} : target->getNameAsString();
        if (helper.empty())
            return;
        const bool standard_factory = helper == "make_unique" || helper == "make_shared";
        const auto reason = standard_factory ? "standard smart-pointer factory construction"
                                             : "conservative owning-pointer factory construction";
        bool matched = false;
        for (const auto* constructor : element->ctors()) {
            const auto before = graph_.edges().size() + graph_.roots().size();
            add_use(caller, constructor, EdgeKind::Constructs, global_initializer, reason);
            matched = matched || graph_.edges().size() + graph_.roots().size() != before;
        }
        const auto before = graph_.edges().size() + graph_.roots().size();
        add_use(caller, element->getDestructor(), EdgeKind::Constructs, global_initializer, reason);
        matched = matched || graph_.edges().size() + graph_.roots().size() != before;
        if (matched && !standard_factory) {
            diagnostics_.push_back("unsupported owning-pointer factory " + helper +
                                   "; conservatively retained construction and destruction for " +
                                   element->getQualifiedNameAsString());
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

    static bool has_function_context(const clang::FunctionDecl& declaration) {
        for (auto* context = declaration.getDeclContext(); context != nullptr;
             context = context->getParent()) {
            if (context->isFunctionOrMethod())
                return true;
        }
        return false;
    }

    static std::string usr(const clang::FunctionDecl& declaration) {
        llvm::SmallString<128> storage;
        if (clang::index::generateUSRForDecl(declaration.getCanonicalDecl(), storage))
            return {};
        return std::string(storage);
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
    std::vector<std::string>& diagnostics_;
};

class UseVisitor : public clang::RecursiveASTVisitor<UseVisitor> {
  public:
    UseVisitor(TranslationUnitCollector& collector, std::vector<bool>& registration_rule_matches)
        : collector_(collector), registration_rule_matches_(registration_rule_matches) {}

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
        std::vector<const clang::FunctionDecl*> callees;
        if (const auto* target = expression->getDirectCallee()) {
            callees.push_back(target);
            collector_.add_use(caller_, target, EdgeKind::DirectCall, global_initializer_);
            collector_.add_factory_construction(caller_, *expression, target, global_initializer_);
        }
        for (const auto* target : callable_targets(expression->getCallee())) {
            if (std::ranges::find(callees, target) == callees.end()) {
                callees.push_back(target);
                collector_.add_use(caller_, target, EdgeKind::DirectCall, global_initializer_);
            }
        }
        for (unsigned argument = 0; argument < expression->getNumArgs(); ++argument) {
            for (const auto* target : callable_targets(expression->getArg(argument))) {
                collector_.add_escape(caller_, target, EscapeKind::CallableObject,
                                      "callable value passed outside a direct callee position");
            }
        }
        for (std::size_t rule_index = 0;
             rule_index < collector_.options().callback_registration_rules.size(); ++rule_index) {
            const auto& rule = collector_.options().callback_registration_rules[rule_index];
            if (!std::ranges::any_of(callees, [&](const clang::FunctionDecl* target) {
                    return collector_.matches_rule(target, rule);
                })) {
                continue;
            }
            registration_rule_matches_[rule_index] = true;
            if (rule.argument_index >= expression->getNumArgs()) {
                throw std::runtime_error("callback registration rule " + describe(rule.callee) +
                                         ":" + std::to_string(rule.argument_index) +
                                         " exceeds the registrar argument list");
            }
            const auto callbacks = callable_targets(expression->getArg(rule.argument_index));
            if (callbacks.empty()) {
                throw std::runtime_error("callback registration rule " + describe(rule.callee) +
                                         ":" + std::to_string(rule.argument_index) +
                                         " did not resolve a callable argument");
            }
            for (const auto* callback : callbacks)
                collector_.add_registration(caller_, callback, global_initializer_, rule);
        }
        if (const auto* callee = expression->getCallee())
            direct_callee_expressions_.insert(callee->IgnoreParenImpCasts());
        return true;
    }

    bool VisitCXXConstructExpr(clang::CXXConstructExpr* expression) {
        if (expression == nullptr)
            return true;
        collector_.add_construction(caller_, expression->getConstructor(), global_initializer_);
        const auto* record = expression->getType()->getAsCXXRecordDecl();
        if (record != nullptr && record->getQualifiedNameAsString() == "std::function") {
            for (unsigned argument = 0; argument < expression->getNumArgs(); ++argument) {
                for (const auto* target : callable_targets(expression->getArg(argument))) {
                    collector_.add_escape(caller_, target, EscapeKind::CallableObject,
                                          "callable value stored in std::function");
                }
            }
        }
        return true;
    }

    bool VisitBinaryOperator(clang::BinaryOperator* expression) {
        if (expression == nullptr || !expression->isAssignmentOp())
            return true;
        const auto* left = expression->getLHS()->IgnoreParenImpCasts();
        if (const auto* reference = llvm::dyn_cast<clang::DeclRefExpr>(left)) {
            if (const auto* variable = llvm::dyn_cast<clang::VarDecl>(reference->getDecl()))
                invalidated_variables_.insert(variable);
        }
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
    std::vector<const clang::FunctionDecl*> callable_targets(const clang::Expr* expression) {
        std::unordered_set<const clang::VarDecl*> visited_variables;
        std::vector<const clang::FunctionDecl*> result;
        collect_callable_targets(expression, visited_variables, result);
        return result;
    }

    void collect_callable_targets(const clang::Expr* expression,
                                  std::unordered_set<const clang::VarDecl*>& visited_variables,
                                  std::vector<const clang::FunctionDecl*>& output) {
        if (expression == nullptr)
            return;
        expression = expression->IgnoreParenImpCasts();
        const auto append = [&](const clang::FunctionDecl* target) {
            if (target != nullptr && std::ranges::find(output, target) == output.end())
                output.push_back(target);
        };
        if (const auto* lambda = llvm::dyn_cast<clang::LambdaExpr>(expression)) {
            append(lambda->getCallOperator());
            return;
        }
        if (const auto* reference = llvm::dyn_cast<clang::DeclRefExpr>(expression)) {
            if (const auto* target = llvm::dyn_cast<clang::FunctionDecl>(reference->getDecl())) {
                append(target);
                return;
            }
            if (const auto* variable = llvm::dyn_cast<clang::VarDecl>(reference->getDecl());
                variable != nullptr && variable->hasInit() &&
                !invalidated_variables_.contains(variable) &&
                visited_variables.insert(variable).second) {
                collect_callable_targets(variable->getInit(), visited_variables, output);
            }
        }
        if (const auto* member = llvm::dyn_cast<clang::MemberExpr>(expression)) {
            if (const auto* target = llvm::dyn_cast<clang::FunctionDecl>(member->getMemberDecl())) {
                append(target);
                return;
            }
        }
        if (const auto* record = expression->getType()->getAsCXXRecordDecl()) {
            if (record->getQualifiedNameAsString() != "std::function") {
                for (const auto* method : record->methods()) {
                    if (method->getOverloadedOperator() == clang::OO_Call)
                        append(method);
                }
            }
        }
        for (const auto* child : expression->children()) {
            if (const auto* child_expression = llvm::dyn_cast_or_null<clang::Expr>(child))
                collect_callable_targets(child_expression, visited_variables, output);
        }
    }

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
        caller_ = id;
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
    std::vector<bool>& registration_rule_matches_;
    std::optional<SymbolId> caller_;
    bool global_initializer_{false};
    std::unordered_set<const clang::FunctionDecl*> visited_functions_;
    std::unordered_set<const clang::Expr*> direct_callee_expressions_;
    std::unordered_set<const clang::VarDecl*> invalidated_variables_;
};

class FactConsumer : public clang::ASTConsumer {
  public:
    FactConsumer(const CompileCommand& command, const IndexOptions& options, Graph& graph,
                 std::vector<std::string>& diagnostics,
                 std::vector<bool>& registration_rule_matches)
        : command_(command), options_(options), graph_(graph), diagnostics_(diagnostics),
          registration_rule_matches_(registration_rule_matches) {}

    void HandleTranslationUnit(clang::ASTContext& context) override {
        TranslationUnitCollector collector(command_, options_, context, graph_, diagnostics_);
        UseVisitor uses(collector, registration_rule_matches_);
        uses.TraverseDecl(context.getTranslationUnitDecl());
    }

  private:
    const CompileCommand& command_;
    const IndexOptions& options_;
    Graph& graph_;
    std::vector<std::string>& diagnostics_;
    std::vector<bool>& registration_rule_matches_;
};

class FactAction : public clang::ASTFrontendAction {
  public:
    FactAction(const CompileCommand& command, const IndexOptions& options, Graph& graph,
               std::vector<std::string>& diagnostics, std::vector<bool>& registration_rule_matches)
        : command_(command), options_(options), graph_(graph), diagnostics_(diagnostics),
          registration_rule_matches_(registration_rule_matches) {}

    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance&,
                                                          llvm::StringRef) override {
        return std::make_unique<FactConsumer>(command_, options_, graph_, diagnostics_,
                                              registration_rule_matches_);
    }

  private:
    const CompileCommand& command_;
    const IndexOptions& options_;
    Graph& graph_;
    std::vector<std::string>& diagnostics_;
    std::vector<bool>& registration_rule_matches_;
};

class FactActionFactory : public clang::tooling::FrontendActionFactory {
  public:
    FactActionFactory(const CompileCommand& command, const IndexOptions& options, Graph& graph,
                      std::vector<std::string>& diagnostics,
                      std::vector<bool>& registration_rule_matches)
        : command_(command), options_(options), graph_(graph), diagnostics_(diagnostics),
          registration_rule_matches_(registration_rule_matches) {}

    std::unique_ptr<clang::FrontendAction> create() override {
        return std::make_unique<FactAction>(command_, options_, graph_, diagnostics_,
                                            registration_rule_matches_);
    }

  private:
    const CompileCommand& command_;
    const IndexOptions& options_;
    Graph& graph_;
    std::vector<std::string>& diagnostics_;
    std::vector<bool>& registration_rule_matches_;
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
            if (symbol.qualified_name == requested || symbol.identity.linkage_name == requested ||
                symbol.key == requested) {
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
    if (options_.configuration_id.empty())
        throw std::invalid_argument("configuration identity cannot be empty");
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
    std::vector<bool> registration_rule_matches(options_.callback_registration_rules.size(), false);
    if (options_.translation_unit_timeout.count() > 0 || options_.index_timeout.count() > 0 ||
        options_.max_ast_bytes != 0) {
        RunDiagnostics diagnostics{
            .state = RunState::Unsupported,
            .frontend = result.frontend,
            .partial_graph_discarded = false,
            .translation_units = {},
        };
        for (const auto& command : commands) {
            diagnostics.translation_units.push_back({
                .file = command.file,
                .status = TranslationUnitStatus::Unsupported,
                .stage = "configuration",
                .message = "hard resource limits require --frontend ast-json",
                .exit_code = std::nullopt,
                .signal = std::nullopt,
            });
        }
        throw IndexingError("hard resource limits require --frontend ast-json",
                            std::move(diagnostics));
    }
    for (std::size_t command_index = 0; command_index < commands.size(); ++command_index) {
        const auto& command = commands[command_index];
        if (options_.cancellation_requested && options_.cancellation_requested()) {
            RunDiagnostics diagnostics{
                .state = RunState::Incomplete,
                .frontend = result.frontend,
                .partial_graph_discarded = !result.translation_unit_diagnostics.empty(),
                .translation_units = result.translation_unit_diagnostics,
            };
            diagnostics.translation_units.push_back({
                .file = command.file,
                .status = TranslationUnitStatus::Cancelled,
                .stage = "indexing",
                .message = "indexing cancelled before translation-unit processing",
                .exit_code = std::nullopt,
                .signal = std::nullopt,
            });
            for (std::size_t index = command_index + 1U; index < commands.size(); ++index) {
                diagnostics.translation_units.push_back({
                    .file = commands[index].file,
                    .status = TranslationUnitStatus::Skipped,
                    .stage = "indexing",
                    .message = "not indexed because an earlier translation unit did not complete",
                    .exit_code = std::nullopt,
                    .signal = std::nullopt,
                });
            }
            throw IndexingError("LibTooling indexing cancelled", std::move(diagnostics));
        }
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
        FactActionFactory factory(command, options_, translation_unit_facts, result.diagnostics,
                                  registration_rule_matches);
        const int exit_code = tool.run(&factory);
        if (exit_code != 0) {
            const auto message = "Clang LibTooling indexing failed for " + command.file.string() +
                                 " (exit " + std::to_string(exit_code) + ")";
            RunDiagnostics diagnostics{
                .state = RunState::Incomplete,
                .frontend = result.frontend,
                .partial_graph_discarded = !result.translation_unit_diagnostics.empty(),
                .translation_units = result.translation_unit_diagnostics,
            };
            diagnostics.translation_units.push_back({
                .file = command.file,
                .status = TranslationUnitStatus::Failed,
                .stage = "clang",
                .message = message,
                .exit_code = exit_code,
                .signal = std::nullopt,
            });
            for (std::size_t index = command_index + 1U; index < commands.size(); ++index) {
                diagnostics.translation_units.push_back({
                    .file = commands[index].file,
                    .status = TranslationUnitStatus::Skipped,
                    .stage = "indexing",
                    .message = "not indexed because an earlier translation unit did not complete",
                    .exit_code = std::nullopt,
                    .signal = std::nullopt,
                });
            }
            throw IndexingError(message, std::move(diagnostics));
        }
        try {
            result.fact_bytes += graph_fact_bytes(translation_unit_facts);
            merge_graph(result.graph, translation_unit_facts);
        } catch (const std::exception& error) {
            const auto message = "could not merge LibTooling facts for " + command.file.string() +
                                 ": " + error.what();
            RunDiagnostics diagnostics{
                .state = RunState::Incomplete,
                .frontend = result.frontend,
                .partial_graph_discarded = true,
                .translation_units = result.translation_unit_diagnostics,
            };
            diagnostics.translation_units.push_back({
                .file = command.file,
                .status = TranslationUnitStatus::Failed,
                .stage = "fact_merge",
                .message = message,
                .exit_code = std::nullopt,
                .signal = std::nullopt,
            });
            for (std::size_t index = command_index + 1U; index < commands.size(); ++index) {
                diagnostics.translation_units.push_back({
                    .file = commands[index].file,
                    .status = TranslationUnitStatus::Skipped,
                    .stage = "indexing",
                    .message = "not indexed because an earlier translation unit did not complete",
                    .exit_code = std::nullopt,
                    .signal = std::nullopt,
                });
            }
            throw IndexingError(message, std::move(diagnostics));
        }
        ++result.translation_units;
        result.translation_unit_diagnostics.push_back({
            .file = command.file,
            .status = TranslationUnitStatus::Indexed,
            .stage = "indexing",
            .message = "translation unit indexed successfully",
            .exit_code = std::nullopt,
            .signal = std::nullopt,
        });
    }
    for (std::size_t index = 0; index < registration_rule_matches.size(); ++index) {
        if (!registration_rule_matches[index]) {
            const auto& rule = options_.callback_registration_rules[index];
            throw IndexingError("callback registration rule did not match a registrar call: " +
                                    describe(rule.callee) + ":" +
                                    std::to_string(rule.argument_index),
                                {.state = RunState::Incomplete,
                                 .frontend = result.frontend,
                                 .partial_graph_discarded = true,
                                 .translation_units = result.translation_unit_diagnostics});
        }
    }
    add_manual_roots(result, options_);
    try {
        apply_provider_policies(result.graph, options_.provider_policies);
    } catch (const std::exception& error) {
        throw IndexingError("could not apply reachability provider: " + std::string(error.what()),
                            {.state = RunState::Incomplete,
                             .frontend = result.frontend,
                             .partial_graph_discarded = true,
                             .translation_units = result.translation_unit_diagnostics});
    }
    if (!options_.ast_filter.empty())
        result.diagnostics.push_back("Clang AST name filter active: " + options_.ast_filter);
    if (result.graph.roots().empty()) {
        throw IndexingError(
            "no application roots found; include main(), --root, or a provider root",
            {.state = RunState::Incomplete,
             .frontend = result.frontend,
             .partial_graph_discarded = true,
             .translation_units = result.translation_unit_diagnostics});
    }
    for (const auto& symbol : result.graph.symbols()) {
        if (symbol.identity.quality == IdentityQuality::Fallback) {
            result.diagnostics.push_back("fallback symbol identity for " + symbol.qualified_name +
                                         " : " + symbol.signature + " at " +
                                         symbol.identity.fallback_anchor +
                                         "; Clang could not generate a linkage name or USR");
        }
    }
    try {
        result.graph.canonicalize();
    } catch (const std::exception& error) {
        throw IndexingError("could not finalize LibTooling facts: " + std::string(error.what()),
                            {.state = RunState::Incomplete,
                             .frontend = result.frontend,
                             .partial_graph_discarded = true,
                             .translation_units = result.translation_unit_diagnostics});
    }
    std::ranges::sort(result.translation_unit_diagnostics, {}, &TranslationUnitDiagnostic::file);
    std::ranges::sort(result.diagnostics);
    const auto duplicate = std::ranges::unique(result.diagnostics);
    result.diagnostics.erase(duplicate.begin(), duplicate.end());
    return result;
}

bool libtooling_available() {
    return true;
}

} // namespace cxx_dead
