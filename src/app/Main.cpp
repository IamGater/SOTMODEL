#include "app/Cli.h"
#include "core/Common.h"
#include <windows.h>
#include <shellapi.h>

int RunGui();

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int rc;
    if (argc >= 2 && wcscmp(argv[1], L"--cli") == 0) rc = RunCli(argc, argv);
    else rc = RunGui();
    LocalFree(argv);
    return rc;
}
