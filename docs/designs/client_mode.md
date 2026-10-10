# Client Mode

## Target

This mode works with self-deployed LLM and AI agent system. It aims at evaluatling the accurate performance of AI agent system by simulating multiple client visits.

## Princeple

* Simulate specified number of clients to visit the AI agent system without considering the construction of the system.
* All requests are close to business scenarios.
* Observe the max session and request concurrency as the result.

## Detail

### Business Scenarios

At present, we set the following business scenarios.

| No   | Business Scenarios | Description                                                                       |
| ---- | -------------------|-----------------------------------------------------------------------------------|
| 1    | Web Service        | Startup Browser, search and filter something on specified aspect                  |
| 2    | Code Edit          | Implement a project-level function via create a code reposity                     |
| 3    | Image Recognition  | Recognize a specified number of images                                            |
| 4    | Data Query         | Visit a database and make a heavy query                                           |
| 5    | System Maintance   | Check the health of a virtual IT system and maintain the alerts                   |
| 6    | Report Analysis    | Read and analyse specified data source                                            |

### Metrics

* Throughout: concurrent tasks completed in 10s/20s/50s/100s/200s.
* Latency: End-to-end duration to compelte tasks. P99 is selected as the metric.

As to each scenario, the metrics are as follows.

| No   | Business Scenarios | Metrics                        |
| ---- | -------------------|--------------------------------|
| 1    | Web Service        | P99, T10, T20, T50, T100, T200 |
| 2    | Code Edit          | P99                            |
| 3    | Image Recognition  | P99, T10, T20, T50, T100, T200 |
| 4    | Data Query         | P99, T10, T20, T50, T100, T200 |
| 5    | System Maintance   | P99                            |
| 6    | Report Analysis    | P99, T10, T20, T50, T100, T200 |
