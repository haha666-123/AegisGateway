#include <cstring>
#include <string>
#include <string_view>

#include "TestFramework.h"

int main(int argc, char** argv) {
    std::string filter;
    bool list_only = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg.starts_with("--filter=")) {
            filter = std::string(arg.substr(std::strlen("--filter=")));
        } else if (arg == "--list") {
            list_only = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: aegis_tests [--filter=substr] [--list]\n";
            return 0;
        }
    }

    return aegis::test::Registry::instance().run(filter, list_only);
}
