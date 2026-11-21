#pragma once
#include "core/Common.h"

// Small helpers shared by the UI translation units (App.cpp: viewers / jobs, Shell.cpp: main window).
std::string HumanSize(u64 bytes);
void CopyToClipboard(const std::string& s);
std::string GamePathToPackage(std::string_view gamePath);   // Athena/Content/X/y.uasset -> /Game/X/y
bool IsPackageFile(std::string_view path);                 // .uasset / .umap
