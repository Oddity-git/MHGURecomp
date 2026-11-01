// Shader mods: post passes from the mods' shaders/post/*.glsl, run on the game's 3D picture every frame just before
// the HUD is drawn, with the scene's depth and camera (runtime/nvn/vulkan/post.c). The hook points in exec.c and
// device.c are marked "shader mods:"; with no such mod none of them changes anything.
#pragma once
#include "../../shader_mods.h"
#include "fsr.h"

extern bool g_post_on;  // a mod's post passes compiled (set once at start-up: vk_post_init)

void vk_post_init(void);              // device.c, once the device exists
void vk_post_draw(const FsrDraw *d);  // exec.c draw(), before FSR's: may run the passes (the start of the HUD) first
void vk_post_present(NvnTexture *t);  // device.c backend_present, before FSR's: the end of the frame
// (F6 vk_post_toggle, Shift+F6 vk_post_reload, the F5 window's effects and settings: shader_mods.h)
