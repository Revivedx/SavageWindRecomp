#include "colour_vision.h"

#include <stdlib.h>
#include <string.h>

#include "settings.h"

// Machado et al. 2009, severity 1.0 (protanopia, deuteranopia, tritanopia), linear RGB, row-major
static const float k_sim[3][9] = {
    {0.152286f, 1.052583f, -0.204868f, 0.114503f, 0.786281f, 0.099216f, -0.003882f, -0.048116f, 1.051998f},
    {0.367322f, 0.860646f, -0.227968f, 0.280085f, 0.672501f, 0.047413f, -0.011820f, 0.042940f, 0.968881f},
    {1.255528f, -0.076749f, -0.178779f, -0.078411f, 0.930809f, 0.147602f, 0.004733f, 0.691367f, 0.303900f},
};
// Where the lost difference goes: red-green into green and blue; blue-yellow into red and green
static const float k_shift[3][9] = {
    {0, 0, 0, 0.7f, 1, 0, 0.7f, 0, 1},
    {0, 0, 0, 0.7f, 1, 0, 0.7f, 0, 1},
    {1, 0, 0.7f, 0, 1, 0.7f, 0, 0, 0},
};

static void identity(float m[9]) {
    memset(m, 0, 9 * sizeof m[0]);
    m[0] = m[4] = m[8] = 1;
}

static void mul(const float a[9], const float b[9], float out[9]) {
    float r[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
    memcpy(out, r, sizeof r);
}

void colour_vision_correction(int type, float strength, float m[9]) {
    identity(m);
    if (type < 1 || type > 3 || strength <= 0) return;
    float d[9];  // I - S
    for (int i = 0; i < 9; i++) d[i] = (i % 4 == 0 ? 1.0f : 0.0f) - k_sim[type - 1][i];
    float e[9];
    mul(k_shift[type - 1], d, e);
    for (int i = 0; i < 9; i++) m[i] += strength * e[i];
}

void colour_vision_simulation(int type, float severity, float m[9]) {
    identity(m);
    if (type < 1 || type > 3 || severity <= 0) return;
    for (int i = 0; i < 9; i++) m[i] += severity * (k_sim[type - 1][i] - m[i]);
}

bool colour_vision_matrix(float m[9]) {
    static int sim_type = -1;
    static float sim_severity = 1;
    if (sim_type < 0) {
        sim_type = 0;
        const char *e = getenv("SAVAGEWIND_CVD_SIMULATE");
        if (e && *e) {
            sim_type = !strncmp(e, "protan", 6) ? 1 : !strncmp(e, "deutan", 6) ? 2 : !strncmp(e, "tritan", 6) ? 3 : 0;
            const char *c = strchr(e, ':');
            if (c) sim_severity = (float)atoi(c + 1) / 100.0f;
        }
    }
    int type = g_settings.colour_vision;
    bool corr = type >= 1 && type <= 3;
    if (!corr && !sim_type) return false;
    colour_vision_correction(corr ? type : 0, g_settings.colour_vision_strength / 100.0f, m);
    if (sim_type) {
        float s[9];
        colour_vision_simulation(sim_type, sim_severity, s);
        mul(s, m, m);
    }
    return true;
}
