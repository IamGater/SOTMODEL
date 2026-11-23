#pragma once
#include "app/Api.h"

// The game's HTML interface (Coherent UI): Athena/Content/WebRoot (front end menus, HUD, tooltips) and
// Athena/Content/uiresources (in-game HUD pages). Loose files, served to the embedded browser by path.

// The web root folder a game path sits in ("Athena/Content/WebRoot/"), or "" when it is not part of the web UI.
std::string WebRootOf(std::string_view gamePath);
bool IsWebPageFile(std::string_view name);   // .html / .htm

// Writes every file of the web root to <outputDir>\Web\<root path>\..., keeping the folder layout so relative links
// work (files already there with the same size are skipped). Returns the local folder of the root.
bool ExportWebRoot(Api& a, const std::string& root, const std::wstring& outputDir, std::wstring& localRoot, int* written = nullptr,
                   int* total = nullptr, std::string* error = nullptr);
