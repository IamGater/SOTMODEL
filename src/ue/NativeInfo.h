#pragma once
#include "ue/Package.h"

class JsonWriter;

// Adds a short description of the class-specific binary data of an export (mesh/texture/anim summaries) to its JSON.
void WriteNativeInfo(JsonWriter& w, const Package& pkg, size_t exportIdx, size_t propsEnd);
