#pragma once
#include "DefinitionAcquisitionCore.h"
#include <array>
#include <string>

namespace MiniMapDefinitionDiagnostic
{
    namespace D=MiniMapDefinitionAcquisition;
    // CPU identity only. In particular, no configuration/class/string pointer
    // or borrowed UObject address is retained between native opportunities.
    struct Identity
    {
        bool Present=false,Validated=false;
        int32_t Index=-1,Serial{},NameIndex=-1;uint32_t NameNumber{};
        bool operator==(const Identity&)const=default;
    };
    struct Observation
    {
        D::Token Token;Identity Config,AuthoredClass,High,Low;
        std::array<int32_t,6> AuthoredReference{};
        std::array<int32_t,2> SubpathBounds{};
        std::string CanonicalBlueprint,HighStatus,LowStatus,AuthoredStatus;
        unsigned Route{};
        bool SameSource(const Observation& b)const
        {return AuthoredClass==b.AuthoredClass&&High==b.High&&Low==b.Low&&AuthoredReference==b.AuthoredReference&&
            SubpathBounds==b.SubpathBounds&&CanonicalBlueprint==b.CanonicalBlueprint&&Route==b.Route&&
            HighStatus==b.HighStatus&&LowStatus==b.LowStatus&&AuthoredStatus==b.AuthoredStatus;}
    };
    enum class Event { First,Unchanged,LifetimeReplaced,SerialAssigned,ConfigurationChanged,SourceChanged,Retry };
    inline const char* Label(Event e)
    {switch(e){case Event::First:return "first";case Event::LifetimeReplaced:return "lifetime_replaced";
        case Event::SerialAssigned:return "serial_became_nonzero";case Event::ConfigurationChanged:return "configuration_changed";
        case Event::SourceChanged:return "representation_metadata_changed";case Event::Retry:return "retry_existing_visit";
        default:return "unchanged";}}
    struct Sample
    {bool Used=false;Observation Last;unsigned Comparisons{};D::Status LastStatus=D::Status::Success;};
    struct Selection {Sample* Record{};Event Why=Event::Unchanged;bool PreviouslySampled=false;};
    struct Session
    {
        // Five reserved diagnostic target slots, three general slots; at most
        // two samples per slot (16 total). Replacement tokens reuse a slot.
        // These diagnostic bounds never affect production work.
        std::array<Sample,8> Samples;
        uint64_t Generation{};bool SummaryWritten=false;
        size_t Observed{},Zero{},Nonzero{},Attempts{},Matches{},Mismatches{},CompletedScopes{},
            ConfigChanges{},SourceChanges{},Replacements{},Unchanged{},Suppressed{},Interruptions{},TokenReadFailures{},ZeroSkips{};
        std::array<size_t,size_t(D::Status::Interrupted)+1> Rejections{};
        static size_t Target(uint32_t id)
        {switch(id){case 11:return 0;case 227:return 1;case 229:return 2;case 336:return 3;case 238:return 4;default:return 8;}}
        Selection Select(const Observation& o)
        {
            if(Generation!=o.Token.Generation){*this=Session{};Generation=o.Token.Generation;}
            size_t i=Target(o.Token.Building);
            if(i==8){for(i=5;i<8;++i)if(Samples[i].Used&&Samples[i].Last.Token.Building==o.Token.Building&&
                Samples[i].Last.Token.NameIndex==o.Token.NameIndex&&Samples[i].Last.Token.NameNumber==o.Token.NameNumber)break;
                if(i==8)for(i=5;i<8&&Samples[i].Used;++i){} }
            if(i==8){++Suppressed;return {};}
            auto& s=Samples[i];const bool previous=s.Used;Event event=Event::First;
            const auto& old=s.Last.Token;
            const bool sameLifetime=previous&&old.Index==o.Token.Index&&old.Serial==o.Token.Serial;
            const bool configChanged=sameLifetime&&s.Last.Config!=o.Config;
            const bool sourceChanged=sameLifetime&&!s.Last.SameSource(o);
            if(previous){
                if(old.Index!=o.Token.Index||old.NameIndex!=o.Token.NameIndex||old.NameNumber!=o.Token.NameNumber||
                    (old.Serial!=o.Token.Serial&&old.Serial!=0))event=Event::LifetimeReplaced;
                else if(old.Serial==0&&o.Token.Serial>0)event=Event::SerialAssigned;
                else if(s.Last.Config!=o.Config)event=Event::ConfigurationChanged;
                else if(!s.Last.SameSource(o))event=Event::SourceChanged;
                else if(s.LastStatus!=D::Status::Success&&old.Serial>0&&s.Comparisons<2)event=Event::Retry;
                else {++Unchanged;return {};}
            }
            if(s.Comparisons>=2){++Suppressed;return {};}
            s.Last=o;s.Used=true;++s.Comparisons;
            if(event==Event::First||event==Event::LifetimeReplaced||event==Event::SerialAssigned){++Observed;if(o.Token.Serial==0)++Zero;else ++Nonzero;}
            if(configChanged)++ConfigChanges;
            if(sourceChanged)++SourceChanges;
            if(event==Event::LifetimeReplaced)++Replacements;
            return {&s,event,previous};
        }
        void Complete(Sample& s,D::Status status,bool matched,const D::ScopeObservation& scope)
        {
            s.LastStatus=status;
            if(status!=D::Status::Success)++Rejections[size_t(status)];else if(matched)++Matches;else ++Mismatches;
            if(scope.Retained==1&&scope.ReleaseReached==1&&scope.ReleaseReturned==1)++CompletedScopes;
            if(scope.Interrupted||status==D::Status::Interrupted)++Interruptions;
        }
    };
}
