#pragma once
#include <iostream>
#include <limits.h>
#include "Luau/Frontend.h"
#include "Luau/Autocomplete.h"
#include "Luau/BuiltinDefinitions.h"
#include "Luau/ToString.h"
#include "Luau/AstQuery.h"
#include "Luau/TypeInfer.h"
#include "Luau/Transpiler.h"
#include "glob/glob.hpp"
#include "Client.hpp"
#include "Protocol.hpp"
#include "Sourcemap.hpp"
#include "TextDocument.hpp"
#include "DocumentationParser.hpp"
#include "WorkspaceFileResolver.hpp"
#include "LuauExt.hpp"
#include "Utils.hpp"

static std::optional<Luau::AutocompleteEntryMap> nullCallback(std::string tag, std::optional<const Luau::ClassTypeVar*> ptr)
{
    return std::nullopt;
}

// Get the corresponding Luau module name for a file
Luau::ModuleName getModuleName(const std::string& name)
{
    return name;
}
Luau::ModuleName getModuleName(const std::filesystem::path& name)
{
    return name.generic_string();
}
Luau::ModuleName getModuleName(const Uri& name)
{
    return name.fsPath().generic_string();
}

Luau::Position convertPosition(const lsp::Position& position)
{
    LUAU_ASSERT(position.line <= UINT_MAX);
    LUAU_ASSERT(position.character <= UINT_MAX);
    return Luau::Position{static_cast<unsigned int>(position.line), static_cast<unsigned int>(position.character)};
}

lsp::Position convertPosition(const Luau::Position& position)
{
    return lsp::Position{static_cast<size_t>(position.line), static_cast<size_t>(position.column)};
}

class WorkspaceFolder
{
public:
    std::shared_ptr<Client> client;
    std::string name;
    lsp::DocumentUri rootUri;
    WorkspaceFileResolver fileResolver;
    Luau::Frontend frontend;

public:
    WorkspaceFolder(std::shared_ptr<Client> client, const std::string& name, const lsp::DocumentUri& uri)
        : client(client)
        , name(name)
        , rootUri(uri)
        , fileResolver(WorkspaceFileResolver())
        , frontend(Luau::Frontend(&fileResolver, &fileResolver, {true}))
    {
        fileResolver.rootUri = uri;
        setup();
    }

    /// Checks whether a provided file is part of the workspace
    bool isInWorkspace(const lsp::DocumentUri& file)
    {
        // Check if the root uri is a prefix of the file
        auto prefixStr = rootUri.toString();
        auto checkStr = file.toString();
        if (checkStr.compare(0, prefixStr.size(), prefixStr) == 0)
        {
            return true;
        }
        return false;
    }

    void openTextDocument(const lsp::DocumentUri& uri, const lsp::DidOpenTextDocumentParams& params)
    {
        auto moduleName = getModuleName(uri);
        fileResolver.managedFiles.emplace(
            std::make_pair(moduleName, TextDocument(uri, params.textDocument.languageId, params.textDocument.version, params.textDocument.text)));
        // Mark the file as dirty as we don't know what changes were made to it
        frontend.markDirty(moduleName);
    }

    void updateTextDocument(const lsp::DocumentUri& uri, const lsp::DidChangeTextDocumentParams& params)
    {
        auto moduleName = getModuleName(uri);

        if (fileResolver.managedFiles.find(moduleName) == fileResolver.managedFiles.end())
        {
            std::cerr << "Text Document not loaded locally: " << uri.toString() << std::endl;
            return;
        }
        auto& textDocument = fileResolver.managedFiles.at(moduleName);
        textDocument.update(params.contentChanges, params.textDocument.version);

        // Mark the module dirty for the typechecker
        frontend.markDirty(moduleName);
    }

    void closeTextDocument(const lsp::DocumentUri& uri)
    {
        auto moduleName = getModuleName(uri);
        fileResolver.managedFiles.erase(moduleName);
    }

    // lsp::PublishDiagnosticsParams publishDiagnostics(const lsp::DocumentUri& uri, std::optional<int> version)
    // {
    //     auto moduleName = getModuleName(uri);
    //     auto diagnostics = findDiagnostics(moduleName);
    //     return {uri, version, diagnostics};
    // }

private:
    lsp::Diagnostic createTypeErrorDiagnostic(const Luau::TypeError& error)
    {
        std::string message;
        if (const Luau::SyntaxError* syntaxError = Luau::get_if<Luau::SyntaxError>(&error.data))
            message = "SyntaxError: " + syntaxError->message;
        else
            message = "TypeError: " + Luau::toString(error);

        lsp::Diagnostic diagnostic;
        diagnostic.source = "Luau";
        diagnostic.code = error.code();
        diagnostic.message = message;
        diagnostic.severity = lsp::DiagnosticSeverity::Error;
        diagnostic.range = {convertPosition(error.location.begin), convertPosition(error.location.end)};
        return diagnostic;
    }

    lsp::Diagnostic createLintDiagnostic(const Luau::LintWarning& lint)
    {
        lsp::Diagnostic diagnostic;
        diagnostic.source = "Luau";
        diagnostic.code = lint.code;
        diagnostic.message = std::string(Luau::LintWarning::getName(lint.code)) + ": " + lint.text;
        diagnostic.severity = lsp::DiagnosticSeverity::Warning; // Configuration can convert this to an error
        diagnostic.range = {convertPosition(lint.location.begin), convertPosition(lint.location.end)};
        return diagnostic;
    }

public:
    /// Whether the file has been marked as ignored by any of the ignored lists in the configuration
    bool isIgnoredFile(const std::filesystem::path& path, const std::optional<ClientConfiguration>& givenConfig = std::nullopt)
    {
        // We want to test globs against a relative path to workspace, since thats what makes most sense
        auto relativePath = path.lexically_relative(rootUri.fsPath()).generic_string(); // HACK: we convert to generic string so we get '/' separators

        auto config = givenConfig ? *givenConfig : client->getConfiguration(rootUri);
        std::vector<std::string> patterns = config.ignoreGlobs; // TODO: extend further?
        for (auto& pattern : patterns)
        {
            if (glob::fnmatch_case(relativePath, pattern))
            {
                return true;
            }
        }
        return false;
    }

    lsp::DocumentDiagnosticReport documentDiagnostics(const lsp::DocumentDiagnosticParams& params)
    {
        // TODO: should we apply a resultId and return an unchanged report if unchanged?
        lsp::DocumentDiagnosticReport report;
        std::unordered_map<std::string /* lsp::DocumentUri */, std::vector<lsp::Diagnostic>> relatedDiagnostics;

        auto moduleName = getModuleName(params.textDocument.uri);
        Luau::CheckResult cr;
        if (frontend.isDirty(moduleName))
            cr = frontend.check(moduleName);

        // If there was an error retrieving the source module, bail early with this diagnostic
        if (!frontend.getSourceModule(moduleName))
        {
            lsp::Diagnostic errorDiagnostic;
            errorDiagnostic.source = "Luau";
            errorDiagnostic.code = "000";
            errorDiagnostic.message = "Failed to resolve source module for this file";
            errorDiagnostic.severity = lsp::DiagnosticSeverity::Error;
            errorDiagnostic.range = {{0, 0}, {0, 0}};
            report.items.emplace_back(errorDiagnostic);
            return report;
        }

        auto config = client->getConfiguration(rootUri);

        // Report Type Errors
        // Note that type errors can extend to related modules in the require graph - so we report related information here
        for (auto& error : cr.errors)
        {
            auto diagnostic = createTypeErrorDiagnostic(error);
            if (error.moduleName == moduleName)
            {
                report.items.emplace_back(diagnostic);
            }
            else
            {
                auto fileName = fileResolver.resolveVirtualPathToRealPath(error.moduleName);
                if (!fileName || isIgnoredFile(*fileName, config))
                    continue;
                auto uri = Uri::file(*fileName);
                auto& currentDiagnostics = relatedDiagnostics[uri.toString()];
                currentDiagnostics.emplace_back(diagnostic);
            }
        }

        // Convert the related diagnostics map into an equivalent report
        if (!relatedDiagnostics.empty())
        {
            for (auto& [uri, diagnostics] : relatedDiagnostics)
            {
                // TODO: resultId?
                lsp::SingleDocumentDiagnosticReport subReport{lsp::DocumentDiagnosticReportKind::Full, std::nullopt, diagnostics};
                report.relatedDocuments.emplace(uri, subReport);
            }
        }

        // Report Lint Warnings
        // Lints only apply to the current file
        Luau::LintResult lr = frontend.lint(moduleName);
        for (auto& error : lr.errors)
        {
            auto diagnostic = createLintDiagnostic(error);
            diagnostic.severity = lsp::DiagnosticSeverity::Error; // Report this as an error instead
            report.items.emplace_back(diagnostic);
        }
        for (auto& error : lr.warnings)
            report.items.emplace_back(createLintDiagnostic(error));

        return report;
    }

    std::vector<lsp::CompletionItem> completion(const lsp::CompletionParams& params)
    {
        auto result = Luau::autocomplete(frontend, getModuleName(params.textDocument.uri), convertPosition(params.position), nullCallback);
        std::vector<lsp::CompletionItem> items;

        for (auto& [name, entry] : result.entryMap)
        {
            lsp::CompletionItem item;
            item.label = name;
            item.deprecated = entry.deprecated;

            if (entry.documentationSymbol)
                item.documentation = {lsp::MarkupKind::Markdown, printDocumentation(client->documentation, *entry.documentationSymbol)};

            switch (entry.kind)
            {
            case Luau::AutocompleteEntryKind::Property:
                item.kind = lsp::CompletionItemKind::Field;
                break;
            case Luau::AutocompleteEntryKind::Binding:
                item.kind = lsp::CompletionItemKind::Variable;
                break;
            case Luau::AutocompleteEntryKind::Keyword:
                item.kind = lsp::CompletionItemKind::Keyword;
                break;
            case Luau::AutocompleteEntryKind::String:
                item.kind = lsp::CompletionItemKind::Constant; // TODO: is a string autocomplete always a singleton constant?
                break;
            case Luau::AutocompleteEntryKind::Type:
                item.kind = lsp::CompletionItemKind::Interface;
                break;
            case Luau::AutocompleteEntryKind::Module:
                item.kind = lsp::CompletionItemKind::Module;
                break;
            }

            // Handle parentheses suggestions
            if (entry.parens == Luau::ParenthesesRecommendation::CursorAfter)
            {
                item.insertText = name + "()$0";
                item.insertTextFormat = lsp::InsertTextFormat::Snippet;
            }
            else if (entry.parens == Luau::ParenthesesRecommendation::CursorInside)
            {
                item.insertText = name + "($1)$0";
                item.insertTextFormat = lsp::InsertTextFormat::Snippet;
                // Trigger Signature Help
                item.command = lsp::Command{"Trigger Signature Help", "editor.action.triggerParameterHints"};
            }

            if (entry.type.has_value())
            {
                auto id = Luau::follow(entry.type.value());
                // Try to infer more type info about the entry to provide better suggestion info
                if (Luau::get<Luau::FunctionTypeVar>(id))
                {
                    item.kind = lsp::CompletionItemKind::Function;
                }
                else if (auto ttv = Luau::get<Luau::TableTypeVar>(id))
                {
                    // Special case the RBXScriptSignal type as a connection
                    if (ttv->name && ttv->name.value() == "RBXScriptSignal")
                    {
                        item.kind = lsp::CompletionItemKind::Event;
                    }
                }
                else if (Luau::get<Luau::ClassTypeVar>(id))
                {
                    item.kind = lsp::CompletionItemKind::Class;
                }
                item.detail = Luau::toString(id);
            }

            items.emplace_back(item);
        }

        return items;
    }

    std::vector<lsp::DocumentLink> documentLink(const lsp::DocumentLinkParams& params)
    {
        auto moduleName = getModuleName(params.textDocument.uri);
        std::vector<lsp::DocumentLink> result;

        // We need to parse the code, which is currently only done in the type checker
        frontend.check(moduleName);

        auto sourceModule = frontend.getSourceModule(moduleName);
        if (!sourceModule || !sourceModule->root)
            return {};

        // Only resolve document links on require(Foo.Bar.Baz) code
        // TODO: Curerntly we only link at the top level block, not nested blocks
        for (auto stat : sourceModule->root->body)
        {
            if (auto local = stat->as<Luau::AstStatLocal>())
            {
                if (local->values.size == 0)
                    continue;

                for (size_t i = 0; i < local->values.size; i++)
                {
                    const Luau::AstExprCall* call = local->values.data[i]->as<Luau::AstExprCall>();
                    if (!call)
                        continue;

                    if (auto maybeRequire = types::matchRequire(*call))
                    {
                        if (auto moduleInfo = frontend.moduleResolver.resolveModuleInfo(moduleName, **maybeRequire))
                        {
                            // Resolve the module info to a URI
                            std::optional<std::filesystem::path> realName = moduleInfo->name;
                            if (fileResolver.isVirtualPath(moduleInfo->name))
                                realName = fileResolver.resolveVirtualPathToRealPath(moduleInfo->name);

                            if (realName)
                            {
                                lsp::DocumentLink link;
                                link.target = Uri::file(*realName);
                                link.range = lsp::Range{{call->argLocation.begin.line, call->argLocation.begin.column},
                                    {call->argLocation.end.line, call->argLocation.end.column - 1}};
                                result.push_back(link);
                            }
                        }
                    }
                }
            }
        }

        return result;
    }

    std::optional<lsp::Hover> hover(const lsp::HoverParams& params)
    {
        auto moduleName = getModuleName(params.textDocument.uri);
        auto position = convertPosition(params.position);

        // Run the type checker to ensure we are up to date
        frontend.check(moduleName);

        auto sourceModule = frontend.getSourceModule(moduleName);
        if (!sourceModule)
            return std::nullopt;

        auto module = frontend.moduleResolver.getModule(moduleName);
        auto exprOrLocal = Luau::findExprOrLocalAtPosition(*sourceModule, position);

        std::optional<Luau::TypeId> type = std::nullopt;

        if (auto expr = exprOrLocal.getExpr())
        {
            if (auto it = module->astTypes.find(expr))
                type = *it;
            else if (auto index = expr->as<Luau::AstExprIndexName>())
            {
                if (auto parentIt = module->astTypes.find(index->expr))
                {
                    auto parentType = Luau::follow(*parentIt);
                    auto indexName = index->index.value;
                    if (auto ctv = Luau::get<Luau::ClassTypeVar>(parentType))
                    {
                        if (auto prop = Luau::lookupClassProp(ctv, indexName))
                        {
                            type = prop->type;
                        }
                    }
                    else if (auto tbl = Luau::get<Luau::TableTypeVar>(parentType))
                    {
                        if (tbl->props.find(indexName) != tbl->props.end())
                        {
                            type = tbl->props.at(indexName).type;
                        }
                    }
                    else if (auto mt = Luau::get<Luau::MetatableTypeVar>(parentType))
                    {
                        if (auto tbl = Luau::get<Luau::TableTypeVar>(mt->table))
                        {
                            if (tbl->props.find(indexName) != tbl->props.end())
                            {
                                type = tbl->props.at(indexName).type;
                            }
                        }
                    }
                    // else if (auto i = get<Luau::IntersectionTypeVar>(parentType))
                    // {
                    //     for (Luau::TypeId ty : i->parts)
                    //     {
                    //         // TODO: find the corresponding ty
                    //     }
                    // }
                    // else if (auto u = get<Luau::UnionTypeVar>(parentType))
                    // {
                    //     // Find the corresponding ty
                    // }
                }
            }
        }
        else if (auto local = exprOrLocal.getLocal())
        {
            auto scope = Luau::findScopeAtPosition(*module, position);
            if (!scope)
                return std::nullopt;
            type = scope->lookup(local);
        }

        if (!type)
            return std::nullopt;
        type = Luau::follow(*type);

        Luau::ToStringOptions opts;
        opts.exhaustive = true;
        opts.useLineBreaks = true;
        opts.functionTypeArguments = true;
        opts.hideNamedFunctionTypeParameters = false;
        opts.indent = true;
        std::string typeString = Luau::toString(*type, opts);

        // If we have a function and its corresponding name
        if (auto ftv = Luau::get<Luau::FunctionTypeVar>(*type))
        {
            types::NameOrExpr name = exprOrLocal.getExpr();
            if (auto localName = exprOrLocal.getName())
            {
                name = localName->value;
            }
            typeString = codeBlock("lua", types::toStringNamedFunction(module, ftv, name));
        }
        else if (exprOrLocal.getLocal() || exprOrLocal.getExpr()->as<Luau::AstExprLocal>())
        {
            std::string builder = "local ";
            builder += exprOrLocal.getName()->value;
            builder += ": " + typeString;
            typeString = codeBlock("lua", builder);
        }
        else if (auto global = exprOrLocal.getExpr()->as<Luau::AstExprGlobal>())
        {
            // TODO: should we indicate this is a global somehow?
            std::string builder = "type ";
            builder += global->name.value;
            builder += " = " + typeString;
            typeString = codeBlock("lua", builder);
        }
        else
        {
            typeString = codeBlock("lua", typeString);
        }

        if (auto symbol = type.value()->documentationSymbol)
        {
            typeString += "\n----------\n";
            typeString += printDocumentation(client->documentation, *symbol);
        }

        return lsp::Hover{{lsp::MarkupKind::Markdown, typeString}};
    }

    std::optional<lsp::SignatureHelp> signatureHelp(const lsp::SignatureHelpParams& params)
    {
        auto moduleName = getModuleName(params.textDocument.uri);
        auto position = convertPosition(params.position);

        // Run the type checker to ensure we are up to date
        frontend.check(moduleName);

        auto sourceModule = frontend.getSourceModule(moduleName);
        if (!sourceModule)
            return std::nullopt;

        auto module = frontend.moduleResolver.getModule(moduleName);
        auto ancestry = Luau::findAstAncestryOfPosition(*sourceModule, position);

        if (ancestry.size() == 0)
            return std::nullopt;

        Luau::AstExprCall* candidate = ancestry.back()->as<Luau::AstExprCall>();
        if (!candidate && ancestry.size() >= 2)
            candidate = ancestry.at(ancestry.size() - 2)->as<Luau::AstExprCall>();

        if (!candidate)
            return std::nullopt;

        size_t activeParameter = candidate->args.size == 0 ? 0 : candidate->args.size - 1;

        auto it = module->astTypes.find(candidate->func);
        if (!it)
            return std::nullopt;
        auto followedId = Luau::follow(*it);

        std::vector<lsp::SignatureInformation> signatures;

        auto addSignature = [&](const Luau::FunctionTypeVar* ftv)
        {
            Luau::ToStringOptions opts;
            opts.functionTypeArguments = true;
            opts.hideNamedFunctionTypeParameters = false;
            opts.hideFunctionSelfArgument = candidate->self; // If self has been provided, then hide the self argument

            // Create the whole label
            std::string label = types::toStringNamedFunction(module, ftv, candidate->func);
            lsp::MarkupContent documentation{lsp::MarkupKind::PlainText, ""};

            if (followedId->documentationSymbol)
                documentation = {lsp::MarkupKind::Markdown, printDocumentation(client->documentation, *followedId->documentationSymbol)};

            // Create each parameter label
            std::vector<lsp::ParameterInformation> parameters;
            auto it = Luau::begin(ftv->argTypes);
            size_t idx = 0;

            while (it != Luau::end(ftv->argTypes))
            {
                // If the function has self, and the caller has called as a method (i.e., :), then omit the self parameter
                if (idx == 0 && ftv->hasSelf && candidate->self)
                {
                    it++;
                    idx++;
                    continue;
                }

                std::string label;
                lsp::MarkupContent parameterDocumentation{lsp::MarkupKind::PlainText, ""};
                if (idx < ftv->argNames.size() && ftv->argNames[idx])
                {
                    label = ftv->argNames[idx]->name + ": ";
                }
                label += Luau::toString(*it);

                parameters.push_back(lsp::ParameterInformation{label, parameterDocumentation});
                it++;
                idx++;
            }

            signatures.push_back(lsp::SignatureInformation{label, documentation, parameters});
        };

        if (auto ftv = Luau::get<Luau::FunctionTypeVar>(followedId))
        {
            // Single function
            addSignature(ftv);
        }

        // Handle overloaded function
        if (auto intersect = Luau::get<Luau::IntersectionTypeVar>(followedId))
        {
            for (Luau::TypeId part : intersect->parts)
            {
                if (auto candidateFunctionType = Luau::get<Luau::FunctionTypeVar>(part))
                {
                    addSignature(candidateFunctionType);
                }
            }
        }

        return lsp::SignatureHelp{signatures, 0, activeParameter};
    }

    std::optional<lsp::Location> gotoDefinition(const lsp::DefinitionParams& params)
    {
        auto moduleName = getModuleName(params.textDocument.uri);
        auto position = convertPosition(params.position);

        // Run the type checker to ensure we are up to date
        if (frontend.isDirty(moduleName))
            frontend.check(moduleName);

        auto sourceModule = frontend.getSourceModule(moduleName);
        auto module = frontend.moduleResolver.getModule(moduleName);
        if (!sourceModule || !module)
            return std::nullopt;

        auto binding = Luau::findBindingAtPosition(*module, *sourceModule, position);
        if (!binding)
            return std::nullopt;

        // TODO: we should get further definitions from other modules (e.g., if the binding was from a `local X = require(...)`, we want more info
        // about X from its required file)
        // TODO: we should extend further if we don't find a binding (i.e., a index function call etc)

        return lsp::Location{params.textDocument.uri, lsp::Range{convertPosition(binding->location.begin), convertPosition(binding->location.end)}};
    }

    std::optional<lsp::Location> gotoTypeDefinition(const lsp::TypeDefinitionParams& params)
    {
        // If its a binding, we should find its assigned type if possible, and then find the definition of that type
        // If its a type, then just find the definintion of that type (i.e. the type alias)

        auto moduleName = getModuleName(params.textDocument.uri);
        auto position = convertPosition(params.position);

        // Run the type checker to ensure we are up to date
        if (frontend.isDirty(moduleName))
            frontend.check(moduleName);

        auto sourceModule = frontend.getSourceModule(moduleName);
        auto module = frontend.moduleResolver.getModule(moduleName);
        if (!sourceModule || !module)
            return std::nullopt;

        auto node = Luau::findNodeAtPosition(*sourceModule, position);
        if (!node)
            return std::nullopt;

        auto findTypeLocation = [&module, &position, &params](Luau::AstType* type) -> std::optional<lsp::Location>
        {
            // TODO: should we only handle references here? what if its an actual type
            if (auto reference = type->as<Luau::AstTypeReference>())
            {
                // TODO: handle if imported from a module (i.e., reference.prefix)
                auto scope = Luau::findScopeAtPosition(*module, position);
                if (!scope)
                    return std::nullopt;
                if (scope->typeAliasLocations.find(reference->name.value) == scope->typeAliasLocations.end())
                    return std::nullopt;
                auto location = scope->typeAliasLocations.at(reference->name.value);
                return lsp::Location{params.textDocument.uri, lsp::Range{convertPosition(location.begin), convertPosition(location.end)}};
            }
            return std::nullopt;
        };

        if (auto type = node->asType())
        {
            return findTypeLocation(type);
        }
        else
        {
            // ExprOrLocal gives us better information
            auto exprOrLocal = Luau::findExprOrLocalAtPosition(*sourceModule, position);
            if (auto local = exprOrLocal.getLocal())
            {
                if (auto annotation = local->annotation)
                {
                    return findTypeLocation(annotation);
                }
            }
            // TODO: handle expressions
            // AstExprTypeAssertion
            // AstStatTypeAlias
        }

        return std::nullopt;
    }

    std::optional<std::vector<lsp::DocumentSymbol>> documentSymbol(const lsp::DocumentSymbolParams& params)
    {
        auto moduleName = getModuleName(params.textDocument.uri);

        // Run the type checker to ensure we are up to date
        if (frontend.isDirty(moduleName))
            frontend.check(moduleName);

        auto module = frontend.moduleResolver.getModule(moduleName);
        if (!module)
            return std::nullopt;

        std::vector<lsp::DocumentSymbol> result;

        // TODO

        return result;
    }

    bool updateSourceMap()
    {
        // Read in the sourcemap
        // TODO: We should invoke the rojo process dynamically if possible here, so that we can also refresh the sourcemap when we notice files are
        // changed
        // TODO: we assume a sourcemap.json file in the workspace root
        if (auto sourceMapContents = readFile(rootUri.fsPath() / "sourcemap.json"))
        {
            fileResolver.updateSourceMap(sourceMapContents.value());
            return true;
        }
        else
        {
            return false;
        }
    }

private:
    void registerExtendedTypes(Luau::TypeChecker& typeChecker, const std::filesystem::path& definitionsFile)
    {
        if (auto definitions = readFile(definitionsFile))
        {
            auto loadResult = Luau::loadDefinitionFile(typeChecker, typeChecker.globalScope, *definitions, "@roblox");
            if (!loadResult.success)
            {
                client->sendWindowMessage(lsp::MessageType::Error, "Syntax error when reading definitions file. Extended types will not be provided");
                return;
            }

            // Extend globally registered types with Instance information
            if (fileResolver.rootSourceNode)
            {
                if (fileResolver.rootSourceNode->className == "DataModel")
                {
                    for (const auto& service : fileResolver.rootSourceNode->children)
                    {
                        auto serviceName = service->className; // We know it must be a service of the same class name
                        if (auto serviceType = typeChecker.globalScope->lookupType(serviceName))
                        {
                            if (Luau::ClassTypeVar* ctv = Luau::getMutable<Luau::ClassTypeVar>(serviceType->type))
                            {
                                // Extend the props to include the children
                                for (const auto& child : service->children)
                                {
                                    ctv->props[child->name] = Luau::makeProperty(types::makeLazyInstanceType(
                                        typeChecker.globalTypes, typeChecker.globalScope, child, serviceType->type, fileResolver));
                                }
                            }
                        }
                    }
                }

                // Prepare module scope so that we can dynamically reassign the type of "script" to retrieve instance info
                typeChecker.prepareModuleScope = [this](const Luau::ModuleName& name, const Luau::ScopePtr& scope)
                {
                    if (auto node = fileResolver.isVirtualPath(name) ? fileResolver.getSourceNodeFromVirtualPath(name)
                                                                     : fileResolver.getSourceNodeFromRealPath(name))
                    {
                        // HACK: we need a way to get the typeArena for the module, but I don't know how
                        // we can see that moduleScope->returnType is assigned before prepareModuleScope is called in TypeInfer, so we could try it
                        // this way...
                        LUAU_ASSERT(scope->returnType);
                        auto typeArena = scope->returnType->owningArena;
                        LUAU_ASSERT(typeArena);

                        scope->bindings[Luau::AstName("script")] =
                            Luau::Binding{types::makeLazyInstanceType(*typeArena, scope, node.value(), std::nullopt, fileResolver), Luau::Location{},
                                {}, {}, std::nullopt};
                    }
                };
            }
        }
        else
        {
            client->sendWindowMessage(lsp::MessageType::Error, "Unable to read the definitions file. Extended types will not be provided");
        }

        if (auto instanceType = typeChecker.globalScope->lookupType("Instance"))
        {
            if (auto* ctv = Luau::getMutable<Luau::ClassTypeVar>(instanceType->type))
            {
                Luau::attachMagicFunction(ctv->props["IsA"].type, types::magicFunctionInstanceIsA);
                Luau::attachMagicFunction(ctv->props["FindFirstChildWhichIsA"].type, types::magicFunctionFindFirstXWhichIsA);
                Luau::attachMagicFunction(ctv->props["FindFirstChildOfClass"].type, types::magicFunctionFindFirstXWhichIsA);
                Luau::attachMagicFunction(ctv->props["FindFirstAncestorWhichIsA"].type, types::magicFunctionFindFirstXWhichIsA);
                Luau::attachMagicFunction(ctv->props["FindFirstAncestorOfClass"].type, types::magicFunctionFindFirstXWhichIsA);
                Luau::attachMagicFunction(ctv->props["Clone"].type, types::magicFunctionInstanceClone);
            }
        }
    }

    bool isNullWorkspace() const
    {
        return name == "$NULL_WORKSPACE";
    }

    void setup()
    {
        if (!isNullWorkspace() && !updateSourceMap())
        {
            client->sendWindowMessage(
                lsp::MessageType::Error, "Failed to load sourcemap.json for workspace '" + name + "'. Instance information will not be available");
        }

        Luau::registerBuiltinTypes(frontend.typeChecker);
        Luau::registerBuiltinTypes(frontend.typeCheckerForAutocomplete);

        if (client->definitionsFile)
        {
            client->sendLogMessage(lsp::MessageType::Info, "Loading definitions file: " + client->definitionsFile->generic_string());
            registerExtendedTypes(frontend.typeChecker, *client->definitionsFile);
            registerExtendedTypes(frontend.typeCheckerForAutocomplete, *client->definitionsFile);
        }
        else
        {
            client->sendLogMessage(lsp::MessageType::Error, "Definitions file was not provided by the client. Extended types will not be provided");
            client->sendWindowMessage(
                lsp::MessageType::Error, "Definitions file was not provided by the client. Extended types will not be provided");
        }
        Luau::freeze(frontend.typeChecker.globalTypes);
        Luau::freeze(frontend.typeCheckerForAutocomplete.globalTypes);
    }
};