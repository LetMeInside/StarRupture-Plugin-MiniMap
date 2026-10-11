#include "../Src/Experiments/SpriteCaptureCore.h"
#include <cassert>
#include <fstream>
#include <iostream>
namespace P=MiniMapSpriteCapture;
P::Candidate Candidate()
{
    P::Candidate c;c.Identity.Generation=4;c.Identity.Building=336;c.Identity.DefinitionObject=20;c.Identity.NameIndex=21;
    c.ActorObject=30;c.ActorName=31;c.Tier=P::D::Tier::High;
    c.ClassificationObserved=c.RepresentationVerified=true;c.Truncated=false;return c;
}
int main(int argc,char** argv)
{
    const P::Evidence ready{true,true,true,true,true};auto c=Candidate();
    // Unknown alone never permits capture. The named static-reference experiment
    // supplies the only override, and still requires every safety predicate.
    assert(P::Decide(c,ready).Reason==std::string("classification_unknown"));
    assert(P::Experiment::Evaluate(c,ready).Allowed());
    for(unsigned mask=0;mask<32;++mask)
    {
        const P::Evidence e{bool(mask&1),bool(mask&2),bool(mask&4),bool(mask&8),bool(mask&16)};
        assert(P::Experiment::Evaluate(c,e).Allowed()==(mask==31));
        auto excluded=c;P::K::Evidence tag;tag.InteriorTag=true;excluded.Classification=P::K::Decide(tag);
        assert(!P::Experiment::Evaluate(excluded,e).MayRetain());
        excluded.Classification.ExcludeFromMiniMap=P::K::Exclusion::Include; // Contradictory inclusion cannot override InteriorTag.
        assert(!P::Experiment::Evaluate(excluded,e).Allowed());
    }
    auto noRetention=ready;noRetention.SourcesRetained=false;
    assert(P::Experiment::Evaluate(c,noRetention).Status==P::State::NeedsRetention);
    // General admission must establish backend preflight BEFORE lifecycle
    // evidence is consumed, including when no legacy startup capture ran.
    auto general=c;general.General=true;general.Identity.Building=1001;
    auto cold=ready;cold.LifecycleReady=false;cold.SourcesRetained=false;
    assert(P::Decide(general,cold,true).Reason==std::string("capture_lifecycle_unavailable"));
    cold.LifecycleReady=true; // mocked successful idle-owner backend preflight
    assert(P::Decide(general,cold,true).Status==P::State::NeedsRetention);
    cold.SourcesRetained=true;assert(P::Decide(general,cold,true).Allowed());
    general.Classification.Reason="InteriorTag";
    assert(!P::Decide(general,cold,true).MayRetain());
    c.Classification.ExcludeFromMiniMap=P::K::Exclusion::Include;
    assert(P::Decide(c,ready).Allowed());
    auto invalid=c;invalid.Identity.Generation=0;assert(!P::Decide(invalid,ready).Allowed());
    invalid=c;invalid.Identity.DefinitionObject=-1;assert(!P::Decide(invalid,ready).Allowed());
    invalid=c;invalid.Truncated=true;assert(!P::Experiment::Evaluate(invalid,ready).MayRetain());
    invalid=c;invalid.RepresentationVerified=false;assert(!P::Experiment::Evaluate(invalid,ready).MayRetain());
    invalid=c;invalid.ClassificationObserved=false;assert(!P::Experiment::Evaluate(invalid,ready).MayRetain());
    invalid=c;invalid.ActorObject=-1;assert(!P::Experiment::Evaluate(invalid,ready).MayRetain());
    invalid=c;invalid.Tier=P::D::Tier::Mass;assert(!P::Experiment::Evaluate(invalid,ready).MayRetain());
    invalid=c;invalid.Identity.Spline=true;assert(!P::Experiment::Evaluate(invalid,ready).MayRetain());
    invalid=c;invalid.Identity.TintBits[0]=0;assert(!P::Experiment::Evaluate(invalid,ready).MayRetain());
    for(unsigned id:{4u,18u,220u,238u,317u,341u})
    {invalid=c;invalid.Identity.Building=id;assert(!P::Experiment::Evaluate(invalid,ready).Allowed());}
    const auto key=P::SessionVisualKey(c);invalid=c;++invalid.ActorNumber;assert(P::SessionVisualKey(invalid)!=key);
    invalid=c;++invalid.Identity.Generation;assert(P::SessionVisualKey(invalid)!=key);
    invalid=c;++invalid.Identity.NameNumber;assert(P::SessionVisualKey(invalid)!=key);
    const auto json=P::Json(c,P::Experiment::Evaluate(c,ready));
    assert(json.find("\"persistent_cache_key_available\":false")!=std::string::npos);
    assert(json.find("Smelter_StaticAssembly.png")!=std::string::npos);
    assert(json.find("skeletal_omitted")!=std::string::npos);
    if(argc>1){std::ofstream out(argv[1]);out<<json;assert(out.good());}
    std::cout<<"Sprite capture eligibility: precedence, stale/invalid evidence, retention, isolated experimental override, variants and session-only output passed\n";
}
