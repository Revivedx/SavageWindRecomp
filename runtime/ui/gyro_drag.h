// The drag at a shrine apparatus (runtime/pc_gyro_scene.c), run from runtime/ui/menu.mm. Left drag rolls an
// arcball centred on the click. Right drag banks and tilts in the table's axes. Releasing keeps the pose.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstring>

#include "imgui.h"
#include "../motion.h"
#include "../pc_gyro_scene.h"

// The point on a ball of `radius` around `center` under the mouse (past its edge: on its rim)
static void gyro_ball_vector(ImVec2 mouse, ImVec2 center, float radius, float out[3]) {
    out[0] = (mouse.x - center.x) / radius;
    out[1] = (center.y - mouse.y) / radius;
    float d = out[0] * out[0] + out[1] * out[1];
    out[2] = d <= 1 ? sqrtf(1 - d) : 0;
    if (d > 1) {
        float k = 1 / sqrtf(d);
        out[0] *= k, out[1] *= k;
    }
}

// The rotation taking `from` to `to` (unit vectors) as a quaternion w, x, y, z; false when they are the same
static bool gyro_ball_delta(const float from[3], const float to[3], float q[4]) {
    float dot = std::clamp(from[0] * to[0] + from[1] * to[1] + from[2] * to[2], -1.f, 1.f);
    if (dot > 1 - 1e-7f) return false;
    q[0] = 1 + dot;
    q[1] = from[1] * to[2] - from[2] * to[1];
    q[2] = from[2] * to[0] - from[0] * to[2];
    q[3] = from[0] * to[1] - from[1] * to[0];
    float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n < 1e-6f) {  // (opposite: a half turn about any axis across them)
        q[0] = 0, q[1] = 0, q[2] = -from[2], q[3] = from[1];
        n = hypotf(q[2], q[3]);
        if (n < 1e-6f) {
            q[1] = -from[1], q[2] = from[0], q[3] = 0;
            n = hypotf(q[1], q[2]);
        }
    }
    for (int i = 0; i < 4; i++) q[i] /= n;
    return true;
}

static void gyro_scene_draw(ImGuiIO &io, const GyroScene &scene, bool enabled) {
    static int button = -1;
    static unsigned generation;
    static ImVec2 origin;
    static float radius, previous[3];
    if (!enabled || !scene.active || generation != scene.generation ||
        (button >= 0 && !ImGui::IsMouseDown(button))) button = -1;
    generation = scene.generation;
    if (!enabled || !scene.active || !ImGui::IsMousePosValid()) return;

    float w = io.DisplaySize.x, h = io.DisplaySize.y;
    bool inside = io.MousePos.x >= 0 && io.MousePos.x <= w && io.MousePos.y >= 0 && io.MousePos.y <= h;
    bool left = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    bool right = ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    if (inside && (left || right)) {
        button = right ? ImGuiMouseButton_Right : ImGuiMouseButton_Left;
        origin = io.MousePos;
        radius = fmaxf(1.f, fminf(w, h) * .38f);
        gyro_ball_vector(io.MousePos, origin, radius, previous);
        return;
    }

    if (button == ImGuiMouseButton_Left) {
        float v[3], q[4];
        gyro_ball_vector(io.MousePos, origin, radius, v);
        if (gyro_ball_delta(previous, v, q)) motion_gizmo_arcball(q[0], q[1], -q[3], q[2]);  // (screen -> table axes)
        memcpy(previous, v, sizeof v);
    } else if (button == ImGuiMouseButton_Right) {
        // Compose a pitch about table X and a bank about table Y. The radius gives the same scale as a small arcball drag.
        if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) {
            // Keep even a full-window flick in one frame below a half-turn, which could wrap the pose backward.
            float scale = fmaxf(2 * radius, hypotf(io.MouseDelta.x, io.MouseDelta.y) / .7853981634f);
            float pitch = io.MouseDelta.y / scale, bank = io.MouseDelta.x / scale;
            float cp = cosf(pitch), sp = sinf(pitch), cb = cosf(bank), sb = sinf(bank);
            motion_gizmo_arcball(cp * cb, sp * cb, cp * sb, sp * sb);
        }
    }
    if (button >= 0) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
}
