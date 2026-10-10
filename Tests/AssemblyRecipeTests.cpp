#include "../Src/Experiments/AssemblyRecipeCore.h"
#include <cassert>
#include <fstream>
#include <iostream>
namespace A=MiniMapAssemblyRecipe;
namespace C=MiniMapRepresentationCore;
static bool Near(double a,double b){return std::abs(a-b)<1e-6;}
static A::Node Node(const char* id,const char* parent="")
{A::Node n;n.Id=id;n.Component=id;n.Parent=parent;n.Scene=n.InputsValid=true;return n;}
int main(int argc,char** argv)
{
    // UE FRotator convention and independently evaluated QST vectors:
    // yaw +90 rotates (2,6,12) to (-6,2,12).
    A::Transform local,parent,out;local.T={1,2,3};local.R=A::FromRotator({90,0,0});local.S={1,2,3};
    parent.T={10,20,30};parent.R=A::FromRotator({0,90,0});parent.S={2,3,4};
    assert(A::Compose(local,parent,out));
    assert(Near(out.T[0],4)&&Near(out.T[1],22)&&Near(out.T[2],42));
    assert(out.S==A::V({2,6,12}));
    const A::Q expected{.5,-.5,.5,.5};for(size_t i=0;i<4;++i)assert(Near(out.R[i],expected[i]));
    auto multiTurn=A::FromRotator({90,720,1260});auto canonical=A::FromRotator({90,0,180});
    double dot=0;for(size_t i=0;i<4;++i)dot+=multiTurn[i]*canonical[i];assert(Near(std::abs(dot),1));
    A::Transform invalid=local;invalid.S[0]=-1;assert(!A::Compose(invalid,parent,out));
    invalid=local;invalid.S[0]=0;assert(!A::Compose(invalid,parent,out));
    invalid=local;invalid.T[0]=NAN;assert(!A::Compose(invalid,parent,out));
    invalid=local;invalid.R={0,0,0,0};assert(!A::Compose(invalid,parent,out));
    invalid=local;for(auto& v:invalid.R)v*=3;assert(A::Compose(invalid,parent,out));

    bool ambiguous=false;
    assert(A::Match({5,0},{{"root",{5,0}},{"root",{5,0}}},ambiguous)=="root"&&!ambiguous);
    assert(A::Match({5,0},{{"root",{5,0}},{"other",{5,0}}},ambiguous).empty()&&ambiguous);
    assert(A::Match({5,1},{{"root",{5,0}}},ambiguous).empty()&&!ambiguous);
    std::map<std::string,A::Node> nodes;nodes["root"]=Node("root");nodes["root"].NativeParent="native";
    nodes["a"]=Node("a","root");nodes["a"].Local.T={-231,0,300};nodes["a"].Local.R=A::FromRotator({0,180,0});
    nodes["b"]=Node("b","a");nodes["b"].Local.T={0,0,-40};
    A::Result anchored;anchored.Status="verified";anchored.Reason="verified_native_root";
    std::map<std::string,A::Result> native{{"native",anchored}};std::set<std::string> active;
    auto r=A::Resolve("b",nodes,native,active);assert(r.Status=="verified"&&Near(r.Effective.T[2],260));assert(active.empty());
    assert(A::Resolve("b",nodes,{},active).Status=="unresolved");
    nodes["root"].Parent="b";assert(A::Resolve("b",nodes,native,active).Reason=="parent_cycle");nodes["root"].Parent="";
    nodes["b"].ParentConflict=true;assert(A::Resolve("b",nodes,native,active).Status=="invalid");nodes["b"].ParentConflict=false;
    nodes["b"].Absolute=true;assert(A::Resolve("b",nodes,native,active).Status=="unsupported");nodes["b"].Absolute=false;
    nodes["b"].Socket=true;assert(A::Resolve("b",nodes,native,active).Status=="unsupported");nodes["b"].Socket=false;
    nodes["b"].TemplateAttachment=true;assert(A::Resolve("b",nodes,native,active).Status=="unsupported");nodes["b"].TemplateAttachment=false;
    size_t work=1;r=A::Resolve("b",nodes,native,active,0,&work);assert(r.Status=="truncated"&&work==0);
    assert(A::Resolve("b",nodes,native,active,A::DepthLimit).Status=="truncated");
    nodes["a"].Mesh=nodes["b"].Mesh="same_mesh";assert(nodes.size()==3&&nodes["a"].Local.T!=nodes["b"].Local.T);
    auto first=A::ProposedMaterials({{0,"default",true}},{{0,"override_a",true}});
    auto second=A::ProposedMaterials({{0,"default",true}},{{0,"override_b",true}});
    assert(first[0].Material!=second[0].Material);
    assert(A::ProposedMaterials({{0,"default",true}},{{0,"",false}})[0].Material=="default");
    assert(!A::CaptureReady(true,true,true,true,false,false));
    assert(!A::CaptureReady(false,true,true,true,true,false));
    assert(!A::CaptureReady(true,true,true,true,true,true));
    assert(A::CaptureReady(true,true,true,true,true,false));
    const auto doc=C::Object({{"schema_version","1"},{"stage",C::Quote("R2.5a")},
        {"escaped",C::Quote("quote\"\n\\")},{"transform",A::Json(out)},
        {"missing_parent_status",C::Quote(A::Resolve("b",nodes,{},active).Status)},
        {"partial_skeletal",C::Object({{"mesh_id",C::Quote("skeletal")},{"reference_pose_supported","null"}})},
        {"overflow",A::Bound(std::string(A::OutputLimit+1,'x'))}});
    if(argc>1){std::ofstream file(argv[1],std::ios::binary);file<<doc;file.close();assert(file);}
    std::cout<<"Assembly recipe tests passed: UE QST vectors, hierarchy, native matching, cycles, bounds, materials, partial status and JSON\n";
}
