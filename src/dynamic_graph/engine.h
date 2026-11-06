#pragma once
#include "src/common/circuit/circuit.h"
#include <functional>
#include <memory>
#include <string>

namespace dynamic_graph {
using Word=std::uint64_t;
using Wire=threepc::wire_t;
using Circuit=threepc::Circuit<Word>;

class Encoding {
    unsigned ell_;
public:
    // Zero selects the smallest supported ell with 2^ell > 2*Dmax.
    explicit Encoding(Word Dmax,unsigned ell=0);
    unsigned shift() const {return ell_;}
    unsigned value_bits() const {return 64-ell_;}
    Word scale() const {return Word{1}<<ell_;}
    Word value_mask() const {return ~Word{0}>>ell_;}
    Word encode(Word local_message) const {return local_message<<ell_;}
};

// Arithmetic gates respect projection Z_(2^64) -> Z_q. Zero and sign tests
// use S*x so high representative bits cannot affect comparison in Z_q.
class RingOps {
    Circuit& c_; const Encoding& enc_;
public:
    RingOps(Circuit& c,const Encoding& enc):c_(c),enc_(enc){}
    Wire add(Wire a,Wire b){return c_.addGate(threepc::GateType::kAdd,a,b);}
    Wire sub(Wire a,Wire b){return c_.addGate(threepc::GateType::kSub,a,b);}
    Wire mul(Wire a,Wire b){return c_.addGate(threepc::GateType::kMul,a,b);}
    Wire scale(Wire a,Word k){return c_.addCGate(threepc::GateType::kCMul,a,k);}
    Wire add_public(Wire a,Word k){return c_.addCGate(threepc::GateType::kCAdd,a,k);}
    Wire is_zero(Wire a){return c_.addEqzGate(scale(a,enc_.scale()));}
    Wire negative(Wire a){return c_.addLtzGate(scale(a,enc_.scale()));}
};
using Function=std::function<Wire(RingOps&,Wire,Wire)>;
struct Program {
    Function apply_e,apply_v;
    bool count_must_fit=false;
    static Program reachability(); // bounded-hop BFS; enforces indegree-count fit
    static Program weighted_sum(); // m=h_src*w; H'=H+sum(incoming m)
};
struct Config {
    int party; std::size_t vertices; Word max_delta;
    int nph_port=19200, risa_port=19250;
    std::string peer="127.0.0.1";
    unsigned shift=0;
};
struct BaseDagShare {
    // Vertex order: V vertex rows in vertex-index order, then E edge rows.
    // Endpoint shares are canonical modulo V. data is local in Z_q.
    std::vector<Word> src,dst,data;
};

// Stateful, synchronous, three-process runtime. The helper follows public
// scheduling and MPC preprocessing, never the two-party RISA calls.
class Engine {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit Engine(std::unique_ptr<Impl>);
public:
    ~Engine();
    Engine(const Engine&)=delete;
    Engine& operator=(const Engine&)=delete;
    static std::unique_ptr<Engine> Init(Config,BaseDagShare,Program=Program::weighted_sum());
    void Insert(Word public_edge_id, Word local_src, Word local_dst, Word local_edge_data);
    void Iter();
    Word iteration() const;
    std::size_t delta_count() const;
    // Secret shares, not opened values. Helper obtains an empty vector.
    const std::vector<Word>& vertex_shares() const;
};

// Inputs are arithmetic MPC wires of the sum of the local high portions and
// of the local low portions after P0 adds S/2. Result is correct modulo q.
Wire decode_parts(Circuit&,const Encoding&,Wire high_sum,Wire low_sum);
}
