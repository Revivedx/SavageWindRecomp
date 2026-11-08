// PC handhelds (included by runtime/settings.c only; settings.h has what the rest uses): this machine by its firmware's
// maker and model, and the size of its screen. A handheld not in the table but of a maker that only makes them takes
// the desktop's size (w 0). SAVAGEWIND_HANDHELD=0, 1, deck or WxH decides it (tests).
#pragma once
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

typedef struct { const char *vendor, *product; short w, h; bool deck; } HandheldModel;
static const HandheldModel k_handhelds[] = {
    {"Valve", "Jupiter", 1280, 800, true},    // Steam Deck LCD
    {"Valve", "Galileo", 1280, 800, true},    // Steam Deck OLED
    {"ASUS", "RC71L", 1920, 1080, false},     // ROG Ally
    {"ASUS", "RC72L", 1920, 1080, false},     // ROG Ally X
    {"ASUS", "RC73", 1920, 1080, false},      // ROG Xbox Ally, Ally X
    {"LENOVO", "83E1", 2560, 1600, false},    // Legion Go
    {"LENOVO", "83L3", 1920, 1200, false},    // Legion Go S
    {"LENOVO", "83N6", 1920, 1200, false},
    {"LENOVO", "83Q2", 1920, 1200, false},
    {"LENOVO", "83Q3", 1920, 1200, false},
    {"LENOVO", "83N0", 1920, 1200, false},    // Legion Go 2
    {"LENOVO", "83N1", 1920, 1200, false},
    {"Micro-Star", "Claw A1M", 1920, 1080, false},
    {"Micro-Star", "Claw 8", 1920, 1200, false},
    {"Micro-Star", "Claw 7", 1920, 1200, false},
    {"Micro-Star", "Claw", 0, 0, false},
    {"ZOTAC", "ZONE", 1920, 1080, false},
    {"GPD", "G1617", 1920, 1080, false},      // Win Mini
    {"GPD", "G1618", 1920, 1080, false},      // Win 4
    {"AYANEO", "", 0, 0, false},
    {"AYADEVICE", "", 0, 0, false},
    {"ONE-NETBOOK", "", 0, 0, false},
};

static void handheld_firmware(char *vendor, char *product, size_t n) {
    vendor[0] = product[0] = 0;
#ifdef _WIN32
    static const char key[] = "HARDWARE\\DESCRIPTION\\System\\BIOS";
    DWORD s = (DWORD)n;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, key, "SystemManufacturer", RRF_RT_REG_SZ, NULL, vendor, &s)) vendor[0] = 0;
    s = (DWORD)n;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, key, "SystemProductName", RRF_RT_REG_SZ, NULL, product, &s)) product[0] = 0;
#elif defined(__linux__)
    FILE *f = fopen("/sys/devices/virtual/dmi/id/sys_vendor", "r");
    if (f) { if (!fgets(vendor, (int)n, f)) vendor[0] = 0; fclose(f); }
    f = fopen("/sys/devices/virtual/dmi/id/product_name", "r");
    if (f) { if (!fgets(product, (int)n, f)) product[0] = 0; fclose(f); }
    vendor[strcspn(vendor, "\r\n")] = 0, product[strcspn(product, "\r\n")] = 0;
#else
    (void)n;
#endif
}

typedef struct { bool on, deck; int w, h; } Handheld;

static Handheld handheld_detect(void) {
    Handheld h = {0};
    const char *e = getenv("SAVAGEWIND_HANDHELD");
    if (e && *e) {
        int w, ht;
        if (!strcmp(e, "deck")) h = (Handheld){true, true, 1280, 800};
        else if (sscanf(e, "%dx%d", &w, &ht) == 2) h = (Handheld){true, false, w, ht};
        else h.on = atoi(e) != 0;
        return h;
    }
    char vendor[128], product[128];
    handheld_firmware(vendor, product, sizeof vendor);
    for (size_t i = 0; i < sizeof k_handhelds / sizeof k_handhelds[0]; i++) {
        const HandheldModel *m = &k_handhelds[i];
        if (strstr(vendor, m->vendor) && strstr(product, m->product)) return (Handheld){true, m->deck, m->w, m->h};
    }
    e = getenv("SteamDeck");  // (Steam's own, in Gaming Mode)
    if (e && atoi(e) != 0) h = (Handheld){true, true, 1280, 800};
    return h;
}
