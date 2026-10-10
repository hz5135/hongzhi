# Simulation Mode

## Target

Sometimes, users want to have a glimple of the performance of a specified hardware and basic software system without deploying LLM and AI agent workloads. Simulation mode is the best choice.

## Princeple

* Simulate different kinds of automic scenarios and the working progress of AI agent systems.
* Measure the time cost and score all the automic scenarios.
* Calculate the final score to roughly evaluate the performance.

## Detail

### Autiomic Scenarios

Automic scenarios or mico-operations come from business scenarios. According to the survey and a large number of experiments observation, some most common used automic scenarios are listed as below.

| No   | Automic Scenarios   | Weight | Description                                                                       |
| ---- | ------------------- | ------ | ----------------------------------------------------------------------------------|
| 1    | Agentic Loop        | 13%    | Full Glob->Grep->Read->Edit->Verify cycle with growing conversation state         |
| 2    | JSON Processing     | 10%    | Serialization & deserialization of tool schemas, function calls, and conversations|
| 3    | Subprocess Spawning | 9%     | Process fork/exec, pipe management, and output collection                         |
| 4    | Text Processing     | 8%     | Tokenizer-like chunking, regex parsing, BPE-like frequency counting               |
| 5    | Tree Search         | 8%     | Monte Carlo tree search and beam search simulating agent planning/reasoning       |
| 6    | Context Switching   | 7%     | Rapid coroutine/thread switching simulating agent reasoning loops                 |
| 7    | Concurrent Dispatch | 7%     | Thread-pool throughput for parallel tool execution with shared state              |
| 8    | Diff/Patch          | 7%     | Line-by-line comparison, unified diff generation, and patch application           |
| 9    | Streaming Parse     | 7%     | SSE stream parsing with fragmented tool-call JSON accumulation                    |
| 10   | Code Edit Apply     | 7%     | Exact substring search, uniqueness verification, and edit application             |
| 11   | HTML Parsing        | 6%     | DOM tree construction, CSS-like queries, and HTML-to-markdown conversion          |
| 12   | Schema Validation   | 6%     | JSON Schema compilation, validation, and malformed JSON repair                    |
| 13   | Memory Pressure     | 5%     | Large working-set manipulation, burst alloc/dealloc, context window management    |

Weight represents the frequency the automic scenario appears under the observation.

### Single Agent

In order to take a look at the performance of running single Agent, all the automic scenarios will be simulated except the No.3, 6 and 7. All the operations will be executed within a thread.

### Multiple Agents

All the automic scenarios will be simulated to evaluate the performance of multiple agents. All the operations will be executed within multiple threads. Users can set the thread count, namely agent concurrency. The default value is the total number of CPU cores.

### Score

Assuming the execution time of automic scenario A is `executionTime`, the score of this automic scenario is as follows.

$$
score = (referenceTime / executionTime) * 1000
$$

The `referenceTime` is the execution time running on Hongmin9198.

The final score is the sum of scores of all the automic scenarios.
