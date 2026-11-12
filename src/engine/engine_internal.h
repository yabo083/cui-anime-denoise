/* engine_internal.h -- non-public engine symbols shared with tests. Every
 * cross-file internal carries the eng__ prefix (same convention as upd__). */
#ifndef ENGINE_INTERNAL_H
#define ENGINE_INTERNAL_H

/* Tile positions on one axis of the fixed-window scheme (see engine.c). */
int eng__axis_windows(int n, int tile, int pad, int *starts, int *sizes,
                      int cap);

#endif
