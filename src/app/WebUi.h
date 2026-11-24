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

// Local web server (http://127.0.0.1:<port>/<game path>) that runs the pages in a normal browser: files come straight
// from the archives, coui:// links (web root files, fonts, Texture2D'/Game/...' as PNG) are rewritten to it and pages
// get a script standing in for the game (Coherent engine calls / events answered with empty data, feature flags,
// localised text from the game's locres; ?state=Engage (title, default) / Lobby / PirateSelection / ... picks the front
// end screen: the BootFlowStart<state>StateEvent the game would send).
// Starts on first use; the URL of a game path, or "" + error.
std::string WebUiUrl(Api& a, std::string_view gamePath, std::string* error = nullptr);
void StopWebUiServer();   // before the archives are unmounted / the Api goes away
