#pragma once
#include "core/Common.h"
#include <map>
#include <string>

class Vfs;

// Values from the game's shipped config (Athena/Config/DefaultEngine.ini), read once after mounting.
namespace GameConfig {
// reads the config files from the mounted archives (Api::EnsureMounted)
void Load(const Vfs& vfs);
void Clear();
// "SurfaceType10" -> "Foliage" (PhysicsSettings +PhysicalSurfaces); SurfaceType_Default is "Default"
const std::map<std::string, std::string>& PhysicalSurfaces();
std::string PhysicalSurfaceName(const std::string& type);
}
