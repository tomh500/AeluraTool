#include <windows.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
void run_hash_tests(fs::path const& root);
void run_migration_tests(fs::path const& root);
void run_filesearch_tests(fs::path const& root);

int main(int argc, char** argv) {
    const std::string suite = argc == 3 && std::string(argv[1]) == "--suite" ? argv[2] : "all";
    if ((argc != 1 && argc != 3) || (argc == 3 && std::string(argv[1]) != "--suite") ||
        (suite != "all" && suite != "hash" && suite != "migration")) {
        std::cerr << "Usage: CoreTests.exe [--suite all|hash|migration]\n";
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
        fs::remove_all(root);
        std::cout << "PASS: selected suites completed\n";
        return 0;
    } catch (std::exception const& error) {
        // Preserve evidence and recovery files on failure, including any remaining junctions.
        std::cerr << "FAIL: " << error.what() << " (sandbox preserved: " << root << ")\n";
        return 1;
    }
}
