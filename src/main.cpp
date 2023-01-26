#include "LSP/LanguageServer.hpp"
#include "Analyze/AnalyzeCli.hpp"
#include "Luau/ExperimentalFlags.h"
#include "argparse/argparse.hpp"

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

LUAU_FASTINT(LuauTarjanChildLimit)

static void displayFlags()
{
    printf("Available flags:\n");

    for (Luau::FValue<bool>* flag = Luau::FValue<bool>::list; flag; flag = flag->next)
    {
        printf("  %s=%s\n", flag->name, flag->value ? "true" : "false");
    }

    for (Luau::FValue<int>* flag = Luau::FValue<int>::list; flag; flag = flag->next)
    {
        printf("  %s=%d\n", flag->name, flag->value);
    }
}

void registerFastFlags(std::unordered_map<std::string, std::string>& fastFlags)
{
    for (Luau::FValue<bool>* flag = Luau::FValue<bool>::list; flag; flag = flag->next)
    {
        if (fastFlags.find(flag->name) != fastFlags.end())
        {
            std::string valueStr = fastFlags.at(flag->name);

            if (valueStr == "true" || valueStr == "True")
                flag->value = true;
            else if (valueStr == "false" || valueStr == "False")
                flag->value = false;
            else
            {
                std::cerr << "Bad flag option, expected a boolean 'True' or 'False' for flag " << flag->name << "\n";
                std::exit(1);
            }

            fastFlags.erase(flag->name);
        }
    }

    for (Luau::FValue<int>* flag = Luau::FValue<int>::list; flag; flag = flag->next)
    {
        if (fastFlags.find(flag->name) != fastFlags.end())
        {
            std::string valueStr = fastFlags.at(flag->name);

            int value = 0;
            try
            {
                value = std::stoi(valueStr);
            }
            catch (...)
            {
                std::cerr << "Bad flag option, expected an int for flag " << flag->name << "\n";
                std::exit(1);
            }

            flag->value = value;
            fastFlags.erase(flag->name);
        }
    }

    for (auto& [key, _] : fastFlags)
    {
        std::cerr << "Unknown FFlag: " << key << "\n";
    }
}

int startLanguageServer(argparse::ArgumentParser program)
{
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    std::vector<std::filesystem::path> definitionsFiles;
    std::optional<std::filesystem::path> documentationFile;

    if (auto definitions = program.present<std::vector<std::filesystem::path>>("--definitions"))
    {
        definitionsFiles = *definitions;
    }
    // TODO: docs

    LanguageServer server(definitionsFiles, documentationFile);

    // Begin input loop
    server.processInputLoop();

    // If we received a shutdown request before exiting, exit normally. Otherwise, it is an abnormal exit
    return server.requestedShutdown() ? 0 : 1;
}

int main(int argc, char** argv)
{
    // Debug loop: uncomment and set a breakpoint on while to attach debugger before init
    // auto d = 4;
    // while (d == 4)
    // {
    //     d = 4;
    // }

    Luau::assertHandler() = [](const char* expr, const char* file, int line, const char*) -> int
    {
        fprintf(stderr, "%s(%d): ASSERTION FAILED: %s\n", file, line, expr);
        return 1;
    };

    argparse::ArgumentParser program("luau-lsp");

    // Global arguments
    program.add_argument("--show-flags")
        .help("Display all the currently available Luau FFlags and their values")
        .default_value(false)
        .implicit_value(true);

    program.add_argument("--no-flags-enabled").help("Do not enable all Luau FFlags by default").default_value(false).implicit_value(true);

    // LSP sub command
    argparse::ArgumentParser lsp_command("lsp");
    lsp_command.add_description("Start the language server");
    lsp_command.add_epilog("This will start up a server which listens to LSP messages on stdin, and responds on stdout");
    lsp_command.add_argument("--definitions").help("A path to a Luau definitions file to load into the global namespace").append().metavar("PATH");
    lsp_command.add_argument("--docs", "--documentation")
        .help("A path to a Luau documentation database for loaded definitions")
        .append()
        .metavar("PATH");

    // Analyze sub command
    argparse::ArgumentParser analyze_command("analyze");
    analyze_command.add_description("Run luau-analyze type checking and linting");
    analyze_command.add_argument("--annotate")
        .help("Output the source file with type annotations after typechecking")
        .default_value(false)
        .implicit_value(true);
    analyze_command.add_argument("--timetrace")
        .help("Record compiler time tracing information into trace.json")
        .default_value(false)
        .implicit_value(true);
    analyze_command.add_argument("--formatter")
        .default_value(std::string("default"))
        .action(
            [](const std::string& value)
            {
                static const std::vector<std::string> choices = {"default", "plain", "gnu"};
                if (std::find(choices.begin(), choices.end(), value) != choices.end())
                {
                    return value;
                }
                return std::string{"default"};
            })
        .help("Output analysis errors in a particular format. [Values: default, plain/luacheck, gnu]")
        .default_value(false)
        .implicit_value(true);
    analyze_command.add_argument("--sourcemap").help("A path to a Rojo-style instance sourcemap to understand the DataModel").metavar("PATH");
    analyze_command.add_argument("--definitions")
        .help("A path to a Luau definitions file to load into the global namespace")
        .append()
        .metavar("PATH");
    analyze_command.add_argument("--ignore").help("A file glob pattern for ignoring error outputs").append().metavar("GLOB");
    analyze_command.add_argument("files").help("Files to perform analysis on").remaining();

    program.add_subparser(lsp_command);
    program.add_subparser(analyze_command);

    try
    {
        program.parse_args(argc, argv);
    }
    catch (const std::runtime_error& err)
    {
        std::cerr << err.what() << std::endl;
        std::cerr << program;
        std::exit(1);
    }

    // Display flags if requested
    if (program.is_used("--show-flags"))
    {
        displayFlags();
        return 0;
    }

    // Parse provided FFlags
    bool enableAllFlags = !program.is_used("--no-flags-enabled");

    std::unordered_map<std::string, std::string> fastFlags;
    for (int i = 1; i < argc; i++)
    {
        if (strncmp(argv[i], "--flag:", 7) == 0)
        {
            std::string flagSet = std::string(argv[i] + 7);

            size_t eqIndex = flagSet.find("=");
            if (eqIndex == std::string::npos)
            {
                std::cerr << "Bad flag option, missing =: " << flagSet << "\n";
                return 1;
            }

            std::string flagName = flagSet.substr(0, eqIndex);
            std::string flagValue = flagSet.substr(eqIndex + 1, flagSet.length());
            fastFlags.emplace(flagName, flagValue);
        }
    }

    if (enableAllFlags)
    {
        for (Luau::FValue<bool>* flag = Luau::FValue<bool>::list; flag; flag = flag->next)
            if (strncmp(flag->name, "Luau", 4) == 0 && !Luau::isFlagExperimental(flag->name))
                flag->value = true;
    }
    registerFastFlags(fastFlags);

    // Manually enforce a LuauTarjanChildLimit increase
    // TODO: re-evaluate the necessity of this change
    if (FInt::LuauTarjanChildLimit > 0 && FInt::LuauTarjanChildLimit < 15000)
        FInt::LuauTarjanChildLimit.value = 15000;

    if (program.is_subcommand_used("lsp"))
        return startLanguageServer(program);
    else if (program.is_subcommand_used("analyze"))
        return startAnalyze(argc, argv);

    // No sub-command specified
    std::cerr << "Specify a particular mode to run the program (analyze/lsp)" << std::endl;
    std::cerr << program;
    return 1;
}