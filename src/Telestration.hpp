#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Single drawing stroke. All coordinates are normalised to 0..1 of the
// controller's intrinsic video size, so the OBS source can scale them to
// any width/height without knowing the controller's canvas resolution.
struct Stroke {
    enum Type : int { Freehand = 0, Arrow = 1 };

    int      id = 0;
    int      type = Freehand;
    uint32_t colorRgba = 0xFFFFFFFFu;   // 0xRRGGBBAA
    float    width = 0.004f;            // line width, also normalised (0..1 of canvas height)
    std::vector<std::pair<float, float>> points;
};

// Thread-safe stroke list shared between the WebRTC DataChannel handlers
// (writers — server thread) and the OBS source's video_render (reader —
// graphics thread). The version counter lets the source cache GPU resources
// and rebuild only when the stroke list mutates.
class TelestrationState {
public:
    void Add(Stroke s);
    bool UndoById(int id);
    bool UndoLast();
    void Clear();

    std::vector<Stroke> Snapshot() const;
    uint64_t Version() const { return version_.load(std::memory_order_acquire); }
    bool Empty() const;

private:
    mutable std::mutex      mutex_;
    std::vector<Stroke>     strokes_;
    std::atomic<uint64_t>   version_{0};
};

// Wire protocol — single JSON object per DataChannel message.
enum class TelestrateOp { Unknown, Stroke_, Undo, Clear, Snapshot };

struct TelestrateMessage {
    TelestrateOp        op = TelestrateOp::Unknown;
    Stroke              stroke;            // valid when op == Stroke_
    int                 undoId = 0;        // valid when op == Undo
    std::vector<Stroke> snapshot;          // valid when op == Snapshot
};

bool ParseTelestrateMessage(const std::string& json, TelestrateMessage& out);
std::string SerializeStrokeMessage(const Stroke& s);
std::string SerializeSnapshotMessage(const std::vector<Stroke>& strokes);
