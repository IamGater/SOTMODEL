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
    bool uiAnimations = true;       // interface transitions (sliding tab marks, fades, spinners)
    bool exportAnimSounds = true;   // animations: their notify sounds (.wav) + a .notifies.json timeline
    int meshFormat = 0;        // 0 = glTF (.glb), 1 = OBJ
    int textureFormat = 0;     // 0 = PNG, 1 = DDS (raw blocks)
    float worldDrawDistance = 2500.0f;   // metres
    bool showGrid = true;
    // main window (FModel layout)
    int explorerViewMode = 0;      // 0 grid, 1 list
    bool loggerExpanded = true;
    float leftPanelWidth = 0;      // 0 = automatic
    bool previewTextures = true;   // thumbnails on explorer cards (textures, models, materials)
    bool viewerWindow = true;      // 3D views open in the separate viewer window (else inside the tab)
    int windowX = 0, windowY = 0, windowW = 0, windowH = 0;   // last normal (restored) placement; windowW 0 = default
    bool windowMaximized = false;
    u32 accentColor = 0x206BD4;    // interface accent (0xRRGGBB): selections, status bar, checkboxes
    std::string language = "en";  // culture of the localised strings shown for FText (Localisation/<target>/<culture>)
    std::vector<std::string> favoriteDirs;   // Packages > Favorite Directories (game folder paths)

    static std::wstring Path();   // %APPDATA%\SOTModel\Settings.ini
    void Load();
    bool Save() const;
    std::vector<AesKey> ParsedKeys() const;
    std::wstring PaksDir() const;   // gameDir\Athena\Content\Paks if it exists, else gameDir

    // Fills empty fields (and adds keys) from %APPDATA%\FModel\AppSettings.json. Returns a summary or empty if not found.
    std::string ImportFromFModel();
};
