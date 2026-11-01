// The keys the recomp keeps for itself (runtime/host_input.c, runtime/overlay.cpp): [Keybinds] can't use them
// (config.c), and the settings menu marks a chat key on one of them (settings_panel.cpp). Keep in step with those.
#pragma once
#include <SDL3/SDL.h>

static inline const char *key_reserved(int scancode) {
    static const struct { int scancode; const char *use; } keys[] = {
        {SDL_SCANCODE_ESCAPE, "Release mouse"}, {SDL_SCANCODE_F1, "Settings"}, {SDL_SCANCODE_INSERT, "Forge menu"},
        {SDL_SCANCODE_F5, "Shader mods window"}, {SDL_SCANCODE_F6, "Shader mods"},
        {SDL_SCANCODE_F7, "Debug"}, {SDL_SCANCODE_F8, "Debug"}, {SDL_SCANCODE_F9, "Debug"}, {SDL_SCANCODE_F10, "Debug"},
        {SDL_SCANCODE_F11, "Full screen"}, {SDL_SCANCODE_F12, "Screenshot"},
    };
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
        if (keys[i].scancode == scancode) return keys[i].use;
    return NULL;
}
