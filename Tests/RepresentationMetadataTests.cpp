#include "../Src/Experiments/RepresentationMetadataCore.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <cstring>
#include <array>

namespace C=MiniMapRepresentationCore;
int main(int argc,char** argv)
{
    alignas(16) std::array<uint8_t,1024> memory{};
    const auto base=reinterpret_cast<uintptr_t>(memory.data());
    auto storage=[&](uintptr_t p,size_t size,size_t alignment)
    {return p>=base&&p<=base+memory.size()&&size<=base+memory.size()-p&&!(p&(alignment-1));};
    auto reader=[&](uintptr_t p,size_t offset,auto& value)
    {
        if(p>UINTPTR_MAX-offset||!storage(p+offset,sizeof(value),1))return false;
        std::memcpy(&value,reinterpret_cast<const void*>(p+offset),sizeof(value));return true;
    };
    auto put=[&](size_t offset,auto value){assert(offset+sizeof(value)<=memory.size());std::memcpy(memory.data()+offset,&value,sizeof(value));};
    const auto object=base+0x100;
    put(0x10,base+0x40);put(0x20,int32_t(2));put(0x24,int32_t(2));put(0x28,int32_t(1));put(0x2c,int32_t(1));
    put(0x40,base+0x80);put(0x80,object);put(0x10c,int32_t(0));
    auto valid=[&]{return C::ValidateObjectIdentity(object,base,0x28,reader,storage);};
    assert(valid()); // Numeric readability alone is insufficient: slot must agree.
    put(0x80,object+8);assert(!valid());put(0x80,object);
    put(0x88,uint32_t(0x10000000));assert(!valid());put(0x88,uint32_t(0x00200000));assert(!valid());put(0x88,uint32_t(0));
    for(uint32_t flag:{0x400u,0x1000u,0x2000u,0x8000u,0x10000u}){put(0x108,flag);assert(!valid());}
    put(0x108,uint32_t(0));assert(valid());
    put(0x10c,int32_t(-1));assert(!valid());put(0x10c,int32_t(2));assert(!valid());put(0x10c,int32_t(0));
    put(0x24,int32_t(3));assert(!valid());put(0x24,int32_t(2));
    put(0x28,int32_t(-1));assert(!valid());put(0x28,int32_t(1));
    put(0x2c,int32_t(0));assert(!valid());put(0x2c,int32_t(1));
    put(0x40,uintptr_t(UINTPTR_MAX-7));assert(!valid());put(0x40,base+0x80);
    assert(!C::ValidateObjectIdentity(object+1,base,0x28,reader,storage));
    assert(!C::ValidateObjectIdentity(object,base,2048,reader,storage));
    assert(valid());
    assert(!C::BoundArray({8,-1,0},8,8,4).Valid);
    assert(!C::BoundArray({8,1,-1},8,8,4).Valid);
    assert(!C::BoundArray({8,2,1},8,8,4).Valid);
    assert(!C::BoundArray({0,1,1},8,8,4).Valid);
    assert(!C::BoundArray({9,1,1},8,8,4).Valid);
    assert(!C::BoundArray({8,1,1},8,3,4).Valid);
    assert(!C::BoundArray({8,1,1},0,8,4).Valid);
    assert(!C::BoundArray({UINTPTR_MAX-7,2,2},8,8,4).Valid);
    assert(!C::BoundArray({8,1,2},SIZE_MAX,8,4).Valid);
    assert(!C::BoundArray({0,0,1},8,8,4).Valid);
    assert(C::BoundArray({0,0,0},8,8,4).Valid);
    auto a=C::BoundArray({16,8,12},8,8,3);
    assert(a.Valid&&a.Truncated&&a.Count==3&&a.Bytes==24);
    assert(std::string(a.Status)=="truncated");
    a=C::BoundArray({16,2,12},8,8,3);
    assert(a.Valid&&!a.Truncated&&a.Count==2&&a.Bytes==16);
    a=C::BoundArray({16,2,12},8,8,0);
    assert(a.Valid&&a.Truncated&&a.Count==0);

    C::Graph graph;
    assert(std::string(graph.Enter(1,0))=="present");
    assert(std::string(graph.Enter(2,1))=="present");
    assert(std::string(graph.Enter(1,2))=="cycle");
    graph.Leave(2);graph.Leave(1);
    assert(std::string(graph.Enter(2,0))=="already_inspected");
    assert(std::string(graph.Enter(3,5,4))=="truncated"&&graph.Truncated);
    C::Graph budget;budget.Remaining=2;
    assert(budget.Spend()&&budget.Spend()&&!budget.Spend()&&budget.Truncated);

    // Synthetic graph uses the same bounded array and graph gates as runtime.
    // A cycle and a repeated subtree must terminate without losing provenance.
    struct Node { size_t Id;std::vector<Node*> Children; };
    Node root{1},child{2},tail{3};root.Children={&child,&tail};child.Children={&root,&tail};
    C::Graph traversal;C::JsonArray edges;size_t visited=0,cycles=0,repeated=0;
    auto walk=[&](auto&& self,Node* node,size_t depth)->void
    {
        const auto status=std::string(traversal.Enter(node->Id,depth));
        if(status=="cycle"){++cycles;return;}
        if(status=="already_inspected"){++repeated;return;}
        if(status!="present")return;
        ++visited;
        auto array=C::BoundArray({reinterpret_cast<uintptr_t>(node->Children.data()),int32_t(node->Children.size()),int32_t(node->Children.capacity())},sizeof(Node*),alignof(Node*),8);
        assert(array.Valid);
        for(size_t i=0;i<array.Count&&traversal.Spend();++i)
        {
            auto* next=node->Children[i];
            edges.Add(C::Object({{"from",std::to_string(node->Id)},{"to",std::to_string(next->Id)}}));
            self(self,next,depth+1);
        }
        traversal.Leave(node->Id);
    };
    walk(walk,&root,0);
    assert(visited==3&&cycles==1&&repeated==1&&edges.Records.size()==4);
    assert(!traversal.Truncated&&traversal.Active.empty());

    assert(C::Quote("a\"\\\n\r\t")=="\"a\\\"\\\\\\u000a\\u000d\\u0009\"");
    std::string controls;for(int i=0;i<256;++i)controls+=char(i);
    const auto quoted=C::Quote(controls);
    assert(quoted.find("\\u0000")!=std::string::npos&&quoted.find("\\u00ff")!=std::string::npos);
    assert(C::Number(std::numeric_limits<double>::infinity())=="null");
    assert(C::Number(std::numeric_limits<double>::quiet_NaN())=="null");
    assert(C::Number(0.25)=="0.25");
    C::JsonArray small;small.Limit=7;
    assert(small.Add("1")&&small.Add("22")&&!small.Add("333"));
    assert(small.String()=="[1,22]"&&small.Truncated);
    auto fallback=C::BoundDocument(std::string(C::OutputLimit+1,'x'));
    assert(fallback.size()<C::OutputLimit&&fallback.find("output_byte_limit")!=std::string::npos);
    assert(C::BoundDocument("123456789",2)=="{}");
    const auto document=C::Object({{"controls",quoted},{"nonfinite",C::Number(NAN)},
        {"relationships",edges.String()},{"partial",small.String()},{"truncated",C::Bool(small.Truncated)},
        {"overflow_document",fallback}});
    if(argc>1){std::ofstream out(argv[1],std::ios::binary);out<<document;out.close();assert(out);}
    std::cout<<"Representation metadata: array, graph, cycle, depth, budget, JSON escaping and output truncation tests passed\n";
}
