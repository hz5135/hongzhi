#include "lua_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *g_lua_benchmark_dir = "lua/benchmarks";
const char *g_lua_internal_dir  = "lua/internal";

/* Path to the installed agentbench internal script.  We'll also search
 * the source directory during development.  Reentrant: caller provides
 * the output buffer.  Returns 1 if found, 0 otherwise. */
static int find_script_r(const char *dir, const char *name, char *out, size_t outsz) {
    const char *prefixes[] = { ".", "/usr/share/agentbench", NULL };
    for (int i = 0; prefixes[i]; i++) {
        snprintf(out, outsz, "%s/%s/%s", prefixes[i], dir, name);
        FILE *f = fopen(out, "r");
        if (f) { fclose(f); return 1; }
    }
    return 0;
}

/* Load and execute a Lua file.  Returns 0 on success. */
static int load_lua_file(lua_State *L, const char *path) {
    int rc = luaL_loadfile(L, path);
    if (rc != 0) {
        fprintf(stderr, "error loading %s: %s\n", path, lua_tostring(L, -1));
        lua_pop(L, 1);
        return rc;
    }
    rc = lua_pcall(L, 0, 0, 0);
    if (rc != 0) {
        fprintf(stderr, "error running %s: %s\n", path, lua_tostring(L, -1));
        lua_pop(L, 1);
        return rc;
    }
    return 0;
}

lua_State *lua_bridge_global_init(void) {
    lua_State *L = luaL_newstate();
    if (!L) {
        fprintf(stderr, "error: failed to create Lua state\n");
        return NULL;
    }
    luaL_openlibs(L);

    /* Create the agentbench global table */
    lua_newtable(L);                              /* agentbench */
    lua_setglobal(L, "agentbench");

    /* Load the internal agentbench.lua framework */
    char script_path[512];
    if (!find_script_r(g_lua_internal_dir, "agentbench.lua",
                       script_path, sizeof(script_path))) {
        /* Try relative to current directory */
        snprintf(script_path, sizeof(script_path), "lua/internal/agentbench.lua");
    }
    if (load_lua_file(L, script_path) != 0) {
        lua_close(L);
        return NULL;
    }

    return L;
}

lua_State *lua_bridge_thread_init(lua_State *global, const char *script_path) {
    (void)global;  /* We don't copy state, we load the script fresh */

    lua_State *L = luaL_newstate();
    if (!L) return NULL;
    luaL_openlibs(L);

    /* Create agentbench table */
    lua_newtable(L);
    lua_setglobal(L, "agentbench");

    /* Load the internal agentbench.lua framework */
    char internal_path[512];
    if (!find_script_r(g_lua_internal_dir, "agentbench.lua",
                       internal_path, sizeof(internal_path))) {
        snprintf(internal_path, sizeof(internal_path), "lua/internal/agentbench.lua");
    }
    if (load_lua_file(L, internal_path) != 0) {
        lua_close(L);
        return NULL;
    }

    /* Load the benchmark script */
    if (script_path) {
        if (load_lua_file(L, script_path) != 0) {
            lua_close(L);
            return NULL;
        }
    }

    /* Apply option defaults from the benchmark script */
    lua_getglobal(L, "agentbench");
    lua_getfield(L, -1, "apply_defaults");
    if (lua_isfunction(L, -1)) {
        lua_pcall(L, 0, 0, 0);
    } else {
        lua_pop(L, 1);  /* pop nil */
    }
    lua_pop(L, 1);  /* pop agentbench */

    return L;
}

int lua_bridge_thread_run(lua_State *L, int thread_id) {
    /* Get the thread_run function from the global table */
    lua_getglobal(L, "thread_run");
    if (!lua_isfunction(L, -1)) {
        fprintf(stderr, "error: thread_run not defined in Lua\n");
        lua_pop(L, 1);
        return 0;
    }

    lua_pushinteger(L, thread_id);
    int rc = lua_pcall(L, 1, 1, 0);
    if (rc != 0) {
        fprintf(stderr, "error in thread_run: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return 0;
    }

    /* Return value: 0 = success, non-zero = error */
    if (lua_isnumber(L, -1)) {
        int ret = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
        return ret;
    }
    lua_pop(L, 1);
    return 0;
}

int lua_bridge_call_thread_init(lua_State *L, int thread_id) {
    lua_getglobal(L, "thread_init");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return 0;  /* thread_init is optional */
    }
    lua_pushinteger(L, thread_id);
    int rc = lua_pcall(L, 1, 0, 0);
    if (rc != 0) {
        fprintf(stderr, "error in thread_init: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return -1;
    }
    return 0;
}

void lua_bridge_call_thread_done(lua_State *L, int thread_id) {
    lua_getglobal(L, "thread_done");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return;  /* thread_done is optional */
    }
    lua_pushinteger(L, thread_id);
    int rc = lua_pcall(L, 1, 0, 0);
    if (rc != 0) {
        fprintf(stderr, "error in thread_done: %s\n", lua_tostring(L, -1));
    }
    lua_pop(L, 1);
}

void lua_bridge_thread_close(lua_State *L) {
    if (L) lua_close(L);
}

void lua_bridge_global_close(lua_State *L) {
    if (L) lua_close(L);
}

void lua_bridge_set_opt(lua_State *L, const char *key, const char *value) {
    /* Set agentbench.opt.<key> = <value> (auto-convert numeric strings) */
    lua_getglobal(L, "agentbench");          /* agentbench */
    lua_getfield(L, -1, "opt");             /* agentbench.opt */
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);                      /* remove nil */
        lua_newtable(L);                    /* create new table */
        lua_setfield(L, -2, "opt");         /* agentbench.opt = {} */
        lua_getfield(L, -1, "opt");         /* agentbench.opt */
    }
    /* Try to convert to number; if it fails, push as string */
    char *end = NULL;
    double num = strtod(value, &end);
    if (end != value && *end == '\0') {
        lua_pushnumber(L, num);             /* value (number) */
    } else {
        lua_pushstring(L, value);           /* value (string) */
    }
    lua_setfield(L, -2, key);               /* agentbench.opt[key] = value */
    lua_pop(L, 2);                          /* agentbench.opt, agentbench */
}

void lua_bridge_apply_opts(lua_State *L, char **opts, int nopts) {
    for (int i = 0; i < nopts; i++) {
        /* Split "key=value" */
        char *eq = strchr(opts[i], '=');
        if (!eq) continue;
        *eq = '\0';
        lua_bridge_set_opt(L, opts[i], eq + 1);
        *eq = '=';  /* restore */
    }
}