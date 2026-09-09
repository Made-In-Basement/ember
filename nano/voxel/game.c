#include "game.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RADIUS 0.30f
#define HEIGHT 1.80f
#define EYE 1.62f
#define DRAW_DISTANCE 30
#define PI 3.14159265358979323846f

struct vg_game {
    unsigned char blocks[VG_BLOCK_COUNT];
    vg_triangle *mesh;
    size_t count;
    unsigned char light[VG_BLOCK_COUNT];
    uint32_t *light_queue;
    int light_dirty;
    vg_vec3 lights[VG_MAX_LIGHTS];
    size_t light_count;
    int lights_truncated;
    float day_time, animation_time;
    vg_vec3 feet;
    float yaw, pitch, velocity_y, accumulator;
    uint32_t previous;
    unsigned int selected, tool;
    uint32_t inventory[VG_BLOCK_TYPES], tool_mask;
    unsigned int game_mode, time_mode;
    float swing_elapsed;
    int swing_active, placing;
    vg_event events[VG_MAX_EVENTS];
    size_t event_count;
    float walk_distance, mining, mining_progress;
    int mining_cell[3];
    int grounded, jump_pending, truncated;
};
static void emit(vg_game *g, unsigned int type, unsigned int block, vg_vec3 position)
{
    if (g->event_count < VG_MAX_EVENTS) {
        vg_event *e=&g->events[g->event_count++];
        e->type=type; e->block=block; e->position=position;
    }
}
static int inside(int x, int y, int z)
{
    return x >= 0 && x < VG_WORLD_X && y >= 0 && y < VG_WORLD_Y && z >= 0 && z < VG_WORLD_Z;
}
static size_t index_of(int x, int y, int z)
{
    return ((size_t)y * VG_WORLD_Z + (size_t)z) * VG_WORLD_X + (size_t)x;
}
static float bounded(float x, float lo, float hi)
{
    if (!isfinite(x)) return 0.0f;
    return x < lo ? lo : (x > hi ? hi : x);
}
static uint32_t hash(uint32_t x)
{
    x ^= x >> 16; x *= UINT32_C(0x7feb352d);
    x ^= x >> 15; x *= UINT32_C(0x846ca68b);
    return x ^ (x >> 16);
}
static float lattice(int x, int z, uint32_t seed)
{
    return (float)(hash((uint32_t)x * UINT32_C(73856093) ^ (uint32_t)z * UINT32_C(19349663) ^ seed) & 65535u) / 65535.0f;
}
static float noise(int x, int z, int scale, uint32_t seed)
{
    int a = x / scale, b = z / scale;
    float u = (float)(x % scale) / (float)scale, v = (float)(z % scale) / (float)scale;
    float p, q;
    u = u*u*(3.0f-2.0f*u); v = v*v*(3.0f-2.0f*v);
    p = lattice(a,b,seed)*(1.0f-u) + lattice(a+1,b,seed)*u;
    q = lattice(a,b+1,seed)*(1.0f-u) + lattice(a+1,b+1,seed)*u;
    return p*(1.0f-v)+q*v;
}
unsigned int vg_get_block(const vg_game *g, int x, int y, int z)
{
    return g && inside(x,y,z) ? g->blocks[index_of(x,y,z)] : VG_AIR;
}
static int solid(unsigned int b) { return b!=VG_AIR && b!=VG_WATER && b!=VG_TORCH; }
static int overlap(vg_vec3 p, int x, int y, int z)
{
    return p.x+RADIUS > (float)x && p.x-RADIUS < (float)x+1.0f &&
           p.y+HEIGHT > (float)y && p.y < (float)y+1.0f &&
           p.z+RADIUS > (float)z && p.z-RADIUS < (float)z+1.0f;
}
int vg_set_block(vg_game *g, int x, int y, int z, unsigned int block)
{
    if (!g || !inside(x,y,z) || y == 0 || block >= VG_BLOCK_TYPES) return 0;
    if (vg_get_block(g,x,y,z)==block) return 0;
    if (solid(block) && overlap(g->feet,x,y,z)) return 0;
    if (block==VG_TORCH && !solid(vg_get_block(g,x,y-1,z))) return 0;
    if (!solid(block) && vg_get_block(g,x,y+1,z)==VG_TORCH) g->blocks[index_of(x,y+1,z)]=VG_AIR;
    g->light_dirty=1;
    g->blocks[index_of(x,y,z)] = (unsigned char)block;
    return 1;
}
static void spawn(vg_game *g)
{
    int y;
    /* Guaranteed clear spawn even for a world filled up to its ceiling. */
    g->feet.x = VG_WORLD_X / 2 + 0.5f;
    g->feet.z = VG_WORLD_Z / 2 + 0.5f;
    for (y = VG_WORLD_Y-1; y >= 0; --y)
        if (vg_get_block(g,VG_WORLD_X/2,y,VG_WORLD_Z/2)) break;
    g->feet.y = (float)y+1.001f;
    g->velocity_y = g->accumulator = 0.0f;
    g->grounded = g->jump_pending = 0;
    g->event_count=0; g->mining=g->mining_progress=g->walk_distance=0;
    g->swing_active=0; g->swing_elapsed=0; g->placing=0;
    g->previous=0; g->light_dirty=1; g->light_count=0;
}
vg_game *vg_create(uint32_t seed)
{
    vg_game *g = (vg_game *)malloc(sizeof(*g));
    int x,y,z;
    if (!g) return NULL;
    memset(g,0,sizeof(*g));
    g->mesh = (vg_triangle *)malloc(sizeof(vg_triangle)*VG_MAX_TRIANGLES);
    if (!g->mesh) { free(g); return NULL; }
    g->light_queue=(uint32_t *)malloc(sizeof(uint32_t)*VG_BLOCK_COUNT);
    if (!g->light_queue) { free(g->mesh); free(g); return NULL; }
    g->game_mode=VG_SURVIVAL; g->time_mode=VG_TIME_CYCLE;
    g->selected = VG_DIRT; g->tool=VG_HAND; g->tool_mask=VG_TOOL_BIT(VG_HAND); g->day_time=0.36f;
    for (z=0; z<VG_WORLD_Z; ++z) for (x=0; x<VG_WORLD_X; ++x) {
        int h = 6+(int)(noise(x,z,16,seed)*11.0f+noise(x,z,7,seed^123u)*4.0f);
        float lx=(float)(x-32)/17.0f, lz=(float)(z-16)/12.0f;
        float lake=lx*lx+lz*lz;
        if (z<16) {
            float rise=(float)(16-z)/16.0f;
            h+=(int)(rise*rise*(4.0f+noise(x,z,9,seed^789u)*5.0f));
        }
        if (lake<0.64f) h=5+(int)(lake*4.0f);
        else if (lake<1.35f) {
            float shore=(lake-0.64f)/0.71f;
            shore=shore*shore*(3.0f-2.0f*shore);
            h=(int)(7.0f+((float)h-7.0f)*shore);
        }
        if ((x-32)*(x-32)+(z-32)*(z-32)<16 && h<11) h=11;
        for (y=0; y<=h; ++y)
            g->blocks[index_of(x,y,z)] = (unsigned char)(y==h ? (h<=9 ? VG_SAND : VG_GRASS) : (y>h-3 ? VG_DIRT : VG_STONE));
        for (y=h+1; y<=9; ++y) g->blocks[index_of(x,y,z)]=VG_WATER;
    }
    for (z=3; z<VG_WORLD_Z-3; ++z) for (x=3; x<VG_WORLD_X-3; ++x) {
        uint32_t r = hash(seed ^ (uint32_t)(z*VG_WORLD_X+x));
        if (r%79u == 0 && (x<VG_WORLD_X/2-3 || x>VG_WORLD_X/2+3 || z<VG_WORLD_Z/2-3 || z>VG_WORLD_Z/2+3)) {
            int h,dx,dz,dy;
            for (h=VG_WORLD_Y-7; h>0 && !vg_get_block(g,x,h,z); --h) {}
            if (vg_get_block(g,x,h,z)!=VG_GRASS) continue;
            for (dy=3; dy<=6; ++dy) for (dz=-2; dz<=2; ++dz) for (dx=-2; dx<=2; ++dx)
                if (dx*dx+dz*dz+(dy-4)*(dy-4)<9 && !vg_get_block(g,x+dx,h+dy,z+dz))
                    g->blocks[index_of(x+dx,h+dy,z+dz)] = VG_LEAVES;
            for (dy=1; dy<=4; ++dy) g->blocks[index_of(x,h+dy,z)] = VG_WOOD;
        }
    }
    /* A few trail torches make the starting shore readable at night. */
    for (x=28; x<=36; x+=4) {
        int h;
        for (h=VG_WORLD_Y-2; h>0 && !solid(vg_get_block(g,x,h,30)); --h) {}
        if (vg_get_block(g,x,h+1,30)==VG_AIR) g->blocks[index_of(x,h+1,30)]=VG_TORCH;
    }
    spawn(g);
    return g;
}
void vg_destroy(vg_game *g)
{
    if (g) { free(g->light_queue); free(g->mesh); free(g); }
}
static vg_camera camera(const vg_game *g)
{
    vg_camera c;
    float cy=cosf(g->yaw), sy=sinf(g->yaw), cp=cosf(g->pitch), sp=sinf(g->pitch);
    c.position=g->feet; c.position.y+=EYE;
    c.forward.x=sy*cp; c.forward.y=sp; c.forward.z=-cy*cp;
    c.right.x=cy; c.right.y=0; c.right.z=sy;
    c.up.x=-sy*sp; c.up.y=cp; c.up.z=cy*sp;
    c.vertical_fov=PI/3.0f; c.near_plane=0.05f; c.far_plane=80.0f;
    return c;
}
static int collides(const vg_game *g, vg_vec3 p)
{
    int x,y,z;
    if (p.x-RADIUS<0 || p.z-RADIUS<0 || p.x+RADIUS>VG_WORLD_X || p.z+RADIUS>VG_WORLD_Z || p.y<1.0f) return 1;
    for (y=(int)floorf(p.y); y<=(int)floorf(p.y+HEIGHT-0.0001f); ++y)
        for (z=(int)floorf(p.z-RADIUS); z<=(int)floorf(p.z+RADIUS-0.0001f); ++z)
            for (x=(int)floorf(p.x-RADIUS); x<=(int)floorf(p.x+RADIUS-0.0001f); ++x)
                if (solid(vg_get_block(g,x,y,z))) return 1;
    return 0;
}
static void move_axis(vg_game *g, int axis, float delta)
{
    vg_vec3 p=g->feet;
    float lo=0.0f, hi=1.0f;
    int i;
    if (axis==0) p.x+=delta; else if (axis==1) p.y+=delta; else p.z+=delta;
    if (!collides(g,p)) { g->feet=p; return; }
    /* Fixed steps keep delta below one block; binary search resolves contact. */
    for (i=0; i<12; ++i) {
        float mid=(lo+hi)*0.5f;
        p=g->feet;
        if (axis==0) p.x+=delta*mid; else if (axis==1) p.y+=delta*mid; else p.z+=delta*mid;
        if (collides(g,p)) hi=mid; else lo=mid;
    }
    if (axis==0) g->feet.x+=delta*lo;
    else if (axis==2) g->feet.z+=delta*lo;
    else { g->feet.y+=delta*lo; g->velocity_y=0; if (delta<0) g->grounded=1; }
}
/* Exact grid traversal. Previous cell is the placement cell. */
static int ray(const vg_game *g, int hit[3], int previous[3])
{
    vg_camera c=camera(g);
    float origin[3]={c.position.x,c.position.y,c.position.z};
    float dir[3]={c.forward.x,c.forward.y,c.forward.z};
    float next[3], stride[3], distance=0.0f;
    int cell[3], step[3], a,i;
    for (a=0; a<3; ++a) {
        cell[a]=(int)floorf(origin[a]); previous[a]=cell[a];
        step[a]=dir[a]>=0 ? 1 : -1;
        stride[a]=fabsf(dir[a])>0.000001f ? 1.0f/fabsf(dir[a]) : 1.0e30f;
        next[a]=(dir[a]>=0 ? (float)cell[a]+1.0f-origin[a] : origin[a]-(float)cell[a])*stride[a];
        if (fabsf(dir[a])<=0.000001f) next[a]=1.0e30f;
    }
    for (i=0; i<32 && distance<=6.0f; ++i) {
        if (vg_get_block(g,cell[0],cell[1],cell[2])!=VG_AIR && (vg_get_block(g,cell[0],cell[1],cell[2])!=VG_WATER || g->selected==VG_WATER)) {
            for (a=0; a<3; ++a) hit[a]=cell[a];
            return 1;
        }
        for (a=0; a<3; ++a) previous[a]=cell[a];
        a=next[0]<next[1] ? 0 : 1;
        if (next[2]<next[a]) a=2;
        distance=next[a]; cell[a]+=step[a]; next[a]+=stride[a];
    }
    return 0;
}
static void craft(vg_game *g,unsigned int recipe)
{
    unsigned int wood,stone,tool;
    if (!recipe || recipe>VG_CRAFT_TORCH) return;
    tool=recipe+1;
    wood=recipe==VG_CRAFT_TORCH ? 1u : 2u;
    stone=recipe==VG_CRAFT_TORCH || recipe==VG_CRAFT_SHOVEL ? 1u : 3u;
    if ((recipe!=VG_CRAFT_TORCH && (g->tool_mask & VG_TOOL_BIT(tool))) ||
        g->inventory[VG_WOOD]<wood || g->inventory[VG_STONE]<stone ||
        (recipe==VG_CRAFT_TORCH && g->inventory[VG_TORCH]>VG_MAX_ITEMS-4)) {
        emit(g,VG_DENIED_SOUND,VG_AIR,g->feet); return;
    }
    g->inventory[VG_WOOD]-=wood; g->inventory[VG_STONE]-=stone;
    if (recipe==VG_CRAFT_TORCH) g->inventory[VG_TORCH]+=4;
    else g->tool_mask|=VG_TOOL_BIT(tool);
    emit(g,VG_CRAFT_SOUND,recipe==VG_CRAFT_TORCH ? VG_TORCH : VG_AIR,g->feet);
}
static float hardness(unsigned int block,unsigned int tool)
{
    if (block==VG_TORCH || block==VG_LEAVES || block==VG_WATER) return 0.30f;
    if (block==VG_STONE) return tool==VG_PICKAXE ? 0.65f : 2.4f;
    if (block==VG_WOOD) return tool==VG_AXE ? 0.85f : 1.7f;
    return tool==VG_SHOVEL ? 0.45f : 0.85f;
}
static void impact(vg_game *g,const int hit[3],unsigned int block)
{
    vg_vec3 at={(float)hit[0]+0.5f,(float)hit[1]+0.5f,(float)hit[2]+0.5f};
    unsigned int supported=vg_get_block(g,hit[0],hit[1]+1,hit[2]);
    if (hit[1]==0) { emit(g,VG_HIT_SOUND,block,at); return; }
    g->mining+=0.34f;
    g->mining_progress=g->game_mode==VG_CREATIVE ? 1 : bounded(g->mining/hardness(block,g->tool),0,1);
    if (g->mining_progress<1) { emit(g,VG_HIT_SOUND,block,at); return; }
    if (g->game_mode!=VG_CREATIVE && (g->inventory[block]>=VG_MAX_ITEMS || (supported==VG_TORCH && g->inventory[VG_TORCH]>=VG_MAX_ITEMS))) {
        emit(g,VG_DENIED_SOUND,block,at); g->mining=g->mining_progress=0; return;
    }
    if (vg_set_block(g,hit[0],hit[1],hit[2],VG_AIR)) {
        if (g->game_mode!=VG_CREATIVE) {
            ++g->inventory[block];
            if (supported==VG_TORCH) ++g->inventory[VG_TORCH];
        }
        emit(g,VG_BREAK_SOUND,block,at);
    }
    g->mining=g->mining_progress=0;
}
static void actions(vg_game *g,const vg_input *in,uint32_t pressed,float seconds)
{
    int hit[3],previous[3],target=ray(g,hit,previous);
    float remaining=bounded(seconds,0,0.25f);
    if (!(in->buttons & VG_BREAK) || !target) g->mining=g->mining_progress=0;
    if (target && (g->mining_cell[0]!=hit[0] || g->mining_cell[1]!=hit[1] || g->mining_cell[2]!=hit[2])) {
        g->mining=g->mining_progress=0; memcpy(g->mining_cell,hit,sizeof(hit));
    }
    if ((pressed & VG_PLACE) && !(in->buttons & VG_BREAK)) {
        if (target && (g->game_mode==VG_CREATIVE || g->inventory[g->selected]>0)) {
            if (vg_set_block(g,previous[0],previous[1],previous[2],g->selected)) {
                vg_vec3 at={(float)previous[0]+0.5f,(float)previous[1]+0.5f,(float)previous[2]+0.5f};
                if (g->game_mode!=VG_CREATIVE) --g->inventory[g->selected];
                emit(g,VG_PLACE_SOUND,g->selected,at);
                g->swing_active=1; g->swing_elapsed=0; g->placing=1;
            }
        } else if (target) emit(g,VG_DENIED_SOUND,g->selected,g->feet);
    }
    while (remaining>0) {
        float amount,old;
        if (!g->swing_active) {
            if (!(in->buttons & VG_BREAK)) break;
            g->swing_active=1; g->swing_elapsed=0; g->placing=0;
        }
        old=g->swing_elapsed;
        amount=remaining<0.34f-old ? remaining : 0.34f-old;
        g->swing_elapsed+=amount; remaining-=amount;
        if (!g->placing && old<0.14f && g->swing_elapsed>=0.14f) {
            if ((in->buttons & VG_BREAK) && ray(g,hit,previous)) {
                if (g->mining_cell[0]!=hit[0] || g->mining_cell[1]!=hit[1] || g->mining_cell[2]!=hit[2]) {
                    g->mining=g->mining_progress=0; memcpy(g->mining_cell,hit,sizeof(hit));
                }
                impact(g,hit,vg_get_block(g,hit[0],hit[1],hit[2]));
            } else emit(g,VG_SWING_SOUND,VG_AIR,g->feet);
        }
        if (g->swing_elapsed>=0.34f) { g->swing_active=0; g->swing_elapsed=0; }
    }
}
void vg_step(vg_game *g, const vg_input *in, float seconds)
{
    uint32_t pressed;
    float f,s,len;
    if (!g || !in) return;
    g->event_count=0;
    pressed=in->buttons & ~g->previous; g->previous=in->buttons;
    if (in->game_mode>=VG_SURVIVAL && in->game_mode<=VG_CREATIVE && in->game_mode!=g->game_mode) {
        g->game_mode=in->game_mode;
        if (g->game_mode==VG_SURVIVAL && !(g->tool_mask & VG_TOOL_BIT(g->tool))) g->tool=VG_HAND;
        g->mining=g->mining_progress=0; g->swing_active=0; g->swing_elapsed=0;
    }
    if (in->time_mode>=VG_TIME_CYCLE && in->time_mode<=VG_TIME_NIGHT) g->time_mode=in->time_mode;
    if ((pressed & VG_NEXT_TIME) && g->time_mode==VG_TIME_CYCLE) {
        if (g->day_time<0.26f) g->day_time=0.26f;
        else if (g->day_time<0.5f) g->day_time=0.5f;
        else if (g->day_time<0.74f) g->day_time=0.74f;
        else g->day_time=0;
    }
    if (g->time_mode==VG_TIME_DAY) g->day_time=0.5f;
    else if (g->time_mode==VG_TIME_NIGHT) g->day_time=0;
    if (g->game_mode!=VG_CREATIVE) craft(g,in->craft);
    if (in->selected_tool>=VG_HAND && in->selected_tool<VG_TOOL_TYPES && (g->game_mode==VG_CREATIVE || (g->tool_mask & VG_TOOL_BIT(in->selected_tool))) && in->selected_tool!=g->tool) {
        g->tool=in->selected_tool; g->mining=g->mining_progress=0; g->swing_active=0; g->swing_elapsed=0;
    }
    if (in->selected_block>VG_AIR && in->selected_block<VG_BLOCK_TYPES) g->selected=in->selected_block;
    if (in->paused) {
        g->previous=in->buttons; g->jump_pending=0; g->mining=g->mining_progress=0; g->swing_active=0; g->swing_elapsed=0;
        return;
    }
    if (g->time_mode==VG_TIME_CYCLE) g->day_time=fmodf(g->day_time+bounded(seconds,0,0.25f)/VG_DAY_SECONDS,1.0f);
    g->animation_time=fmodf(g->animation_time+bounded(seconds,0,0.25f),3600.0f);
    g->yaw=fmodf(g->yaw+bounded(in->look_yaw,-PI,PI),2.0f*PI);
    g->pitch=bounded(g->pitch+bounded(in->look_pitch,-PI,PI),-1.55f,1.55f);
    if (pressed & VG_JUMP) g->jump_pending=1;
    f=bounded(in->forward,-1,1); s=bounded(in->strafe,-1,1);
    len=sqrtf(f*f+s*s); if (len>1) { f/=len; s/=len; }
    g->accumulator+=bounded(seconds,0.0f,0.25f);
    while (g->accumulator>=1.0f/120.0f) {
        float dt=1.0f/120.0f;
        vg_vec3 before=g->feet;
        float falling=g->velocity_y;
        int was_grounded=g->grounded;
        int swimming=vg_get_block(g,(int)g->feet.x,(int)(g->feet.y+0.1f),(int)g->feet.z)==VG_WATER;
        float speed=swimming ? 2.6f : 5.0f;
        if (g->jump_pending && g->grounded) {
            g->velocity_y=7.5f;
            emit(g,VG_JUMP_SOUND,VG_AIR,g->feet);
        }
        g->jump_pending=0;
        g->grounded=0;
        if (swimming) {
            g->velocity_y+=(in->buttons & VG_JUMP ? 14.0f : -4.0f)*dt;
            g->velocity_y=bounded(g->velocity_y,-2.5f,3.2f);
        } else g->velocity_y-=22.0f*dt;
        if (g->velocity_y < -35.0f) g->velocity_y=-35.0f;
        move_axis(g,0,(s*cosf(g->yaw)+f*sinf(g->yaw))*speed*dt);
        move_axis(g,2,(s*sinf(g->yaw)-f*cosf(g->yaw))*speed*dt);
        move_axis(g,1,g->velocity_y*dt);
        if (g->grounded) {
            unsigned int floor_block=vg_get_block(g,(int)floorf(g->feet.x),(int)floorf(g->feet.y-0.01f),(int)floorf(g->feet.z));
            float dx=g->feet.x-before.x, dz=g->feet.z-before.z;
            if (!was_grounded && falling < -2.0f) emit(g,VG_LAND_SOUND,floor_block,g->feet);
            g->walk_distance+=sqrtf(dx*dx+dz*dz);
            if (g->walk_distance>=1.65f) { emit(g,VG_STEP_SOUND,floor_block,g->feet); g->walk_distance-=1.65f; }
        } else g->walk_distance=0;
        g->accumulator-=dt;
    }
    actions(g,in,pressed,seconds);
}
/* Faces point outward, counterclockwise when viewed from outside. */
static const int normals[6][3]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
static const int corners[6][4][3]={
    {{1,0,1},{1,0,0},{1,1,0},{1,1,1}},
    {{0,0,0},{0,0,1},{0,1,1},{0,1,0}},
    {{0,1,1},{1,1,1},{1,1,0},{0,1,0}},
    {{0,0,0},{1,0,0},{1,0,1},{0,0,1}},
    {{0,0,1},{1,0,1},{1,1,1},{0,1,1}},
    {{1,0,0},{0,0,0},{0,1,0},{1,1,0}}
};
/* Multi-source flood fill through non-solid voxels; recomputed only after edits. */
static void rebuild_light(vg_game *g)
{
    size_t i,head=0,tail=0;
    memset(g->light,0,sizeof(g->light));
    for (i=0;i<VG_BLOCK_COUNT;++i) if (g->blocks[i]==VG_TORCH) {
        g->light[i]=15; g->light_queue[tail++]=(uint32_t)i;
    }
    while (head<tail) {
        uint32_t at=g->light_queue[head++];
        int x=(int)(at%VG_WORLD_X), z=(int)((at/VG_WORLD_X)%VG_WORLD_Z), y=(int)(at/(VG_WORLD_X*VG_WORLD_Z));
        int side;
        if (g->light[at]<=1) continue;
        for (side=0;side<6;++side) {
            int nx=x+normals[side][0],ny=y+normals[side][1],nz=z+normals[side][2];
            size_t next;
            if (!inside(nx,ny,nz)) continue;
            next=index_of(nx,ny,nz);
            if (solid(g->blocks[next]) || g->light[next]) continue;
            g->light[next]=(unsigned char)(g->light[at]-1);
            g->light_queue[tail++]=(uint32_t)next;
        }
    }
    g->light_dirty=0;
}
static float illumination(const vg_game *g,int x,int y,int z)
{
    return inside(x,y,z) ? (float)g->light[index_of(x,y,z)]/15.0f : 0;
}
static void face(vg_game *g, int x, int y, int z, int side, unsigned int block)
{
    static const int order[6]={0,1,2,0,2,3};
    static const int uv[4][2]={{0,0},{1,0},{1,1},{0,1}};
    unsigned int tile=block-1;
    int i;
    /* Atlas: grass top, dirt, stone, wood side, leaves, sand, grass side, wood end. */
    if (block==VG_GRASS) tile=side==2 ? 0u : (side==3 ? 1u : 6u);
    if (block==VG_WATER) tile=8;
    if (block==VG_TORCH) tile=9;
    if (block==VG_WOOD && (side==2 || side==3)) tile=7;
    if (g->count+2>VG_MAX_TRIANGLES) { g->truncated=1; return; }
    for (i=0; i<6; ++i) {
        int k=order[i];
        vg_vertex *v=&g->mesh[g->count+(size_t)i/3].v[i%3];
        v->position.x=(float)(x+corners[side][k][0]);
        v->position.y=(float)(y+corners[side][k][1]);
        v->position.z=(float)(z+corners[side][k][2]);
        v->u=((float)(tile*VG_ATLAS_TILE_SIZE)+0.5f+(float)uv[k][0]*(VG_ATLAS_TILE_SIZE-1))/VG_ATLAS_WIDTH;
        v->v=(0.5f+(float)uv[k][1]*(VG_ATLAS_TILE_SIZE-1))/VG_ATLAS_HEIGHT;
        v->ambient=1.0f; v->block_light=0;
        if (block==VG_TORCH) {
            v->position.x=(float)x+0.43f+(float)corners[side][k][0]*0.14f;
            v->position.y=(float)y+(float)corners[side][k][1]*0.74f;
            v->position.z=(float)z+0.43f+(float)corners[side][k][2]*0.14f;
            v->block_light=1;
        } else {
            int a,axes[2],n=0,base[3]={x+normals[side][0],y+normals[side][1],z+normals[side][2]};
            int edge1[3],edge2[3],corner[3],s1,s2,sc;
            for (a=0;a<3;++a) if (!normals[side][a]) axes[n++]=a;
            memcpy(edge1,base,sizeof(base)); memcpy(edge2,base,sizeof(base)); memcpy(corner,base,sizeof(base));
            edge1[axes[0]]+=corners[side][k][axes[0]] ? 1 : -1;
            edge2[axes[1]]+=corners[side][k][axes[1]] ? 1 : -1;
            corner[axes[0]]=edge1[axes[0]]; corner[axes[1]]=edge2[axes[1]];
            s1=solid(vg_get_block(g,edge1[0],edge1[1],edge1[2])); s2=solid(vg_get_block(g,edge2[0],edge2[1],edge2[2]));
            sc=solid(vg_get_block(g,corner[0],corner[1],corner[2]));
            v->ambient=1.0f-0.13f*(float)(s1 && s2 ? 3 : s1+s2+sc);
            v->block_light=(illumination(g,base[0],base[1],base[2])+illumination(g,edge1[0],edge1[1],edge1[2])+illumination(g,edge2[0],edge2[1],edge2[2])+illumination(g,corner[0],corner[1],corner[2]))*0.25f;
            if (block==VG_WATER && corners[side][k][1] && vg_get_block(g,x,y+1,z)!=VG_WATER) v->position.y-=0.10f;
        }
        g->mesh[g->count+(size_t)i/3].material=block;
    }
    g->count+=2;
}
void vg_build_frame(vg_game *g, vg_frame *out)
{
    int x,y,z,s,ring,h[3],p[3];
    int cx,cz;
    if (!g || !out) return;
    if (g->light_dirty) rebuild_light(g);
    g->count=0; g->truncated=0; g->light_count=0; g->lights_truncated=0;
    cx=(int)g->feet.x; cz=(int)g->feet.z;
    /* Near-to-far rings keep nearby geometry if a pathological world hits the cap. */
    for (ring=0; ring<=DRAW_DISTANCE; ++ring)
        for (z=cz-ring; z<=cz+ring; ++z) for (x=cx-ring; x<=cx+ring; ++x) {
            if (abs(x-cx)!=ring && abs(z-cz)!=ring) continue;
            if (x<0 || z<0 || x>=VG_WORLD_X || z>=VG_WORLD_Z) continue;
            for (y=0; y<VG_WORLD_Y; ++y) {
                unsigned int b=vg_get_block(g,x,y,z);
                if (!b) continue;
                if (b==VG_TORCH) {
                    if (g->light_count<VG_MAX_LIGHTS) {
                        vg_vec3 at={(float)x+0.5f,(float)y+0.78f,(float)z+0.5f};
                        g->lights[g->light_count++]=at;
                    } else g->lights_truncated=1;
                }
                for (s=0; s<6; ++s) {
                    if (s==3 && y==0) continue;
                    unsigned int neighbor=vg_get_block(g,x+normals[s][0],y+normals[s][1],z+normals[s][2]);
                    if (b==VG_TORCH || (b==VG_WATER ? neighbor==VG_AIR || neighbor==VG_TORCH : !solid(neighbor))) face(g,x,y,z,s,b);
                }
            }
        }
    memset(out,0,sizeof(*out));
    out->triangles=g->mesh; out->triangle_count=g->count; out->camera=camera(g);
    out->time_of_day=g->day_time; out->animation_time=g->animation_time;
    out->lights=g->lights; out->light_count=g->light_count; out->lights_truncated=g->lights_truncated;
    out->underwater=vg_get_block(g,(int)out->camera.position.x,(int)out->camera.position.y,(int)out->camera.position.z)==VG_WATER;
    memcpy(out->inventory,g->inventory,sizeof(g->inventory)); out->tool_mask=g->game_mode==VG_CREATIVE ? ((1u<<VG_TOOL_TYPES)-2u) : g->tool_mask;
    out->game_mode=g->game_mode; out->time_mode=g->time_mode;
    out->swing_active=g->swing_active; out->swing_progress=g->swing_elapsed/0.34f; out->placing=g->placing;
    out->selected_tool=g->tool; out->mining_progress=g->mining_progress;
    out->events=g->events; out->event_count=g->event_count;
    out->selected_block=g->selected; out->mesh_truncated=g->truncated;
    out->target_valid=ray(g,h,p);
    if (out->target_valid) { out->target_x=h[0]; out->target_y=h[1]; out->target_z=h[2]; }
}
static void put_u32(unsigned char *p,uint32_t value)
{
    int i; for (i=0;i<4;++i) p[i]=(unsigned char)(value>>(i*8));
}
static uint32_t get_u32(const unsigned char *p)
{
    int i; uint32_t value=0; for (i=0;i<4;++i) value|=(uint32_t)p[i]<<(i*8); return value;
}
int vg_export_world(const vg_game *g,void *bytes,size_t size)
{
    unsigned char *b=(unsigned char *)bytes;
    static const unsigned char header[12]={'V','G','0','4',VG_WORLD_X,VG_WORLD_Y,VG_WORLD_Z,0,0,0,0,0};
    uint32_t clock;
    unsigned int i;
    if (!g || !b || size!=VG_SAVE_BYTES) return 0;
    clock=(uint32_t)(g->day_time*1000000.0f+0.5f); if (clock>=1000000u) clock=999999u;
    memcpy(b,header,12); b[7]=(unsigned char)((g->game_mode-1u)+2u*(g->time_mode-1u)); put_u32(b+8,clock); memcpy(b+12,g->blocks,VG_BLOCK_COUNT);
    for (i=0;i<VG_BLOCK_TYPES;++i) put_u32(b+VG_LEGACY_SAVE_BYTES+i*4,g->inventory[i]);
    put_u32(b+VG_LEGACY_SAVE_BYTES+VG_BLOCK_TYPES*4,g->tool_mask);
    return 1;
}
int vg_import_world(vg_game *g,const void *bytes,size_t size)
{
    const unsigned char *b=(const unsigned char *)bytes;
    uint32_t clock,inventory[VG_BLOCK_TYPES]={0},tools=VG_TOOL_BIT(VG_HAND);
    size_t i;
    if (!g || !b || (size!=VG_SAVE_BYTES && size!=VG_LEGACY_SAVE_BYTES)) return 0;
    if (b[0]!='V' || b[1]!='G' || b[2]!='0' || (b[3]!='1' && b[3]!='2' && b[3]!='3' && b[3]!='4') ||
        b[4]!=VG_WORLD_X || b[5]!=VG_WORLD_Y || b[6]!=VG_WORLD_Z || (b[3]=='4' ? b[7]>5 : b[7]!=0)) return 0;
    if ((b[3]>='3')!=(size==VG_SAVE_BYTES)) return 0;
    clock=get_u32(b+8); if (clock>=1000000u || (b[3]=='1' && clock)) return 0;
    if (b[3]>='3') {
        for (i=0;i<VG_BLOCK_TYPES;++i) { inventory[i]=get_u32(b+VG_LEGACY_SAVE_BYTES+i*4); if (inventory[i]>VG_MAX_ITEMS) return 0; }
        tools=get_u32(b+VG_LEGACY_SAVE_BYTES+VG_BLOCK_TYPES*4);
        if (inventory[VG_AIR] || !(tools & VG_TOOL_BIT(VG_HAND)) || (tools & ~((1u<<VG_TOOL_TYPES)-2u))) return 0;
    }
    for (i=0;i<VG_BLOCK_COUNT;++i) {
        if (b[12+i]>=VG_BLOCK_TYPES) return 0;
        if (b[12+i]==VG_TORCH && (i<VG_WORLD_X*VG_WORLD_Z || !solid(b[12+i-VG_WORLD_X*VG_WORLD_Z]))) return 0;
    }
    for (i=0;i<VG_WORLD_X*VG_WORLD_Z;++i) if (!solid(b[12+i])) return 0;
    memcpy(g->blocks,b+12,VG_BLOCK_COUNT); memcpy(g->inventory,inventory,sizeof(inventory)); g->tool_mask=tools; g->tool=VG_HAND;
    g->game_mode=b[3]=='4' ? 1u+(b[7]&1u) : VG_SURVIVAL;
    g->time_mode=b[3]=='4' ? 1u+(b[7]>>1) : VG_TIME_CYCLE;
    g->day_time=b[3]=='1' ? 0.36f : (float)clock/1000000.0f;
    spawn(g); return 1;
}
