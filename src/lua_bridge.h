#ifndef LUA_BRIDGE_H
#define LUA_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#ifdef __cplusplus
}
#endif

/* Create the global Lua state (loads agentbench.lua framework). */
lua_State *lua_bridge_global_init(void);

/* Create a per-thread Lua state, load a benchmark script.
 * Returns NULL on error.  The state inherits the ffi.cdef from the
 * global state via the agentbench.lua internal script. */
lua_State *lua_bridge_thread_init(lua_State *global, const char *script_path);

/* Call thread_run(thread_id) on the thread state.
 * Returns the number of ops from the last event, or 0 on error. */
int lua_bridge_thread_run(lua_State *L, int thread_id);

/* Call thread_init(thread_id) on the thread state.  Returns 0 on success. */
int lua_bridge_call_thread_init(lua_State *L, int thread_id);

/* Call thread_done(thread_id) on the thread state. */
void lua_bridge_call_thread_done(lua_State *L, int thread_id);

/* Close a per-thread Lua state. */
void lua_bridge_thread_close(lua_State *L);

/* Close the global Lua state. */
void lua_bridge_global_close(lua_State *L);

/* Set Lua script options (agentbench.opt.<key> = value) from CLI args. */
void lua_bridge_set_opt(lua_State *L, const char *key, const char *value);

/* Apply a list of "key=value" pairs to agentbench.opt.
 * Called after thread init, before thread_run. */
void lua_bridge_apply_opts(lua_State *L, char **opts, int nopts);

/* Path to the lua/benchmarks directory.  Set at startup. */
extern const char *g_lua_benchmark_dir;
extern const char *g_lua_internal_dir;

#endif /* LUA_BRIDGE_H */