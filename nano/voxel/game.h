#ifndef VOXEL_GAME_H
#define VOXEL_GAME_H
#include <stddef.h>
#include <stdint.h>

#define VG_WORLD_X 64
#define VG_WORLD_Y 32
#define VG_WORLD_Z 64
#define VG_BLOCK_COUNT (VG_WORLD_X * VG_WORLD_Y * VG_WORLD_Z)
#define VG_LEGACY_SAVE_BYTES (12u + VG_BLOCK_COUNT)
#define VG_SAVE_BYTES (VG_LEGACY_SAVE_BYTES + 4u * VG_BLOCK_TYPES + 4u)
#define VG_MAX_ITEMS 1000000u
#define VG_TOOL_BIT(tool) (1u << (tool))
#define VG_MAX_TRIANGLES 65536u

enum vg_block { VG_AIR, VG_GRASS, VG_DIRT, VG_STONE, VG_WOOD, VG_LEAVES, VG_SAND, VG_WATER, VG_TORCH, VG_BLOCK_TYPES };
#define VG_ATLAS_WIDTH 512
#define VG_ATLAS_HEIGHT 32
#define VG_ATLAS_TILE_SIZE 32
#define VG_MAX_LIGHTS 128u
#define VG_DAY_SECONDS 600.0f
#define VG_MAX_EVENTS 16u
enum vg_tool { VG_HAND = 1, VG_PICKAXE, VG_AXE, VG_SHOVEL, VG_TOOL_TYPES };
enum vg_event_type { VG_STEP_SOUND, VG_JUMP_SOUND, VG_LAND_SOUND, VG_BREAK_SOUND, VG_PLACE_SOUND, VG_HIT_SOUND, VG_SWING_SOUND, VG_CRAFT_SOUND, VG_DENIED_SOUND, VG_EVENT_TYPES };
enum vg_craft { VG_CRAFT_NONE, VG_CRAFT_PICKAXE, VG_CRAFT_AXE, VG_CRAFT_SHOVEL, VG_CRAFT_TORCH };
enum vg_mode { VG_SURVIVAL=1, VG_CREATIVE };
enum vg_time_mode { VG_TIME_CYCLE=1, VG_TIME_DAY, VG_TIME_NIGHT };
enum vg_button { VG_JUMP = 1u, VG_BREAK = 2u, VG_PLACE = 4u, VG_SAVE = 8u, VG_LOAD = 16u, VG_NEXT_TIME = 32u };
typedef struct vg_vec3 { float x, y, z; } vg_vec3;
typedef struct vg_event { unsigned int type, block; vg_vec3 position; } vg_event;
typedef struct vg_vertex { vg_vec3 position; float u, v; float ambient, block_light; } vg_vertex;
typedef struct vg_triangle { vg_vertex v[3]; unsigned int material; } vg_triangle;
typedef struct vg_camera { vg_vec3 position, forward, right, up; float vertical_fov, near_plane, far_plane; } vg_camera;
typedef struct vg_input {
    float forward, strafe; /* [-1,1]; positive is forward/right */
    float look_yaw, look_pitch; /* radians since last frame; positive = right/up */
    uint32_t buttons; /* held bits; the core detects rising edges */
    unsigned int selected_block; /* 0 keeps selection; otherwise enum vg_block */
    unsigned int selected_tool; /* 0 keeps selection; otherwise enum vg_tool */
    unsigned int craft; /* one-shot recipe command; works in inventory/pause */
    unsigned int game_mode, time_mode; /* 0 keeps setting; enum values apply even paused */
    int paused; /* skips physics, look, and edits; selection still updates */
} vg_input;
typedef struct vg_frame {
    const vg_triangle *triangles; /* owned by core, valid until next vg_build_frame/destroy */
    size_t triangle_count;
    vg_camera camera;
    unsigned int selected_block;
    int target_valid, target_x, target_y, target_z;
    float time_of_day; /* [0,1): midnight=0, sunrise=.25, noon=.5, sunset=.75 */
    float animation_time; /* bounded seconds, advances only while unpaused */
    const vg_vec3 *lights; /* torch positions, game-owned until next build/import/destroy */
    size_t light_count;
    int underwater, lights_truncated;
    unsigned int selected_tool;
    uint32_t inventory[VG_BLOCK_TYPES];
    uint32_t tool_mask; /* available tools; creative does not change owned tools */
    unsigned int game_mode, time_mode;
    int swing_active, placing;
    float swing_progress; /* independent [0,1] repeated action phase */
    float mining_progress; /* [0,1] for a held tool on the current target */
    const vg_event *events; /* last step only; valid until next step/import/destroy */
    size_t event_count; /* consume once per step, not once per render */
    int mesh_truncated; /* increase capacity or reduce draw distance if nonzero */
} vg_frame;
typedef struct vg_game vg_game;

vg_game *vg_create(uint32_t seed);
void vg_destroy(vg_game *game);
void vg_step(vg_game *game, const vg_input *input, float seconds);
void vg_build_frame(vg_game *game, vg_frame *frame);
unsigned int vg_get_block(const vg_game *game, int x, int y, int z);
/* Rejects out of range coordinates, the foundation (y=0), and solid-block/player overlap. Torches need solid support. */
int vg_set_block(vg_game *game, int x, int y, int z, unsigned int block);
/* World editing bypasses survival inventory; normal gameplay uses vg_step. */
/* Stable byte format, no pointers, struct dumps, or file I/O. Import is transactional. */
int vg_export_world(const vg_game *game, void *bytes, size_t size);
int vg_import_world(vg_game *game, const void *bytes, size_t size);
#endif
