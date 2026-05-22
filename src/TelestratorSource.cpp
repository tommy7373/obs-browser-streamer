#include "TelestratorSource.hpp"
#include "Telestration.hpp"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>

#include <cmath>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Provided by plugin-main.cpp — returns the singleton plugin's telestrator
// state, or nullptr if the plugin hasn't initialised yet.
extern "C" TelestrationState* webpreview_get_telestration_state();

namespace {

// Geometry tessellation for round caps + filled circles.
static constexpr uint32_t kCapSegments    = 12;
static constexpr uint32_t kCircleSegments = 24;

struct CachedStroke {
    gs_vertbuffer_t* vb       = nullptr;
    uint32_t         numVerts = 0;
    struct vec4      color    = {};
};

struct TelestratorInstance {
    obs_source_t*     source         = nullptr;
    uint32_t          width          = 1920;
    uint32_t          height         = 1080;

    std::vector<CachedStroke> cache;
    uint64_t          cachedVersion  = static_cast<uint64_t>(-1);
};

// --------------------------------------------------------------------------
// Vertex assembly. The OBS "Solid" effect technique uses a uniform "color"
// parameter and ignores vertex colors, so we keep one vertex buffer per
// stroke and set the color uniform between draws.
// --------------------------------------------------------------------------

static inline void RgbaToVec4(uint32_t rgba, struct vec4& out)
{
    out.x = ((rgba >> 24) & 0xFF) / 255.f;
    out.y = ((rgba >> 16) & 0xFF) / 255.f;
    out.z = ((rgba >>  8) & 0xFF) / 255.f;
    out.w = ((rgba      ) & 0xFF) / 255.f;
}

// Emit 2 triangles (6 verts) for one thick segment from p0→p1.
static void EmitSegment(uint32_t& count,
                        float x0, float y0, float x1, float y1, float halfW)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-4f) return;
    float nx = -dy / len * halfW;
    float ny =  dx / len * halfW;

    gs_vertex2f(x0 + nx, y0 + ny);
    gs_vertex2f(x0 - nx, y0 - ny);
    gs_vertex2f(x1 - nx, y1 - ny);

    gs_vertex2f(x0 + nx, y0 + ny);
    gs_vertex2f(x1 - nx, y1 - ny);
    gs_vertex2f(x1 + nx, y1 + ny);
    count += 6;
}

// Filled circle (triangle fan) — used as the 1-point freehand "dot".
static void EmitFilledCircle(uint32_t& count,
                             float cx, float cy, float r, uint32_t segments)
{
    if (r <= 0.f || segments < 3) return;
    const float dAng = static_cast<float>(2.0 * M_PI) / segments;
    float prevX = cx + r;
    float prevY = cy;
    for (uint32_t i = 1; i <= segments; ++i) {
        float a = i * dAng;
        float nx = cx + std::cos(a) * r;
        float ny = cy + std::sin(a) * r;
        gs_vertex2f(cx, cy);
        gs_vertex2f(prevX, prevY);
        gs_vertex2f(nx, ny);
        prevX = nx; prevY = ny;
        count += 3;
    }
}

// Round end-cap: half-circle centered at (cx,cy), rounded "outward" along
// (ux,uy) (a unit vector). The half-circle spans from the perpendicular-left
// edge of the line, through the outward tip, to the perpendicular-right edge.
static void EmitRoundCap(uint32_t& count,
                         float cx, float cy, float ux, float uy,
                         float r, uint32_t segments)
{
    if (r <= 0.f || segments < 2) return;
    const float startAng = std::atan2(ux, -uy);
    const float dAng = static_cast<float>(M_PI) / segments;
    float prevX = cx + std::cos(startAng) * r;
    float prevY = cy + std::sin(startAng) * r;
    for (uint32_t i = 1; i <= segments; ++i) {
        float a = startAng + i * dAng;
        float nx = cx + std::cos(a) * r;
        float ny = cy + std::sin(a) * r;
        gs_vertex2f(cx, cy);
        gs_vertex2f(prevX, prevY);
        gs_vertex2f(nx, ny);
        prevX = nx; prevY = ny;
        count += 3;
    }
}

static void EmitArrowHead(uint32_t& count,
                          float x1, float y1, float ux, float uy,
                          float baseLen, float baseHalf)
{
    float bx = x1 - ux * baseLen;
    float by = y1 - uy * baseLen;
    float px = -uy, py = ux;
    gs_vertex2f(x1, y1);
    gs_vertex2f(bx + px * baseHalf, by + py * baseHalf);
    gs_vertex2f(bx - px * baseHalf, by - py * baseHalf);
    count += 3;
}

// Thick polyline with mitered interior joints. Endpoints are returned to the
// caller via out{First,Last}{Dir,Pos} so it can add round caps.
static void EmitPolylineMitered(
    uint32_t& count,
    const std::vector<std::pair<float, float>>& pts,
    float w, float h, float halfW,
    float& firstX, float& firstY, float& firstDirX, float& firstDirY,
    float& lastX,  float& lastY,  float& lastDirX,  float& lastDirY)
{
    const size_t N = pts.size();
    if (N < 2) return;

    std::vector<float> Lx(N), Ly(N), Rx(N), Ry(N);
    const float MITER_LIMIT = halfW * 4.f;

    {
        float dx0 = (pts[1].first  - pts[0].first ) * w;
        float dy0 = (pts[1].second - pts[0].second) * h;
        float l0  = std::sqrt(dx0 * dx0 + dy0 * dy0);
        firstX = pts[0].first  * w;
        firstY = pts[0].second * h;
        if (l0 > 1e-4f) { firstDirX = -dx0 / l0; firstDirY = -dy0 / l0; }
        else            { firstDirX = -1.f; firstDirY = 0.f; }

        float dxN = (pts[N - 1].first  - pts[N - 2].first ) * w;
        float dyN = (pts[N - 1].second - pts[N - 2].second) * h;
        float lN  = std::sqrt(dxN * dxN + dyN * dyN);
        lastX = pts[N - 1].first  * w;
        lastY = pts[N - 1].second * h;
        if (lN > 1e-4f) { lastDirX = dxN / lN; lastDirY = dyN / lN; }
        else            { lastDirX = 1.f; lastDirY = 0.f; }
    }

    for (size_t i = 0; i < N; ++i) {
        const float x = pts[i].first  * w;
        const float y = pts[i].second * h;

        float perpX = 0.f, perpY = 0.f;

        if (i == 0 || i == N - 1) {
            const size_t a = (i == 0) ? 0     : N - 2;
            const size_t b = (i == 0) ? 1     : N - 1;
            float dx = (pts[b].first  - pts[a].first ) * w;
            float dy = (pts[b].second - pts[a].second) * h;
            float len = std::sqrt(dx * dx + dy * dy);
            if (len < 1e-4f) { perpX = halfW; perpY = 0.f; }
            else            { perpX = -dy / len * halfW; perpY = dx / len * halfW; }
        } else {
            float d1x = (pts[i].first  - pts[i - 1].first ) * w;
            float d1y = (pts[i].second - pts[i - 1].second) * h;
            float l1  = std::sqrt(d1x * d1x + d1y * d1y);
            float d2x = (pts[i + 1].first  - pts[i].first ) * w;
            float d2y = (pts[i + 1].second - pts[i].second) * h;
            float l2  = std::sqrt(d2x * d2x + d2y * d2y);

            if (l1 < 1e-4f || l2 < 1e-4f) {
                float dx = (l1 > l2) ? d1x : d2x;
                float dy = (l1 > l2) ? d1y : d2y;
                float ll = std::max(l1, l2);
                perpX = -dy / ll * halfW;
                perpY =  dx / ll * halfW;
            } else {
                float p1x = -d1y / l1, p1y = d1x / l1;
                float p2x = -d2y / l2, p2y = d2x / l2;
                float mx = p1x + p2x, my = p1y + p2y;
                float ml = std::sqrt(mx * mx + my * my);
                if (ml < 1e-3f) {
                    perpX = p1x * halfW;
                    perpY = p1y * halfW;
                } else {
                    mx /= ml; my /= ml;
                    float dot = mx * p1x + my * p1y;
                    if (std::fabs(dot) < 1e-3f) {
                        perpX = p1x * halfW;
                        perpY = p1y * halfW;
                    } else {
                        float factor = halfW / dot;
                        if (factor >  MITER_LIMIT) factor =  MITER_LIMIT;
                        if (factor < -MITER_LIMIT) factor = -MITER_LIMIT;
                        perpX = mx * factor;
                        perpY = my * factor;
                    }
                }
            }
        }

        Lx[i] = x + perpX; Ly[i] = y + perpY;
        Rx[i] = x - perpX; Ry[i] = y - perpY;
    }

    for (size_t i = 0; i + 1 < N; ++i) {
        gs_vertex2f(Lx[i],     Ly[i]);
        gs_vertex2f(Rx[i],     Ry[i]);
        gs_vertex2f(Rx[i + 1], Ry[i + 1]);

        gs_vertex2f(Lx[i],     Ly[i]);
        gs_vertex2f(Rx[i + 1], Ry[i + 1]);
        gs_vertex2f(Lx[i + 1], Ly[i + 1]);
        count += 6;
    }
}

// Build a single-stroke vertex buffer. Returns nullptr if the stroke
// contributes no geometry.
static gs_vertbuffer_t* BuildStrokeVB(const Stroke& s, uint32_t width,
                                      uint32_t height, uint32_t& outNumVerts)
{
    outNumVerts = 0;

    const float fw = static_cast<float>(width);
    const float fh = static_cast<float>(height);
    const float halfW = std::max(0.5f, s.width * fh * 0.5f);

    gs_render_start(true);

    if (s.type == Stroke::Arrow && s.points.size() >= 2) {
        const float x0 = s.points.front().first  * fw;
        const float y0 = s.points.front().second * fh;
        const float x1 = s.points.back().first   * fw;
        const float y1 = s.points.back().second  * fh;
        const float baseLen  = halfW * 6.f;
        const float baseHalf = halfW * 3.f;
        float dx = x1 - x0, dy = y1 - y0;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len > 1e-4f) {
            float ux = dx / len, uy = dy / len;
            // Rounded back-end cap, then shaft, then arrowhead. The head is
            // intentionally pointy — no cap at that end.
            EmitRoundCap(outNumVerts, x0, y0, -ux, -uy, halfW, kCapSegments);
            if (len > baseLen)
                EmitSegment(outNumVerts, x0, y0,
                            x1 - ux * baseLen, y1 - uy * baseLen, halfW);
            EmitArrowHead(outNumVerts, x1, y1, ux, uy, baseLen, baseHalf);
        }
    } else if (s.points.size() == 1) {
        EmitFilledCircle(outNumVerts,
                         s.points[0].first * fw, s.points[0].second * fh,
                         halfW, kCircleSegments);
    } else if (s.points.size() >= 2) {
        float fx, fy, fdx, fdy, lx, ly, ldx, ldy;
        EmitPolylineMitered(outNumVerts, s.points, fw, fh, halfW,
                            fx, fy, fdx, fdy, lx, ly, ldx, ldy);
        EmitRoundCap(outNumVerts, fx, fy, fdx, fdy, halfW, kCapSegments);
        EmitRoundCap(outNumVerts, lx, ly, ldx, ldy, halfW, kCapSegments);
    }

    if (outNumVerts == 0) {
        gs_vertbuffer_t* empty = gs_render_save();
        if (empty) gs_vertexbuffer_destroy(empty);
        return nullptr;
    }
    return gs_render_save();
}

// --------------------------------------------------------------------------
// OBS source vtable
// --------------------------------------------------------------------------

static const char* telestrator_get_name(void*)
{
    return obs_module_text("Telestrator.SourceName");
}

static void telestrator_destroy_cache(TelestratorInstance* t)
{
    if (t->cache.empty()) return;
    obs_enter_graphics();
    for (auto& c : t->cache)
        if (c.vb) gs_vertexbuffer_destroy(c.vb);
    obs_leave_graphics();
    t->cache.clear();
}

static void telestrator_update(void* data, obs_data_t* settings)
{
    auto* t = static_cast<TelestratorInstance*>(data);
    int w = static_cast<int>(obs_data_get_int(settings, "width"));
    int h = static_cast<int>(obs_data_get_int(settings, "height"));
    if (w <= 0 || h <= 0) {
        obs_video_info ovi = {};
        if (obs_get_video_info(&ovi)) {
            if (w <= 0) w = static_cast<int>(ovi.base_width);
            if (h <= 0) h = static_cast<int>(ovi.base_height);
        } else {
            if (w <= 0) w = 1920;
            if (h <= 0) h = 1080;
        }
    }
    t->width  = static_cast<uint32_t>(w);
    t->height = static_cast<uint32_t>(h);
    // Force rebuild against new dimensions on next render.
    t->cachedVersion = static_cast<uint64_t>(-1);
}

static void* telestrator_create(obs_data_t* settings, obs_source_t* source)
{
    auto* t = new TelestratorInstance();
    t->source = source;
    telestrator_update(t, settings);
    return t;
}

static void telestrator_destroy(void* data)
{
    auto* t = static_cast<TelestratorInstance*>(data);
    telestrator_destroy_cache(t);
    delete t;
}

static uint32_t telestrator_get_width(void* data)
{
    return static_cast<TelestratorInstance*>(data)->width;
}

static uint32_t telestrator_get_height(void* data)
{
    return static_cast<TelestratorInstance*>(data)->height;
}

static void telestrator_get_defaults(obs_data_t* settings)
{
    obs_video_info ovi = {};
    if (obs_get_video_info(&ovi)) {
        obs_data_set_default_int(settings, "width",  ovi.base_width);
        obs_data_set_default_int(settings, "height", ovi.base_height);
    } else {
        obs_data_set_default_int(settings, "width",  1920);
        obs_data_set_default_int(settings, "height", 1080);
    }
}

static obs_properties_t* telestrator_get_properties(void*)
{
    obs_properties_t* props = obs_properties_create();
    obs_properties_add_int(props, "width",  obs_module_text("Telestrator.PropWidth"),
                           16, 16384, 1);
    obs_properties_add_int(props, "height", obs_module_text("Telestrator.PropHeight"),
                           16, 16384, 1);
    return props;
}

static void telestrator_render(void* data, gs_effect_t*)
{
    auto* t = static_cast<TelestratorInstance*>(data);
    TelestrationState* state = webpreview_get_telestration_state();
    if (!state) return;

    const uint64_t v = state->Version();
    if (v != t->cachedVersion) {
        // State changed — tear down per-stroke buffers and rebuild from the
        // current snapshot.
        for (auto& c : t->cache)
            if (c.vb) gs_vertexbuffer_destroy(c.vb);
        t->cache.clear();

        auto strokes = state->Snapshot();
        t->cache.reserve(strokes.size());
        for (const auto& s : strokes) {
            uint32_t nv = 0;
            gs_vertbuffer_t* vb = BuildStrokeVB(s, t->width, t->height, nv);
            if (!vb || nv == 0) continue;
            CachedStroke cs;
            cs.vb = vb;
            cs.numVerts = nv;
            RgbaToVec4(s.colorRgba, cs.color);
            t->cache.push_back(cs);
        }
        t->cachedVersion = v;
    }

    if (t->cache.empty()) return;

    gs_effect_t* solid = obs_get_base_effect(OBS_EFFECT_SOLID);
    gs_eparam_t* colorParam = gs_effect_get_param_by_name(solid, "color");

    gs_load_indexbuffer(nullptr);
    for (const auto& c : t->cache) {
        if (!c.vb || c.numVerts == 0) continue;
        gs_effect_set_vec4(colorParam, &c.color);
        gs_load_vertexbuffer(c.vb);
        while (gs_effect_loop(solid, "Solid"))
            gs_draw(GS_TRIS, 0, c.numVerts);
    }
}

static struct obs_source_info kTelestratorSource = {};

} // namespace

void TelestratorSource::RegisterSourceType()
{
    kTelestratorSource.id              = "obs_web_preview_telestrator";
    kTelestratorSource.type            = OBS_SOURCE_TYPE_INPUT;
    kTelestratorSource.output_flags    = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
    kTelestratorSource.get_name        = telestrator_get_name;
    kTelestratorSource.create          = telestrator_create;
    kTelestratorSource.destroy         = telestrator_destroy;
    kTelestratorSource.update          = telestrator_update;
    kTelestratorSource.get_width       = telestrator_get_width;
    kTelestratorSource.get_height      = telestrator_get_height;
    kTelestratorSource.get_defaults    = telestrator_get_defaults;
    kTelestratorSource.get_properties  = telestrator_get_properties;
    kTelestratorSource.video_render    = telestrator_render;
    obs_register_source(&kTelestratorSource);
}
