#include "ue/Mesh.h"

bool ReadSkeletalMesh(const Package& pkg, size_t exportIdx, MeshData& m, std::string* error) {
    (void)pkg; (void)exportIdx; (void)m;
    if (error) *error = "skeletal meshes are not implemented yet";
    return false;
}
