#include "Telestration.hpp"

#include <obs-module.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// TelestrationState
// ---------------------------------------------------------------------------

void TelestrationState::Add(Stroke s)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Upsert by id so a controller's in-flight stroke (sent repeatedly
        // during pointermove with the same id) updates in-place instead of
        // accumulating duplicates.
        auto it = std::find_if(strokes_.begin(), strokes_.end(),
                               [&](const Stroke& e) { return e.id == s.id; });
        if (it != strokes_.end())
            *it = std::move(s);
        else
            strokes_.push_back(std::move(s));
    }
    version_.fetch_add(1, std::memory_order_release);
}

bool TelestrationState::UndoById(int id)
{
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find_if(strokes_.begin(), strokes_.end(),
                               [id](const Stroke& s) { return s.id == id; });
        if (it != strokes_.end()) {
            strokes_.erase(it);
            removed = true;
        }
    }
    if (removed) version_.fetch_add(1, std::memory_order_release);
    return removed;
}

bool TelestrationState::UndoLast()
{
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!strokes_.empty()) {
            strokes_.pop_back();
            removed = true;
        }
    }
    if (removed) version_.fetch_add(1, std::memory_order_release);
    return removed;
}

void TelestrationState::Clear()
{
    bool wasEmpty;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wasEmpty = strokes_.empty();
        strokes_.clear();
    }
    if (!wasEmpty) version_.fetch_add(1, std::memory_order_release);
}

std::vector<Stroke> TelestrationState::Snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return strokes_;
}

bool TelestrationState::Empty() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return strokes_.empty();
}

// ---------------------------------------------------------------------------
// Wire-protocol helpers — built on libobs's obs_data JSON facilities so we
// don't pull in a separate JSON dependency.
// ---------------------------------------------------------------------------

// Parses "#RRGGBB" or "#RRGGBBAA" (with or without leading '#') into RGBA8.
// Alpha defaults to 0xFF when only 6 hex digits are present.
static uint32_t ParseColor(const char* s)
{
    if (!s) return 0xFFFFFFFFu;
    if (s[0] == '#') ++s;
    unsigned r = 0, g = 0, b = 0, a = 0xFF;
    size_t len = std::strlen(s);
    if (len == 6 && std::sscanf(s, "%2x%2x%2x", &r, &g, &b) == 3) {
        // ok
    } else if (len == 8 && std::sscanf(s, "%2x%2x%2x%2x", &r, &g, &b, &a) == 4) {
        // ok
    } else {
        return 0xFFFFFFFFu;
    }
    return (uint32_t(r) << 24) | (uint32_t(g) << 16) | (uint32_t(b) << 8) | uint32_t(a);
}

static std::string FormatColor(uint32_t rgba)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x",
                  (rgba >> 24) & 0xFF,
                  (rgba >> 16) & 0xFF,
                  (rgba >>  8) & 0xFF,
                  (rgba      ) & 0xFF);
    return buf;
}

static bool ParseStrokeData(obs_data_t* data, Stroke& out)
{
    out.id = static_cast<int>(obs_data_get_int(data, "id"));
    const char* type = obs_data_get_string(data, "type");
    out.type = (type && std::strcmp(type, "arrow") == 0) ? Stroke::Arrow : Stroke::Freehand;
    out.colorRgba = ParseColor(obs_data_get_string(data, "color"));
    double w = obs_data_get_double(data, "width");
    if (w <= 0.0) w = 0.004;
    out.width = static_cast<float>(w);

    obs_data_array_t* pts = obs_data_get_array(data, "points");
    if (!pts) return false;
    size_t n = obs_data_array_count(pts);
    out.points.clear();
    out.points.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        obs_data_t* pair = obs_data_array_item(pts, i);
        if (!pair) continue;
        // We expect a 2-element array; encoded as "x"/"y" object for ease.
        // The controller serialises {"x":..,"y":..} per point.
        double x = obs_data_get_double(pair, "x");
        double y = obs_data_get_double(pair, "y");
        out.points.emplace_back(static_cast<float>(x), static_cast<float>(y));
        obs_data_release(pair);
    }
    obs_data_array_release(pts);
    return !out.points.empty();
}

bool ParseTelestrateMessage(const std::string& json, TelestrateMessage& out)
{
    obs_data_t* data = obs_data_create_from_json(json.c_str());
    if (!data) return false;
    const char* op = obs_data_get_string(data, "op");
    if (!op) { obs_data_release(data); return false; }

    bool ok = false;
    if (std::strcmp(op, "stroke") == 0) {
        out.op = TelestrateOp::Stroke_;
        ok = ParseStrokeData(data, out.stroke);
    } else if (std::strcmp(op, "undo") == 0) {
        out.op = TelestrateOp::Undo;
        out.undoId = static_cast<int>(obs_data_get_int(data, "id"));
        ok = true;
    } else if (std::strcmp(op, "clear") == 0) {
        out.op = TelestrateOp::Clear;
        ok = true;
    } else if (std::strcmp(op, "snapshot") == 0) {
        out.op = TelestrateOp::Snapshot;
        obs_data_array_t* arr = obs_data_get_array(data, "strokes");
        if (arr) {
            size_t n = obs_data_array_count(arr);
            out.snapshot.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                obs_data_t* item = obs_data_array_item(arr, i);
                if (item) {
                    Stroke s;
                    if (ParseStrokeData(item, s)) out.snapshot.push_back(std::move(s));
                    obs_data_release(item);
                }
            }
            obs_data_array_release(arr);
        }
        ok = true;
    }

    obs_data_release(data);
    return ok;
}

static void StrokeToData(const Stroke& s, obs_data_t* data)
{
    obs_data_set_int   (data, "id",    s.id);
    obs_data_set_string(data, "type",  s.type == Stroke::Arrow ? "arrow" : "freehand");
    obs_data_set_string(data, "color", FormatColor(s.colorRgba).c_str());
    obs_data_set_double(data, "width", s.width);

    obs_data_array_t* arr = obs_data_array_create();
    for (auto& p : s.points) {
        obs_data_t* pt = obs_data_create();
        obs_data_set_double(pt, "x", p.first);
        obs_data_set_double(pt, "y", p.second);
        obs_data_array_push_back(arr, pt);
        obs_data_release(pt);
    }
    obs_data_set_array(data, "points", arr);
    obs_data_array_release(arr);
}

std::string SerializeStrokeMessage(const Stroke& s)
{
    obs_data_t* data = obs_data_create();
    obs_data_set_string(data, "op", "stroke");
    StrokeToData(s, data);
    const char* j = obs_data_get_json(data);
    std::string out = j ? j : "{}";
    obs_data_release(data);
    return out;
}

std::string SerializeSnapshotMessage(const std::vector<Stroke>& strokes)
{
    obs_data_t* data = obs_data_create();
    obs_data_set_string(data, "op", "snapshot");
    obs_data_array_t* arr = obs_data_array_create();
    for (const auto& s : strokes) {
        obs_data_t* item = obs_data_create();
        StrokeToData(s, item);
        obs_data_array_push_back(arr, item);
        obs_data_release(item);
    }
    obs_data_set_array(data, "strokes", arr);
    obs_data_array_release(arr);
    const char* j = obs_data_get_json(data);
    std::string out = j ? j : "{}";
    obs_data_release(data);
    return out;
}
