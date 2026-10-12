-- JSON processing benchmark
-- Mirrors the reference json_processing benchmark: AI agents communicate
-- via JSON (tool schemas, function-call arguments, structured responses,
-- conversation histories). The C kernel exercises a full recursive-descent
-- JSON parser/serializer across four phases:
--   1. generate + serialize tool schemas
--   2. deserialize all schemas back
--   3. serialize/deserialize multi-turn conversations, re-serialize tool_use args
--   4. function call -> result round trips
--
-- Workload constants live here (edit to tune, no recompile). `total_sets` is
-- the per-event total workload, split across `threads` (more threads finish
-- the same work faster); keep it >= your core count so threads are not capped.

local TOTAL_SETS = 256                -- workload sets per event (>= core count)
local NUM_SCHEMAS = 200                -- tool schemas per set
local NUM_CONVERSATIONS = 100          -- conversations per set
local MESSAGES_PER_CONVERSATION = 50   -- messages per conversation
local NUM_ROUND_TRIPS = 500            -- function-call round trips per set

local ffi = require("ffi")
ffi.cdef[[
long long run_json_processing(int total_sets, int num_schemas, int num_conversations,
                        int messages_per_conversation, int num_round_trips,
                        int threads);
]]

function event(thread_id)
    return ffi.C.run_json_processing(
        TOTAL_SETS, NUM_SCHEMAS, NUM_CONVERSATIONS,
        MESSAGES_PER_CONVERSATION, NUM_ROUND_TRIPS,
        agentbench.opt.threads
    )
end
