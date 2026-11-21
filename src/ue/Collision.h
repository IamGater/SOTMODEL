#pragma once
#include "ue/Mesh.h"
#include "render/Math.h"

// Simple collision of a mesh: the BodySetup's AggGeom (boxes, spheres, capsules, convex hulls) of a StaticMesh, or the
// bodies of a SkeletalMesh's PhysicsAsset placed at their bones in the reference pose.
struct CollisionShape {
    enum Kind : u8 { Box, Sphere, Capsule, Convex } kind = Box;
    Mat4 transform;                // shape -> mesh space (centre + orientation; bone pose for physics bodies)
    Vec3 extent;                   // box: half extents
    float radius = 0, length = 0;  // sphere / capsule radius; capsule: cylinder length along Z (without the caps)
    std::vector<Vec3> vertices;    // convex: hull points (shape space)
    std::vector<u32> indices;      // convex: hull triangles
    std::string bone;              // physics body bone ("" for static meshes)
};

struct CollisionData {
    std::vector<CollisionShape> shapes;
    std::string source;       // BodySetup export or PhysicsAsset path
    std::string traceFlag;    // CollisionTraceFlag (CTF_UseDefault / UseSimpleAsComplex / UseComplexAsSimple)
    int bodies = 0;           // physics asset bodies
    int Count(CollisionShape::Kind k) const;
};

// mesh: the decoded mesh (for skeletal meshes its reference skeleton places the bodies). false = no simple collision.
bool ReadMeshCollision(PackageCache& cache, const Package& pkg, size_t meshExport, const MeshData& mesh, CollisionData& out, std::string* error = nullptr);

// Wireframe segments (pairs of points, mesh space) and a triangle mesh of all shapes (for export).
void CollisionLines(const CollisionData& c, std::vector<Vec3>& segments);
MeshLod CollisionMesh(const CollisionData& c);

// 3D convex hull (incremental): outward triangles over the input points. false when the points are degenerate.
bool ConvexHull(const std::vector<Vec3>& points, std::vector<u32>& triangles);
