#include <Dashboard.h>
#include <unistd.h>

#include <iostream>
#include <stdexcept>
#include <string_view>

int
main(int argc, char** argv)
{
    try
    {
        std::filesystem::path data = BALLAST_DASHBOARD_DATA;
        ballast::dashboard::State state;
        bool plain = !isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO);
        for (int i = 1; i < argc; ++i)
        {
            std::string_view arg = argv[i];
            if (arg == "--plain")
                plain = true;
            else if (arg == "--provenance")
                state.provenance = true;
            else if (arg == "--data" && i + 1 < argc)
                data = argv[++i];
            else if (arg == "--stage" && i + 1 < argc)
            {
                auto value = std::string_view(argv[++i]);
                if (value == "parse_only")
                    state.stage = 0;
                else if (value == "verify_only")
                    state.stage = 1;
                else if (value == "parse_verify")
                    state.stage = 2;
                else
                    throw std::invalid_argument("Unknown stage");
            }
            else if (arg == "--help")
            {
                std::cout
                    << "Ballast C++ dashboard\nUsage: ballast_dashboard [--data FILE] [--plain] "
                       "[--stage parse_only|verify_only|parse_verify] [--provenance]\n"
                       "Keys: 1/2/3 stage, m metric, r run, j/k or arrows select, p provenance, q "
                       "quit\n";
                return 0;
            }
            else
                throw std::invalid_argument("Unknown option or missing value");
        }
        auto const snapshot = ballast::dashboard::loadSnapshot(data);
        if (plain)
        {
            for (auto const& line : ballast::dashboard::render(snapshot, state))
                std::cout << line << '\n';
            return 0;
        }
        return ballast::dashboard::interactive(snapshot, state);
    }
    catch (std::exception const& error)
    {
        std::cerr << "Dashboard: " << ballast::dashboard::safeText(error.what()) << '\n';
        return 1;
    }
}
