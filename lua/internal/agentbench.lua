-- agentbench.lua — Internal framework for the LuaJIT-based benchmark runner.
-- Loaded into every Lua state (both global and per-thread).
-- Provides the thread_run loop that drives event() calls with timing.

local ffi = require("ffi")

-- Declare C functions exported by the agentbench binary
ffi.cdef[[
int sb_more_events(int thread_id);
void sb_event_start(int thread_id);
void sb_event_stop(int thread_id);
void sb_add_ops(int thread_id, uint64_t n);
]]

-- Set up agentbench table (created by C code before loading this script)
-- We ensure agentbench.opt exists
if not agentbench then
    agentbench = {}
end
if not agentbench.opt then
    agentbench.opt = {}
end

-- Default thread_run loop.
-- Calls event() repeatedly until sb_more_events returns false.
-- Each iteration is timed by sb_event_start / sb_event_stop.
-- The return value of event() is accumulated as ops.
function thread_run(thread_id)
    while ffi.C.sb_more_events(thread_id) ~= 0 do
        ffi.C.sb_event_start(thread_id)
        local ok, ops = pcall(event, thread_id)
        if not ok then
            -- event() raised an error — log and continue
            io.stderr:write("event error on thread ", thread_id, ": ", ops, "\n")
            ffi.C.sb_event_stop(thread_id)
        else
            ffi.C.sb_event_stop(thread_id)
            -- A kernel op count returned as a 64-bit integer reaches Lua as
            -- cdata<int64_t>, so convert before counting.
            local n = tonumber(ops)
            if n then
                ffi.C.sb_add_ops(thread_id, n)
            end
        end
    end
end

-- Default no-op thread_init (can be overridden by benchmark scripts)
function thread_init(thread_id)
end

-- Default no-op thread_done (can be overridden by benchmark scripts)
function thread_done(thread_id)
end

-- Apply defaults from agentbench.cmdline.options into agentbench.opt.
-- Called by the C code after loading the benchmark script.
-- Each option is defined as {description, default_value}.
function agentbench.apply_defaults()
    if agentbench.cmdline and agentbench.cmdline.options then
        for k, v in pairs(agentbench.cmdline.options) do
            if agentbench.opt[k] == nil then
                agentbench.opt[k] = v[2]
            end
        end
    end
end

-- Locate a data file, trying CWD and installed locations.
-- Usage: agentbench.find_data_file("data/text_sample.txt")
function agentbench.find_data_file(rel)
    if rel == nil or rel == "" then return nil end
    -- Absolute or explicitly overridden path: use as-is
    if rel:sub(1,1) == "/" then
        local f = io.open(rel, "r")
        if f then f:close() return rel end
        return nil
    end
    local candidates = { rel, "/usr/share/agentbench/" .. rel }
    for _, p in ipairs(candidates) do
        local f = io.open(p, "r")
        if f then f:close() return p end
    end
    return nil
end

-- Escape a string for embedding in JSON (escape \, ", \n, \r, \t)
function agentbench.json_escape(s)
    if s == nil then return "" end
    return s:gsub('\\', '\\\\'):gsub('"', '\\"'):gsub('\n', '\\n'):gsub('\r', '\\r'):gsub('\t', '\\t')
end