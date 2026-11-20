#include "app/Cli.h"
#include "core/Common.h"
#include <windows.h>
#include <shellapi.h>

int RunGui(const std::vector<std::string>& open);
int RunMcp();

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int rc;
    if (argc >= 2 && wcscmp(argv[1], L"--cli") == 0) rc = RunCli(argc, argv);
    else if (argc >= 2 && wcscmp(argv[1], L"--mcp") == 0) rc = RunMcp();
    else {
        std::vector<std::string> open;   // SOTModel.exe <asset path>... opens assets in tabs
        for (int i = 1; i < argc; i++) open.push_back(Narrow(argv[i]));
        rc = RunGui(open);
    }
    LocalFree(argv);
    return rc;
}
