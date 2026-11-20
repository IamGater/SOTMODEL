#pragma once
#include "ue/Package.h"

// FModel-like JSON of every export (type, name, outer, properties and a summary of the native data that follows them).
std::string PackageToJson(const Package& pkg);
