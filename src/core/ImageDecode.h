#pragma once
#include "core/Common.h"

// Loose image files of the game (the web UI's .png / .jpg / .gif / .bmp, ...) -> RGBA8, through Windows Imaging Component.
bool DecodeImage(const u8* data, size_t size, Bytes& rgba, int& width, int& height, std::string* error = nullptr);
bool IsImageFileName(std::string_view name);   // extensions DecodeImage reads
