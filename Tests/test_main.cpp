#include <windows.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include "../Integration/Privileges.h"

namespace fs = std::filesystem;
void run_hash_tests(fs::path const& root);
void run_migration_tests(fs::path const& root);
void run_filesearch_tests(fs::path const& root);
void run_everything_tests(fs::path const& root);
void run_file_shell_tests(fs::path const& root);
void run_privileges_tests(fs::path const& root);
void run_autoclicker_tests();
void run_ipv6_tests(fs::path const& root);
void run_network_tests();
void run_network_live_probe();

int main(int argc, char** argv) {
    bool startup{};
    if (!aelura::privileges::prepareLaunch(startup)) return ERROR_ACCESS_DENIED;
    if(argc==2 && std::string(argv[1])=="--network-live"){run_network_live_probe();return 0;}
    const std::string suite = argc == 3 && std::string(argv[1]) == "--suite" ? argv[2] : "all";
    if ((argc != 1 && argc != 3) || (argc == 3 && std::string(argv[1]) != "--suite") ||
        (suite != "all" && suite != "hash" && suite != "migration" && suite != "network")) {
        std::cerr << "Usage: CoreTests.exe [--suite all|hash|migration|network] or --network-live\n";
        return 2;
    }

    fs::path root;
    try {
        // Atomically reserve a fresh sandbox; never reuse a directory from a prior run.
        const auto temporary = fs::temp_directory_path();
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            auto candidate = temporary / (L"AeluraTests-" + std::to_wstring(GetCurrentProcessId()) +
                L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt));
            if (fs::create_directory(candidate)) { root = candidate; break; }
        }
        if (root.empty()) throw std::runtime_error("Cannot reserve a test sandbox");
        if (suite == "all" || suite == "hash") run_hash_tests(root / "hash");
        if (suite == "all" || suite == "migration") run_migration_tests(root / "migration");
        if (suite == "all") run_filesearch_tests(root / "filesearch");
        if (suite == "all") run_everything_tests(root / "everything");
        if (suite == "all") run_file_shell_tests(root / "shell");
        if (suite == "all") run_privileges_tests(root / "privileges");
        if (suite == "all") run_autoclicker_tests();
        if (suite == "all") run_ipv6_tests(root / "ipv6");
        if (suite == "all" || suite == "network") run_network_tests();
        fs::remove_all(root);
        std::cout << "PASS: selected suites completed\n";
        return 0;
    } catch (std::exception const& error) {
        // Preserve evidence and recovery files on failure, including any remaining junctions.
        std::cerr << "FAIL: " << error.what() << " (sandbox preserved: " << root << ")\n";
        return 1;
    }
}
