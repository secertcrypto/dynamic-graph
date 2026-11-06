#include "engine.h"
#include "dynamic_graph/delta_access.h"
#include "src/graphiti/message_passing.h"
#include "src/nph/arith/online_evaluator.h"
#include "src/nph/arith/offline_evaluator.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace dynamic_graph {
using threepc::GateType;
using Words=std::vector<Word>;
using Wires=graphiti::Wires;

Encoding::Encoding(Word d,unsigned ell):ell_(ell) {
    if(d >= (Word{1}<<62))throw std::invalid_argument("Dmax too large for a nontrivial value ring");
    if(!ell_){ell_=1;while((Word{1}<<ell_)<=2*d)++ell_;}
    if(ell_>63 || scale()<=2*d)throw std::invalid_argument("require 1<=ell<=63 and 2^ell>2*Dmax");
}

Wire decode_parts(Circuit& c,const Encoding& e,Wire hi,Wire lo) {
    // lo in [0,2S-2], hence lo-S fits signed int64 even at ell=63.
    auto lt=c.addLtzGate(c.addCGate(GateType::kCSub,lo,e.scale()));
    auto carry=c.addCGate(GateType::kCSub,lt,Word{1},true);
    return c.addGate(GateType::kAdd,hi,carry);
}

Program Program::weighted_sum(){
    return {[](RingOps& op,Wire h,Wire w){return op.mul(h,w);},
            [](RingOps& op,Wire old,Wire a){return op.add(old,a);}};
}

Program Program::reachability(){
    return {[](RingOps& op,Wire h,Wire){
                return op.add_public(op.scale(op.is_zero(h),~Word{0}),1);
            },
            [](RingOps& op,Wire old,Wire a){
                auto neither=op.mul(op.is_zero(old),op.is_zero(a));
                return op.add_public(op.scale(neither,~Word{0}),1);
            },true};
}

namespace {
// Every phase uses the existing backend on the same NetNP, with independent
// preprocessing and PRGs. Export wires locally; getOutputs() would open them.
class Phase {
    int pid_;threepc::nph::NetNP& net_;
    Wires inputs_;Words values_;
public:
    Circuit c;
    Phase(int pid,threepc::nph::NetNP& net):pid_(pid),net_(net){}
    Wire shared(Word local) {
        auto a=c.newInputWire(0),b=c.newInputWire(1);
        if(pid_<2){inputs_.push_back(pid_==0?a:b);values_.push_back(local);}
        return c.addGate(GateType::kAdd,a,b);
    }
    Wires shared(const Words& local){
        Wires out;out.reserve(local.size());
        for(auto v:local)out.push_back(shared(v));return out;
    }
    Words run(const Wires& outs){
        auto ordered=c.orderGatesByLevel();
        threepc::nph::OfflineEvaluator<Word> off(pid_,2,net_);
        off.run(ordered);
        threepc::nph::OnlineEvaluator<Word> on(pid_,2,net_,off.take_preprocessing(),off.take_pairwise_prg());
        if(pid_<2)on.setInputs(inputs_,values_);
        on.evaluate(ordered);
        Words result(outs.size(),0);
        if(pid_<2)for(size_t i=0;i<outs.size();++i)result[i]=on.getShareValue(outs[i]);
        return result;
    }
};

// Privately reduce the canonical endpoint shares mod V and extract MSB-first
// bits for the existing secure stable radix-sort circuit. All comparisons are
// between small integers; no signed overflow in the LTZ gates.
Wires ranking(Phase& p,const Words& local,size_t V,bool source) {
    unsigned bits=0;while((Word{1}<<bits)<V)++bits;
    Wires keys;keys.reserve(local.size()*(bits+1));
    for(size_t i=0;i<local.size();++i){
        auto s=p.shared(local[i]);
        auto ge=[&](Wire a,Word v){
            return p.c.addCGate(GateType::kCSub,
                p.c.addLtzGate(p.c.addCGate(GateType::kCSub,a,v)),Word{1},true);
        };
        auto rem=p.c.addGate(GateType::kSub,s,p.c.addCGate(GateType::kCMul,ge(s,V),Word(V)));
        for(unsigned j=bits;j>0;--j){
            Word place=Word{1}<<(j-1);auto bit=ge(rem,place);keys.push_back(bit);
            rem=p.c.addGate(GateType::kSub,rem,p.c.addCGate(GateType::kCMul,bit,place));
        }
        auto zero=p.c.addGate(GateType::kSub,s,s);
        keys.push_back(p.c.addCGate(GateType::kCAdd,zero,Word(source ? i>=V : i<V)));
    }
    return p.c.addSortSubcircuit(keys,local.size());
}

void append(Wires& out,const Wires& values){out.insert(out.end(),values.begin(),values.end());}
}

struct Engine::Impl {
    Config cfg;
    Encoding enc;
    BaseDagShare dag;
    Program app;
    std::unique_ptr<threepc::nph::NetNP> net;
    std::unique_ptr<emp::NetIO> risa_io;
    std::unique_ptr<risa::Backend> risa_backend;
    std::unique_ptr<DeltaAccess> access;
    Words vs,sd,dv,source_weights,h;
    std::map<Word,Word> edge_data;
    Word t=0;bool poisoned=false;
    Impl(Config conf,BaseDagShare rows,Program prog)
        :cfg(std::move(conf)),enc(cfg.max_delta,cfg.shift),dag(std::move(rows)),app(std::move(prog)){
        auto n=dag.data.size();
        if(cfg.party<0||cfg.party>2 || !cfg.vertices || cfg.vertices>(Word{1}<<22) ||
           n<cfg.vertices || n>INT_MAX || dag.src.size()!=n || dag.dst.size()!=n ||
           !app.apply_e || !app.apply_v)throw std::invalid_argument("invalid graph dimensions/program");
        if(app.count_must_fit && n-cfg.vertices+cfg.max_delta>enc.value_mask())
            throw std::invalid_argument("BFS requires base edges + Dmax < q to keep counts unambiguous");
        if(cfg.nph_port<1024||cfg.nph_port>65535-2049||cfg.risa_port<1024||cfg.risa_port>65535)
            throw std::invalid_argument("invalid network ports");
        for(int i=0;i<3;++i)for(int j=0;j<2;++j)
            if(cfg.risa_port==cfg.nph_port+1024*i+j)throw std::invalid_argument("RISA and NPH ports collide");
        if(cfg.party<2)for(size_t i=0;i<n;++i)
            if(dag.src[i]>=cfg.vertices || dag.dst[i]>=cfg.vertices)
                throw std::invalid_argument("base index share outside Z_N");
        for(auto& value:dag.data)value=cfg.party==2?0:value&enc.value_mask();
        h.assign(dag.data.begin(),dag.data.begin()+cfg.vertices);
        net=std::make_unique<threepc::nph::NetNP>(cfg.party,3,cfg.peer,cfg.nph_port);
        Phase init(cfg.party,*net);
        auto s=ranking(init,dag.src,cfg.vertices,true);
        auto d=ranking(init,dag.dst,cfg.vertices,false);
        Wires ids;
        auto zero=init.c.addGate(GateType::kSub,s[0],s[0]);
        for(size_t i=0;i<n;++i)ids.push_back(init.c.addCGate(GateType::kCAdd,zero,Word(i)));
        auto sd_wire=init.c.addApplyPermSubcircuit(s,d,init.c.freshPermGroupId());
        auto dv_wire=init.c.addApplyPermSubcircuit(d,ids,init.c.freshPermGroupId());
        auto weights=init.shared(dag.data);
        auto aligned=init.c.addApplyPermSubcircuit(s,weights,init.c.freshPermGroupId());
        Wires ordered=s;append(ordered,sd_wire);append(ordered,dv_wire);append(ordered,aligned);
        auto out=init.run(ordered);
        vs.assign(out.begin(),out.begin()+n);
        sd.assign(out.begin()+n,out.begin()+2*n);dv.assign(out.begin()+2*n,out.begin()+3*n);
        source_weights.assign(out.begin()+3*n,out.end());
        if(cfg.party<2){
            risa_io=std::make_unique<emp::NetIO>(cfg.party==0?nullptr:cfg.peer.c_str(),cfg.risa_port);
            risa_backend=std::make_unique<risa::Backend>(cfg.party,*risa_io);
            access=std::make_unique<DeltaAccess>(*risa_backend,cfg.vertices,enc.value_bits());
        }
    }
    Words base_aggregate(){
        Phase p(cfg.party,*net);RingOps ops(p.c,enc);
        graphiti::MessagePassing<Word> graph(p.c,cfg.vertices,dag.data.size()-cfg.vertices);
        graph.init(p.shared(h),{p.shared(vs),p.shared(sd),p.shared(dv)});
        auto w=p.shared(source_weights);
        auto aggregate=graph.aggregate([&](Circuit&,const Wires& src){
            Wires msgs;msgs.reserve(src.size());
            for(size_t i=0;i<src.size();++i)msgs.push_back(app.apply_e(ops,src[i],w[i]));
            return msgs;
        });
        return p.run(aggregate);
    }
    Words delta_aggregate(){
        if(edge_data.empty())return Words(cfg.vertices,0);
        std::map<Word,Word> sources;
        if(access)sources=access->read_sources(t,h);
        Phase p(cfg.party,*net);RingOps ops(p.c,enc);Wires msgs;
        for(const auto& [id,value]:edge_data) {
            auto source=p.shared(access?sources.at(id):0), weight=p.shared(value);
            msgs.push_back(app.apply_e(ops,source,weight));
        }
        auto local=p.run(msgs);
        if(!access)return Words(cfg.vertices,0);
        std::map<Word,Word> encoded;size_t i=0;
        for(const auto& edge:edge_data)encoded.emplace(edge.first,enc.encode(local[i++]));
        auto delta=access->gather(t,encoded);
        if(delta.max_coordinate_error>cfg.max_delta)throw std::logic_error("rounding bound exceeded");
        return std::move(delta.shares);
    }
    void iter(){
        auto base=base_aggregate();auto delta=delta_aggregate();
        Phase p(cfg.party,*net);RingOps ops(p.c,enc);Wires next;
        for(size_t v=0;v<cfg.vertices;++v) {
            auto total=p.shared(base[v]);
            if(!edge_data.empty()){
                Word w=delta[v]+(cfg.party==0 ? enc.scale()/2 : 0);
                auto hi=p.shared(w>>enc.shift());
                auto lo=p.shared(w&(enc.scale()-1));
                total=ops.add(total,decode_parts(p.c,enc,hi,lo));
            }
            auto old=p.shared(h[v]);
            next.push_back(app.apply_v(ops,old,total));
        }
        auto updated=p.run(next);
        for(auto& v:updated)v&=enc.value_mask();
        std::copy(updated.begin(),updated.end(),dag.data.begin());
        h.swap(updated);++t;
    }
};

std::unique_ptr<Engine> Engine::Init(Config cfg,BaseDagShare dag,Program app){
    return std::unique_ptr<Engine>(new Engine(std::make_unique<Impl>(std::move(cfg),std::move(dag),std::move(app))));
}
Engine::Engine(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
Engine::~Engine()=default;
void Engine::Insert(Word id,Word src,Word dst,Word data){
    auto& p=*impl_;
    if(p.poisoned)throw std::logic_error("discard failed graph session");
    if(p.edge_data.count(id))throw std::invalid_argument("duplicate delta handle");
    if(p.edge_data.size()>=p.cfg.max_delta)throw std::length_error("public delta edge bound reached");
    if(p.access && (src>=p.cfg.vertices || dst>=p.cfg.vertices))throw std::invalid_argument("invalid delta index share");
    try{
        if(p.access)p.access->insert(id,src,dst);
        p.edge_data.emplace(id,p.access?data&p.enc.value_mask():0);
    }catch(...){p.poisoned=true;throw;}
}
void Engine::Iter(){
    auto& p=*impl_;
    if(p.poisoned||p.t==~Word{0})throw std::logic_error("failed session or exhausted iterations");
    try{p.iter();}catch(...){p.poisoned=true;throw;}
}
Word Engine::iteration()const{return impl_->t;}
size_t Engine::delta_count()const{return impl_->edge_data.size();}
const Words& Engine::vertex_shares()const{
    static const Words empty;
    return impl_->access ? impl_->h : empty;
}
}
