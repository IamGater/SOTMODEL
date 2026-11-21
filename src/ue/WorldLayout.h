#pragma once
#include "ue/Package.h"
#include "render/Math.h"

// Where the islands of the open world sit. Cooked tiles carry no world-composition offsets, so (like Athena's
// tiles.py) the placement of every streamed island level is recovered from the "Copied<Name>" NavMeshBoundsVolumes that
// sot_world_layout_2 keeps of each level's own volumes: matching a copy against the original gives the level's yaw and
// translation. Island metadata (name, sea, centre, radius) comes from sot_world_layout_2_generatedislands.

struct LevelPlacement {
    std::string level;   // game path of the sublevel .umap
    Vec3 location;       // level -> world translation (cm)
    float yaw = 0;       // degrees about Z
    int votes = 0;       // matched volumes (100 per exact name match)
    Mat4 Transform() const;
};

struct IslandPlacement {
    std::string dir;          // "Athena/Content/Maps/<sea>/<island>"
    std::string map;          // the island's persistent .umap (empty when the folder has none)
    std::string name;         // folder name / IslandName
    std::string sea;          // sea class (WideSeaOfPlunder, ...), "" when not a generated island
    std::string type;         // EIslandType without the prefix
    Vec3 location;            // island tile transform (its terrain sublevel, else the best voted level)
    float yaw = 0;
    bool manual = false;      // no volumes in the layout: a fixed placement
    Vec3 centre;              // IslandBoundsCentre (world)
    float radius = 0;         // IslandBoundsRadius
    std::vector<LevelPlacement> levels;   // sublevels with their own placement
    Mat4 Transform() const;
};

struct WorldLayout {
    std::string layoutMap;    // the persistent layout map
    std::vector<IslandPlacement> islands;
    Vec3 mapMin, mapMax;      // WorldMapMin / WorldMapMax of the generated islands asset
    const IslandPlacement* IslandOfLevel(std::string_view levelPath) const;   // by folder
    // Level -> world transform of a level of the open world (identity for the layout's own sublevels). false = not placed.
    bool LevelTransform(std::string_view levelPath, Mat4& out) const;
};

// Computed once per cache (the result is kept); thread safe.
const WorldLayout* GetWorldLayout(PackageCache& cache, std::string* error = nullptr);
bool IsWorldLayoutMap(std::string_view path);   // sot_world_layout_2(.umap)
