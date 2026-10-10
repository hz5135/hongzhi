# AgentBench

**AgentBench** is a benchmark tool to measure the performance of AI agent system.

## Features

- Simulate common micro-operations of AI agent system and score the performance.
- Simulate common micro-operations of AI agent system and measure the power of hardware.
- Simulate common micro-operations of multiple AI agents to evaluate the max parallelism.
- Measure the performance of a self-defined AI agent system with rich and typical scenarios.

## Install Guidence

### Linux

// TODO

### macOS

// TODO

### Windows

// TODO

### Source Code

// TODO

## Usage

### Work Mode

- Simulation Mode: Simulate the work progress of AI agent system with kinds of common micro-operations. Users can evaluate the performance of local software and hardware system roughly as to AI agent scenarios in this mode.
- Client Mode: Simulate multiple clients to visit self-defined AI agent system which is a black-box. Users can evaluate the performance of the system with real scenarios.

### Command

``` Bash
[root@localhost ~]# agentbench --help
Usage:
agentbench --mode=simulation [options]
agentbench --mode=client --host=$HOST --port=$PORT [options]

Benchmark suit for AI agent workloads

General options：
  -m, --mode=MODE  run mode for CPU benchmark or Agent workload, simulation for CPU benchmark and client for Agent workload
  --help  show this help message and exit
  
Simulation mode options:
  -b, --benchmarks=BENCHMARK_LIST  run only the named benchmark, separated by comma if mutiple (default: all simulation benchmarks)
  -i, --iterations=ITERATIONS  number of timed iterations per benchmark (default: 5)
  -t, --threads=THREADS  max threads for concurrent benchmarks (default: CPU count)
  --power  measure the power of CPU and server
  --bmc_ip=BMC_IP  the BMC IP for server, required if `--power` is set
  --bmc_user=USER_NAME  username of admin account, required if `--power` is set
  --bmc_password=PASSWORD  password of admin account, required if `--power` is set
  -s, --sandbox  measure the parallelism of sandbox
  --parallelism=SANDBOX_COUNT  sandbox count to be started, required if `--sandbox` is set
  -j, --json  output results as JSON
  -v, --verbose  show detailed per-iteration results  
  
Client mode options:
  -h, --host=HOST  Server host to be tested
  -p, --port=PORT  Gateway service port
  -b, --benchmarks=BENCHMARK_LIST  run only the named benchmark, separated by comma if mutiple (default: all scenario benchmarks)
  -c, --clients=CLIENT_COUNT  clients number to be created (default: 32)
```

## Options

| Option        | Type   | Abbreviation | Default Value | Required | Enum                                                                                                                                                                                                                                                                 | Description                                                                                                                                                                                                 | Remarks                                                               |
| ------------- | ------ | ------------ | ------------- | -------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------- |
| mode          | string | -m           | -             | Y        | • simulation: works in simulation mode, tests locally only.<br>• client: works in real scenario mode, both local and remote server test available.                                                                                                                   | Benchmark work mode. If it is simulation, specified or all simulated operations will be executed. If it is client，benchmark will act as clients to test self-defined AI agent performance run on remote servers. |                                                                       |
| benchmarks    | string | -b           | all           | N        | Simulation Mode:<br>• context_switching<br>• json_processing<br>• text_processing<br>• tree_search<br>• concurrent_dispatch<br>• memory_pressure<br>• subprocess_spawning<br>• diff_patch<br>• html_parsing<br>• schema_validation<br>• streaming_parse<br>• code_edit_apply<br>• agentic_loop<br><br>Client Mode:<br>• web_service<br>• code_edit<br>• image_recognition<br>• data_query<br>• system_maintance<br>• report_analysis | Benchmark suite (or micro-operations) to be run.                                                                                                                                                           | All benchmark suits will be run under the specified mode by default.   |
| iterations    | int    | -i           | 5             | N        | -                                                                                                                                                                                                                                                                    | Rounds to be run.                                                                                                                                                                                           | Only works under simulation mode.                                     |
| threads       | int    | -t           | CPU_COUNT     | N        | -                                                                                                                                                                                                                                                                    | Concurrent thread count.                                                                                                                                                                                   | • Only works under simulation mode.<br>• CPU core count by default.    |
| json          | -      | -j           | -             | N        | -                                                                                                                                                                                                                                                                    | Whether output is JSON format for the stdout.                                                                                                                                                              | • Only works under simulation mode.<br>• Output in human-readable format by default. |
| verbose       | -      | -v           | -             | N        | -                                                                                                                                                                                                                                                                    | Whether output all the mid-process log.                                                                                                                                                                    | • Only works under simulation mode.<br>• Only output the statistical result by default. |
| host          | string | -h           | -             | Y        | -                                                                                                                                                                                                                                                                    | Host IP of the agent server.                                                                                                                                                                               | Only works under client mode.                                          |
| port          | int    | -p           | -             | Y        | -                                                                                                                                                                                                                                                                    | Server port of agent service.                                                                                                                                                                              | Only works under client mode.                                          |
| clients       | int    | -c           | 32            | N        | -                                                                                                                                                                                                                                                                    | Client count to be simulated.                                                                                                                                                                              | Only works under client mode.                                          |
| power         | -      | -            | -             | N        | -                                                                                                                                                                                                                                                                    | Whether measure the power of CPU and Server.                                                                                                                                                               | Only works under client mode.                                          |
| bmc_ip        | string | -            | -             | N        | -                                                                                                                                                                                                                                                                    | BMC IP, required if --power is set.                                                                                                                                                                        | Only works under client mode.                                          |
| bmc_user      | string | -            | -             | N        | -                                                                                                                                                                                                                                                                    | User name of BMC admin account，required if --power is set.                                                                                                                                                | Only works under client mode.                                          |
| bmc_password  | string | -            | -             | N        | -                                                                                                                                                                                                                                                                    | Password of BMC admin account，required if --power is set.                                                                                                                                                 | Only works under client mode.                                          |
| sandbox       | -      | -s           | -             | N        | -                                                                                                                                                                                                                                                                    | Whether testing the density of sandbox.                                                                                                                                                                    | Only works under client mode.                                          |
| parallelism   | int    | -            | -             | N        | -                                                                                                                                                                                                                                                                    | Sandbox count to be started.                                                                                                                                                                               | Only works under client mode.                                          |
| help          | string | -            | -             | N        | -                                                                                                                                                                                                                                                                    | Output the usage and description of all options.                                                                                                                                                           |                                                                       |

## Output

### Simulation Mode

```Bash
[root@localhost ~]# agentbench --mode=simulation

===========================================================================
  AgentBench v1.0.0
  Benchmark suit for AI agent workloads
===========================================================================

  CPU:         HJMicro(R) Hongmin(R) 9198 Processor
  Socket:      2
  NUMA:        2
  Cores:       256
  Platform:    Linux-5.10.211.v10+-aarch64-with-glibc2.28
  Arch:        aarch64
  Time：       2026-07-27 10:45:21

===========================================================================
  Benchmark                 Time (s)   StdDev      CV        Ops    Score
---------------------------------------------------------------------------

  context_switching            0.153    0.029  21.2%     30,000    13047
  json_processing              0.051    0.000   0.1%      4,300    29235
  text_processing              0.154    0.000   0.2%    130,743    16261
  tree_search                  3.830    0.004   0.1%  4,653,944      783
  concurrent_dispatch          0.109    0.002   2.0%      4,000    18301
  memory_pressure              2.586    0.008   0.3%    265,182     1547
  subprocess_spawning          5.670    0.273   4.9%     25,590      882
  diff_patch                   0.312    0.005   1.5%     17,727     6404
  html_parsing                 0.438    0.002   0.5%     52,368     3424
  schema_validation            0.566    0.008   1.3%     12,710     3534
  streaming_parse              0.287    0.000   0.1%     71,578     5228
  code_edit_apply              0.145    0.000   0.1%      4,774    13802
  agentic_loop                 0.249    0.002   0.8%    317,047    12069
---------------------------------------------------------------------------

                        Single-Agent Speed      6710
                    Multi-Agent Throughput      5039
                         OVERALL COMPOSITE      6282

===========================================================================
```

### Client Mode

```Bash
[root@localhost ~]# agentbench --mode=client --host=$HOST --port=$PORT
========================================================================================================================

  AgentBench v1.0.0
  Benchmark suit for AI agent workloads
========================================================================================================================

  CPU:         HJMicro(R) Hongmin(R) 9198 Processor
  Socket:      2
  NUMA:        2
  Cores:       256
  Platform:    Linux-5.10.211.v10+-aarch64-with-glibc2.28
  Arch:        aarch64
  Time：       2026-07-27 10:45:21

========================================================================================================================
  Benchmark            P99(s)    throughout(10s)    throughout(20s)    throught(50s)    throught(100s)    throught(200s)
---------------------------------------------------------------------------------------------------------------------

  web_service          1.28        5                  19                48                79                132
  code_edit            38          -                  -                 -                 -                 -
  image_recognition    0.24        24                 78                156               287               401
  data_query           1.02        201                398               766               1089              2300
  system_maintance     32.80       -                  -                 -                 -                 -
  report_analysis      39.81       12                 27                50                112               238
========================================================================================================================
```

### Power Measurement (optional)

```Bash
[root@localhost ~]# agentbench --mode=simulation --power --bmc_ip=$BMC_IP --bmc_user=$BMC_USER --bmc_password=$BMC_PASSWORD
===========================================================================

  AgentBench v1.0.0
  Benchmark suit for AI agent workloads
===========================================================================

  CPU:         HJMicro(R) Hongmin(R) 9198 Processor
  Socket:      2
  NUMA:        2
  Cores:       256
  Platform:    Linux-5.10.211.v10+-aarch64-with-glibc2.28
  Arch:        aarch64
  Time：       2026-07-27 10:45:21

===========================================================================

                       CPU Max Power    280 Watts
                       CPU Min Power    280 Watts
                       Total Max Power  693 Watts
                       Total Min Power  682 Watts
                       Metrics File     power-2026-07-29-10.45.21.log
                       
===========================================================================
```

### Sandbox Density (optional)

```Bash
[root@localhost ~]# agentbench --mode=simulation --sandbox --parallelism=256
===========================================================================

  AgentBench v1.0.0
  Benchmark suit for AI agent workloads
===========================================================================

  CPU:         HJMicro(R) Hongmin(R) 9198 Processor
  Socket:      2
  NUMA:        2
  Cores:       256
  Platform:    Linux-5.10.211.v10+-aarch64-with-glibc2.28
  Arch:        aarch64
  Time：       2026-07-27 10:45:21

===========================================================================

                    Sandbox Count          256
              Average Agent Speed          6710
                     Metrics File          sandbox-2026-07-29-10.45.21.log
            
===========================================================================
```

## Reference

- [AgentiCPUMark](https://github.com/apellegr/AgentiCPUMark): AgentiCPUMark is a CPU-oriented benchmark tool to measure the performance of AI agent operations. Although it is not designed for real AI agent systems, we consider it as an excellent case to evalute the performance of local environment roughly.
- [Sysbench](https://github.com/akopytov/sysbench): Sysbench is a famous benchmark tool in database filed around the world. It supports TPC-C standard and simulates specifled count of clients to access self-defined database system.
- [TPC-C](https://www.tpc.org/tpcc/): TPC-C is the most widely used online transaction processing benchmark. During designing the benchmark metrics, we referenced its principle a lot.  
- [TPC-H](https://www.tpc.org/tpch/): TPC-H is another famous benchmark which is often used to measure the performance of complex query systems such as big data workloads. It consists of 22 different kinds of queries to cover most typical scenarios.

## Contributing

Contributions are warmly welcomed! Please open an issue to describe your ideas before submitting a PR.

## License

[GPL-v3.0](https://opensource.org/license/GPL-3.0)
