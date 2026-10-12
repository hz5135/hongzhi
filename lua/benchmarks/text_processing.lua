-- Text processing benchmark (agent text/chunking/regex/BPE/log parsing)
-- Mirrors the reference text_processing benchmark: the C kernel builds a
-- pseudo-realistic corpus and runs five phases — overlapping chunk splitting
-- with content hashing, 20 regex-like pattern extractions, byte-pair
-- frequency counting, top-k frequent pair merging (simplified BPE), and
-- line-by-line log parsing.
--
-- Workload constants live here (edit to tune, no recompile). `total_sets` is
-- the per-event total workload, split across `threads` (more threads finish
-- the same work faster); keep it >= your core count so threads are not capped.

local TOTAL_SETS = 256      -- workload sets per event (>= core count)
local CORPUS_SIZE = 500000  -- corpus length in characters

local ffi = require("ffi")
ffi.cdef[[
long long run_text_processing(int total_sets, int corpus_size, int threads);
]]

function event(thread_id)
    return ffi.C.run_text_processing(
        TOTAL_SETS, CORPUS_SIZE,
        agentbench.opt.threads
    )
end
