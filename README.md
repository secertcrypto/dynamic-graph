# CrIS-MPC

CrIS-MPC implements graph-processing protocols and benchmarks from:

- [Graphiti: Secure Graph Computation Made More Scalable](https://eprint.iacr.org/2024/1756).
- [GraSP: Secure Collaborative Graph Processing Made Scalable](https://eprint.iacr.org/2025/590).

Both use a shared C++17 framework for semi-honest secure multiparty computation over rings of the form $\mathbb{Z}_{2^k}$. The framework provides a circuit-building API, offline preprocessing, online evaluation, and two protocol backends. This repository is research and benchmarking code; the implementation scope and benchmark simplifications are described below.

## Stateful dynamic graph computation

The [dynamic graph runtime](src/dynamic_graph/ENGINE.md) implements Init,
between-round delta insertions, and Iter on secret-shared DAG lists. It
supports weighted message passing and bounded-hop reachability, with a
secure carry correction for exact decoding of bounded LWR errors. Build
`dynamic_graph_run` with the RISA read option enabled.

## Protocols

| Protocol | Sharing | Processes |
| --- | --- | --- |
| `rss3` | Three-party replicated secret sharing | Three compute parties, with PIDs `0`, `1`, and `2` |
| `nph` | N-party additive sharing | `n` compute parties with PIDs `0..n-1`, plus a preprocessing helper with PID `n` |

## Circuit primitives

The circuit API provides:

- Secret input sharing
- Addition and subtraction of shared values
- Addition, subtraction, and multiplication by a public constant
- Multiplication of two shared values
- Equality to zero and equality of two shared values
- Signed less-than-zero comparison
- Reconstruction to all parties
- Reconstruction to one selected party
- Grouped shuffle and inverse shuffle
- Permute+Share and NPH amortized Permute+Share
- Local permutation and Propagate/Gather subcircuits

Arithmetic is performed using native unsigned 8-, 16-, 32-, or 64-bit types. Overflow implements reduction modulo $2^k$. Signed comparison interprets the most significant bit using two's-complement representation.

## Repository structure

```text
CrIS-MPC/
├── src/
│   ├── common/
│   │   ├── circuit/           # Gates, circuit construction, and level ordering
│   │   ├── protocol_runner.h  # Common RSS3/NPH execution interface
│   │   └── types.h            # Ring and wire types
│   ├── 3pc/
│   │   ├── arith/             # RSS3 offline and online evaluators
│   │   ├── net/               # Three-party networking
│   │   └── utils/             # Replicated shares and pairwise PRGs
│   └── nph/
│       ├── arith/             # NPH offline and online evaluators
│       ├── net/               # N-party networking
│       └── utils/             # Additive shares, PRGs, and preprocessing types
├── benchmark/
│   ├── primitives/           # Arithmetic, sort, and permutation benchmarks
│   ├── graphiti/             # BFS, PageRank, shuffle, initialization model
│   └── grasp/                # DCC applications, eval.py, parse_results.py
├── test/                      # Multi-process end-to-end tests
├── CMakeLists.txt
├── Dockerfile
├── network.sh                # Network emulation for experiments
└── run.sh                    # Local multiprocess benchmark launcher
```

## Requirements

- Linux/WSL with a C++17 GNU or Clang compiler and CMake 3.14 or newer
- An x86/x86-64 processor with AES-NI and SSE4.2 support
- OpenSSL development headers and libraries, plus system threads
- Python 3 for the Graphiti module regression launcher
- OpenMP (optional)
- Internet access during initial CMake configuration to fetch `nlohmann/json` 3.11.3

EMP is built from the editable `third_party/emp-tool` submodule, pinned to
**0.2.5** (`636b07de93027901705d4d7c2a263add882c2ce6`). It is not installed or
searched for in system directories. This repository's EMP adapter does not
require GMP, NTL or Boost.

On Ubuntu/WSL:

```bash
sudo apt-get update
sudo apt-get install build-essential cmake git libssl-dev python3 ca-certificates
```

## Build

After cloning this repository, populate its pinned submodules:

```bash
git submodule update --init --recursive
cmake -S . -B build-local -DCMAKE_BUILD_TYPE=Release
cmake --build build-local --parallel 4
```

Alternatively, clone with `git clone --recurse-submodules <repository-url>`.
For a source archive without submodule metadata, obtain the same EMP sources:

```bash
git clone --depth 1 --branch 0.2.5 \
  https://github.com/emp-toolkit/emp-tool.git third_party/emp-tool
```

Executables are generated under `build-local/benchmark/`, `build-local/test/`
and `build-local/examples/`. To keep using the existing `run.sh` launcher,
which searches `build/`, configure a fresh `build/` directory instead, or run
`run.sh` from inside `build-local/` (it also searches the current directory).
Do not reuse a CMake cache configured against an installed EMP.

`cmake/LocalEMP.cmake` defines the static target `cris_emp_tool` and alias
`emp-tool::emp-tool`. Thus, existing target links remain unchanged. Neither
`cmake --install` nor `LD_LIBRARY_PATH` is needed for EMP. OpenSSL remains a
normal system dependency. The adapter builds EMP's default source list without
invoking its standalone CMake files, whose paths assume EMP is the top-level
project. It supplies `<cstdint>` for compatibility with newer compilers. The
optional EMP `CRYPTO_IN_CIRCUIT` and `THREADING` modes are not enabled.

### Editing EMP

Modify source files directly under `third_party/emp-tool/emp-tool/`, then rebuild:

```bash
cmake --build build-local --target graphiti_module --parallel 4
ctest --test-dir build-local -R '^graphiti_module_' --output-on-failure
```

CMake tracks changes to the existing sources and headers. If you add a new
`.cpp` translation unit to EMP, also add it to `cmake/LocalEMP.cmake`.
To rebuild the two graph benchmarks:

```bash
cmake --build build-local \
  --target bench_BfsMpaGraphiti bench_PrMpaGraphiti --parallel 4
```

A submodule has its own Git history. Create a branch inside EMP before saving
customizations; commit there and then update the parent repository's submodule
pointer. To share those changes, push the EMP commit to a repository accessible
to your collaborators (usually your fork) and update `.gitmodules` as needed;
a parent commit alone does not distribute modified EMP source files.

### Docker

Populate `third_party/emp-tool` before building the image. The supplied Dockerfile
builds the same local static library on Ubuntu 22.04, without installing EMP:

```bash
git submodule update --init --recursive
docker build -t cris-mpc .
docker run --rm -it cris-mpc
```

The image starts a shell in `/workspace`; programs are in `/workspace/build/`.

## Circuit API

The following example constructs a vector of multiplications and reconstructs the results:

```cpp
#include "src/common/circuit/circuit.h"
#include "src/common/protocol_runner.h"

using T = uint64_t;
using namespace threepc;

Circuit<T> circuit;
auto x = circuit.newInputWire(P0);
auto y = circuit.newInputWire(P1);
auto product = circuit.addGate(GateType::kMul, x, y);
auto opened = circuit.addRecGate(product);
circuit.setAsOutput(opened);

auto ordered = circuit.orderGatesByLevel();

protocol::ProtocolConfig config;
config.kind = protocol::ProtocolKind::Rss3;
config.pid = pid;
config.num_compute_parties = 3;
config.peer = "127.0.0.1";
config.port = 13700;

auto runner = protocol::makeProtocolRunner<T>(config);
if (pid == P0) runner->setInputs({x}, {T{6}});
if (pid == P1) runner->setInputs({y}, {T{7}});
runner->offline(ordered);
runner->online(ordered);
auto outputs = runner->getOutputs(ordered);
```

For NPH, select `ProtocolKind::Nph`, set `num_compute_parties` to at least two, and start one additional process whose PID equals `num_compute_parties`.

## Graph computation: implementation status

The Graphiti computation is also available as a reusable circuit module:
[`src/graphiti`](src/graphiti/README.md), with an independent example in
`examples/graphiti_module.cpp`. Its initialization consumes precomputed secret
order permutations; it does not implement secure sorting of private topology.

Graphiti uses shuffle-based order transitions with Propagate and Gather subcircuits. GraSP uses Decompose-Compute-Combine (DCC), Permute+Share, and amortized Permute+Share to evaluate party-specific subgraphs and combine their results.

| Component | CMake target | Current scope |
| --- | --- | --- |
| Graphiti BFS | `bench_BfsMpaGraphiti` | Bounded-hop message passing; NPH, or RSS3 with `--skip-final-applyv` |
| Graphiti PageRank | `bench_PrMpaGraphiti` | Simplified PageRank workload; RSS3 and NPH |
| Graphiti shuffle | `bench_shuffle` | Repeated grouped shuffle; RSS3 and NPH |
| Graphiti initialization | `microbench_InitGraphiti` | Synthetic shuffle/sort cost model; RSS3 and NPH |
| GraSP PageRank | `bench_PrMpaGraSP` | DCC with simplified PageRank workload; NPH |
| GraSP risk propagation | `RiskPropagation` | Transaction-weighted risk propagation; NPH |
| GraSP group connection | `GroupConnection` | Bounded-hop connection detection; NPH |


For commands and details, see the [Graphiti README](benchmark/graphiti/README.md) and [GraSP README](benchmark/grasp/README.md). The GraSP directory also contains evaluation sweeps and result-table generation.

For example, after building:

```bash
./run.sh bench_PrMpaGraphiti --protocol nph --num-parties 2 --graph-size 10000 --num-iters 10
./run.sh bench_PrMpaGraSP --protocol nph --num-parties 2 --graph-size 10000 --num-iters 10
```

## Arithmetic benchmarks

The following primitive benchmarks are available:

| Target | Purpose | Protocol support |
| --- | --- | --- |
| `bench_gate` | Vectorized add, subtract, multiply, and public-constant gates | RSS3 and NPH |
| `bench_linear` | Chains of linear arithmetic and secret multiplication | RSS3 and NPH |
| `bench_mult` | Batched secret multiplication | RSS3 |

`run.sh` launches all required processes locally, records per-process logs, and writes a summary. For example:

```bash
./run.sh bench_gate \
  --protocol rss3 \
  --gate mul --x 10 --y 20 --vec-size 1000
```

Run the same gate using five compute parties and one NPH helper:

```bash
./run.sh bench_gate \
  --protocol nph --num-parties 5 \
  --gate mul --x 10 --y 20 --vec-size 1000
```

For NPH, `--num-parties` counts compute parties only. The launcher starts the helper automatically. The optional `--pking` flag makes P0 reconstruct values and redistribute them to the other compute parties.

Run the linear arithmetic chain through RSS3:

```bash
./run.sh bench_linear \
  --protocol rss3 \
  --vec-size 1000 --chain-depth 10
```

Or run it with five NPH compute parties and one helper:

```bash
./run.sh bench_linear \
  --protocol nph --num-parties 5 \
  --vec-size 1000 --chain-depth 10
```

The older `bench_mult` program accepts only RSS3 arguments and currently needs to be launched directly as three processes. For example:

```bash
build/benchmark/bench_mult --pid 0 --vec-size 1000 &
build/benchmark/bench_mult --pid 1 --vec-size 1000 &
build/benchmark/bench_mult --pid 2 --vec-size 1000 &
wait
```

All parties must use the same `--port` and `--peer` settings. Start P0 first when launching processes manually.

### Results

By default, `run.sh` stores results under:

```text
Results/<benchmark>/protocol_<protocol>/parties_<n>/<workload>/<timestamp>/
├── meta.txt
├── party_0.log
├── ...
└── summary.txt
```

Set `RESULTS_DIR` to change the base directory:

```bash
RESULTS_DIR=/tmp/cris-mpc-results ./run.sh bench_gate \
  --gate add --x 10 --y 20 --vec-size 1000
```

If `OMP_NUM_THREADS` is unset, the launcher divides the available hardware threads among the processes to avoid local oversubscription.

## Tests

CMake builds eight multiprocess test executables under `build/test/`; they are not registered with CTest. Coverage includes RSS3 and NPH equality, signed comparison, shuffle, grouped shuffle/unshuffle, and NPH amortized Permute+Share. The application benchmarks also include cleartext correctness checks for supported workload sizes.

See [test/README.md](test/README.md) for all targets, process counts, and launch commands.

## Security model

CrIS-MPC targets semi-honest adversaries i.e. parties are assumed to follow the protocol but may inspect their local views. RSS3 targets at most one corrupted compute party among three. NPH targets up to `n-1` corrupted compute parties, with a dedicated preprocessing helper that must not collude with the compute parties. The code is intended for research and experimental benchmarking and has not been independently audited for production use.

## License

This project is licensed under the [MIT License](LICENSE).
