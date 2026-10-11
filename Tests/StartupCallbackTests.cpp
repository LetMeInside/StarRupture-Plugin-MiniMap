#include "../Src/CallbackTiming.h"
#include "../Src/Experiments/SpriteDiscoveryCore.h"
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <cstdarg>
IPluginSelf* current=nullptr;
IPluginSelf* GetSelf(){return current;}
bool bodyExited=false;size_t finishLogs=0;
void Log(const IPluginSelf*,const char* format,...)
{
    if(std::string(format).find("took")!=std::string::npos){assert(bodyExited);++finishLogs;}
}
void Early()
{
    MiniMapCallbackTiming::Scope timing(MiniMapCallbackTiming::Id::WorldBegin,false);
    struct Finish{~Finish(){bodyExited=true;}} finish;
    return;
}
void Throw()
{
    MiniMapCallbackTiming::Scope timing(MiniMapCallbackTiming::Id::ExperienceComplete,false);
    struct Finish{~Finish(){bodyExited=true;}} finish;
    throw std::runtime_error("expected");
}
int main()
{
    IPluginLogger logger{};logger.Info=&Log;IPluginSelf self{};self.logger=&logger;current=&self;
    Early();bodyExited=false;try{Throw();}catch(const std::runtime_error&){}
    assert(finishLogs==2);
    using namespace MiniMapCallbackTiming;
    assert(Samples[size_t(Id::WorldBegin)].Calls==1&&Samples[size_t(Id::ExperienceComplete)].Calls==1);
    for(int i=0;i<10;++i){Scope timing(Id::DiscoveryTick,true);}
    assert(Samples[size_t(Id::DiscoveryTick)].Calls==10&&finishLogs==2);
    const auto& s=Samples[size_t(Id::DiscoveryTick)];assert(s.Min<=s.Last&&s.Last<=s.Max&&s.Total>=s.Max);
    namespace D=MiniMapSpriteDiscoveryCore;
    D::Registry registry;registry.Begin(1,20,true);
    auto get=[](size_t){D::Input in;in.Identity.Generation=1;in.Identity.Building=336;in.Identity.DefinitionObject=1;in.Identity.NameIndex=1;in.DefinitionValid=true;return in;};
    assert(!registry.Step(get,0));assert(registry.Cursor==1); // yields, does not restart
    while(!registry.Finished)registry.Step(get,0);
    assert(registry.RepresentedInstances==20&&registry.Count==1&&registry.Ticks==20);
    current=nullptr;
    {Scope timing(Id::PluginInfo,false);}
    assert(PreLoggerCount==1);
    current=&self;Report(current,true);assert(PreLoggerCount==0&&finishLogs==3);
    current=nullptr;
    std::cout<<"Callback early-return/unwind/end ordering, per-call aggregation and elapsed-budget cursor continuation passed\n";
}
