#extension GL_ARB_gpu_shader_int64 : enable

#define uint64_t uint64_t

#if !defined(VP_STAGE_FRAGMENT)

#define OE_TILE_SIZE 17

#define MAX_TILE_VERTS 417

struct Global {
    vec2 uvs[MAX_TILE_VERTS];
    float padding[2];
};

struct Tile {
    vec4 verts[MAX_TILE_VERTS];
    vec4 normals[MAX_TILE_VERTS];
    mat4 modelViewMatrix;
    mat4 colorMat;
    mat4 elevMat;
    mat4 normalMat;
    mat4 parentMat;
    int colorIndex;
    int elevIndex;
    int normalIndex;
    int parentIndex;
    //float padding[1];
};

#undef MAX_TILE_VERTS

layout(binding = 0, std430) readonly buffer TileBuffer {
    Tile tile[];
};
layout(binding = 1, std430) readonly buffer GlobalBuffer {
    Global global;
};
layout(binding = 5, std430) readonly buffer TextureArena {
    uint64_t tex[];
};

int oe_tileID; // vertex stage global

// Vertex Markers:
//#define VERTEX_VISIBLE  1
//#define VERTEX_BOUNDARY 2
//#define VERTEX_HAS_ELEVATION 4
//#define VERTEX_SKIRT 8

#endif // !VP_STAGE_FRAGMENT
