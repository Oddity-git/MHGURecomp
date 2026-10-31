# Shader mods

A mod folder may hold `shaders/post/*.glsl`: compute passes the renderer runs over the game's 3D picture once a
frame, just before the HUD is drawn (`runtime/nvn/vulkan/post.c`). Like the rest of a mod they load by dropping the
folder into `mods/` and stop loading when it is removed or turned off in the Mods tab.

- Passes run in file name order, mods in load order. A file whose name starts with `_` is skipped.
- They run only on frames with a 3D scene and the main camera (not in menus, loading screens or event cameras).
- With FSR on they run before it, at render size.
- A pass that does not compile is left out; the errors are in the log (`[post]`, line numbers are the file's).
- In game, **F6** turns all shader mods off and on (to compare), **Shift+F6** reads the files again (edit a pass,
  press it, see the change without restarting).
- `MHGU_POST_LOG=1`: a line per second with how many frames the passes ran on, and why they were passed over.

## Effects and settings (the Shader mods window, F5)

**F5** opens a window with every effect of the shader mods: a switch for each, and sliders for its settings,
changed while the game runs. What is changed is kept in the mod's `shaders/settings.ini`.

- Passes whose first line is `// Name: what it does` with the same Name are one effect, turned on and off together
  (`// Name (off): ...`: it starts off).
- A constant with a range in its comment is a setting:

```glsl
const float RADIUS = 90.0;  // [10, 400] Radius: how far round a point to look (cm)
const int SAMPLES = 12;     // [4, 32] Samples: more is smoother and slower
const vec3 TINT = vec3(1.0, 0.9, 0.8);  // [color] Tint: colour of the glow
```

  The window shows the label (before the `:`) with the rest as its tooltip. The value comes from a uniform buffer, so
  moving a slider needs no compiling.

## Writing a pass

A pass is a GLSL 4.50 compute shader without `#version` or `main`: it defines

```glsl
vec4 effect(vec2 uv, ivec2 px)  // the colour of texel px (uv: its centre, 0..1, y down)
```

and can use:

| name | what |
| --- | --- |
| `u_scene` | the picture so far (the previous pass's result; linear filtering) |
| `u_original` | the game's picture before the first pass, with mipmaps (`textureLod(u_original, uv, 4.0)`: blurred) |
| `u_depth` | the scene's depth buffer as stored (0..1, nearest) |
| `u.proj`, `u.inv_proj` | view space <-> clip space (view space: x right, y up, -z forward) |
| `u.view`, `u.inv_view` | world space <-> view space (world units: centimetres, y up) |
| `u.size` | width, height, 1 / width, 1 / height |
| `u.info` | seconds since start, frame number, number of passes |
| `u.cam` | near, far, vertical field of view (radians) |
| `u.eye` | the camera's position in world space |
| `u_pass.pass.x` | this pass's index |
| `raw_depth(uv)`, `view_pos(uv)`, `view_dist(uv)`, `world_pos(uv)` | the scene at uv |
| `view_normal(uv)` | the surface normal at uv (view space, from the depth) |
| `is_sky(uv)` | nothing drawn there but the sky |
| `project(vp)` | a view-space point -> uv |
| `luma(rgb)` | brightness |
| `u_light.pos` | the sun / moon on screen: uv, strength 0..1, w = 1 when found (the brightest part of the sky) |
| `u_light.color` | its colour; a = the brightness the sky round it is measured against |
| `u_light.sky` | the sky's average colour (kept while no sky is in view) |
| `u_light.state` | x: a height that follows the camera's slowly (camera - 300 cm), for fog; w = 1 once set |

The alpha channel passes from one pass to the next, so a pass can leave data there for the next one (the Lighting
mod's ambient occlusion does); the last pass should put `u_original`'s alpha back.
