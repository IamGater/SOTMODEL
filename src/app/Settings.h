#pragma once
#include "core/Common.h"
#include "core/Crypto.h"

struct Settings {
    std::wstring gameDir;      // e.g. E:\depot_2020 (the folder holding Athena\Content\Paks)
    std::wstring outputDir;    // export root
    std::wstring oodlePath;    // oodle-data-shared.dll
    std::vector<std::string> aesKeys;
    int previewMaxTexture = 2048;
    bool exportKeepDirectories = true;
    int meshFormat = 0;        // 0 = glTF (.glb), 1 = OBJ
    int textureFormat = 0;     // 0 = PNG, 1 = DDS (raw blocks)
    float worldDrawDistance = 2500.0f;   // metres
    bool showGrid = true;
    // main window (FModel layout)
    int explorerViewMode = 0;      // 0 grid, 1 list
    bool loggerExpanded = true;
    float leftPanelWidth = 0;      // 0 = automatic

    static std::wstring Path();   // %APPDATA%\SOTModel\Settings.ini
    void Load();
    bool Save() const;
    std::vector<AesKey> ParsedKeys() const;
    std::wstring PaksDir() const;   // gameDir\Athena\Content\Paks if it exists, else gameDir

    // Fills empty fields (and adds keys) from %APPDATA%\FModel\AppSettings.json. Returns a summary or empty if not found.
    std::string ImportFromFModel();
};
