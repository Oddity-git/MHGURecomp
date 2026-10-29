// Merging the same MT Framework archive (.arc) from several mods (romfs.c).
#pragma once
#include <stdbool.h>
#include <stddef.h>

// `orig` is the game's own archive, `prev` the one a mod put in its place so far, `mod` another mod's. Writes to a
// cache file (path in `out`) `prev` with the entries `mod` changed against `orig` (or added); entries `mod` left as in
// `orig` keep `prev`'s. False when a file isn't an archive of the same format: the caller then replaces `prev`.
bool arc_merge(const char *orig, const char *prev, const char *mod, char *out, size_t outsz);
