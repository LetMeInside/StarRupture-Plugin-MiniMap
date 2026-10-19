#pragma once
// Fixed-size diagnostics for the existing capture/atlas owners. No native handles,
// asynchronous tasks or allocations; scopes only accumulate, Flush logs unlocked.
#include <array>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#if defined(MODLOADER_CLIENT_BUILD)
#include "CallbackTiming.h"
#endif
namespace MiniMapCaptureTiming
{
#if defined(MODLOADER_CLIENT_BUILD)
    using Clock=MiniMapCallbackTiming::Clock;
    inline uint32_t Thread() noexcept {return GetCurrentThreadId();}
#else
    using Clock=std::chrono::steady_clock;
    inline uint32_t Thread() noexcept {return 0;}
#endif
    inline double Now() noexcept {return std::chrono::duration<double,std::milli>(Clock::now().time_since_epoch()).count();}
    enum class Stage : size_t { Admission,Service,Metadata,Ownership,Spawn,Construct,Setup,C0,C10,Final,
        Enqueue,ReadbackCommand,FencePoll,ReadbackCheck,Map,CopyRows,Unmap,Analyze,ReadbackCleanup,Coverage,Rgb,Compose,Process,
        Retire,Destroy,Persistence,RgbaCopy,PngReread,Header,SpriteWrite,SpriteRename,SourcePng,
        SourceAdd,Publication,Pack,Clear,OldPixels,NewPixels,LayoutCopy,Prepare,Upload,CommitWait,CommitHold,Commit,
        OldRetire,Free,AtlasExport,ExportPrep,ExportRename,
        PngPrep,PngBgra,PngEncode,PngTransfer,PngWrite,PngFlush,PngTotal,Count };
    inline constexpr const char* Names[]={"admit","service","metadata","ownership","spawn","construct","setup","C0","C10","final",
        "enqueue","readbackCommand","fencePoll","readbackCheck","map","copyRows","unmap","analyze","readbackCleanup","coverage","rgb","compose","process",
        "retire","destroy","persist","rgbaCopy","pngReread","header","spriteWrite","spriteRename","sourcePng",
        "sourceAdd","publication","pack","clear","oldPixels","newPixels","layoutCopy","prepare","upload","commitWait","commitHold","commit",
        "oldRetire","free","atlasExport","exportPrep","exportRename","pngPrep","bgra","encode","transfer","write","flush","pngTotal"};
    static_assert(std::size(Names)==size_t(Stage::Count));
    struct Sample {double Ms{},First{},Last{},Max{};uint32_t Calls{},Thread{};};
    struct UploadEvent {double Entry{},Return{};uint32_t Thread{},Result{};};
    inline constexpr size_t PngStart=size_t(Stage::PngPrep),PngCount=size_t(Stage::Count)-PngStart;
    struct Record
    {
        uint64_t Session{},Id{},Key{},Candidate{},Published{},Bytes{},ReadbackBytes{},Padded{},Frame{},Encoded[3]{};
        uint32_t Width{},Height{},InputWidth{},InputHeight{},Sources{},Probes{},Services{},Thread{},UploadResult{};
        double Begin{},End{},ReadbackLatency{},HdrLatency{},FinalLatency{},ServiceMax{};
        bool Finished=false,Ok=false;
        std::array<UploadEvent,3> Uploads{};uint32_t UploadCount{};
        std::array<Sample,PngStart> Samples{};
        std::array<std::array<Sample,PngCount>,3> Png{}; // processed payload / original source / atlas
    };
    inline thread_local Record* Current=nullptr;
    inline thread_local unsigned PngRole=0;
    inline thread_local std::array<Sample*,size_t(Stage::Count)> Active{};
    struct Context {Record* Previous=Current;explicit Context(Record& r) noexcept {Current=&r;}explicit Context(Record* r) noexcept {Current=r;}~Context(){Current=Previous;}};
    struct PngContext {unsigned Previous=PngRole;explicit PngContext(unsigned role) noexcept {PngRole=role;}~PngContext(){PngRole=Previous;}};
    inline void Add(Sample& s,double begin,double end,uint32_t thread) noexcept
    {s.Ms+=end-begin;s.Max=(end-begin>s.Max)?end-begin:s.Max;if(!s.Calls)s.First=begin;s.Last=end;++s.Calls;s.Thread=thread;}
    struct Span
    {
        Record* Owner=Current;Stage Kind;Sample* Value=nullptr;Sample* Previous=nullptr;double Begin{};uint32_t Tid{};
        explicit Span(Stage kind) noexcept:Kind(kind)
        {
            if(!Owner)return;
            auto* sample=size_t(kind)<PngStart?&Owner->Samples[size_t(kind)]:&Owner->Png[PngRole][size_t(kind)-PngStart];
            if(Active[size_t(kind)]==sample)return; // Same-category nesting counts once.
            Value=sample;Previous=Active[size_t(kind)];Active[size_t(kind)]=sample;Begin=Now();Tid=Thread();
        }
        void Stop() noexcept
        {
            if(!Value)return;
#if defined(MODLOADER_CLIENT_BUILD)
            const DWORD error=GetLastError(); // Preserve the measured Win32 call's result.
#endif
            const double end=Now();
            Add(*Value,Begin,end,Tid);
            if(Kind==Stage::Upload && Owner->UploadCount<Owner->Uploads.size())Owner->Uploads[Owner->UploadCount++]={Begin,end,Tid,0};
            Active[size_t(Kind)]=Previous;Value=nullptr;
#if defined(MODLOADER_CLIENT_BUILD)
            SetLastError(error);
#endif
        }
        ~Span(){Stop();}
        Span(const Span&)=delete;Span& operator=(const Span&)=delete;
    };
    template<class F> decltype(auto) Measure(Stage stage,F&& f) {Span timer(stage);return f();}
    inline void UploadOutcome(uint32_t value) noexcept
    {if(Current){Current->UploadResult=value;if(Current->UploadCount)Current->Uploads[Current->UploadCount-1].Result=value;}}
    inline uint64_t Key(const char* text) noexcept {uint64_t h=14695981039346656037ull;for(;*text;++text){h^=uint8_t(*text);h*=1099511628211ull;}return h;}
    inline void Merge(Record& into,const Record& from) noexcept
    {
        for(size_t i=0;i<PngStart;++i){const auto& s=from.Samples[i];if(!s.Calls)continue;auto& d=into.Samples[i];
            d.Ms+=s.Ms;d.Max=(d.Max>s.Max)?d.Max:s.Max;if(!d.Calls)d.First=s.First;d.Last=s.Last;d.Calls+=s.Calls;d.Thread=s.Thread;}
    }

    // Message text is capped at 600 bytes, leaving room for the loader's own
    // timestamp/thread/plugin prelude below the observed ~1073-byte truncation.
    inline constexpr size_t SafeLogLine=600, FormatTokenBytes=256;
    inline constexpr const char* StageGroups[]={
        "core","core","native","native","native","native","native","native","native","native",
        "readback","readback","native","readback","readback","readback","readback","readback","readback",
        "processing","processing","processing","processing","native","native",
        "persistence","persistence","persistence","persistence","persistence","persistence","source-png",
        "preparation","publication","preparation","preparation","preparation","preparation","preparation","preparation",
        "upload","publication","publication","publication","publication","publication","export","export","export",
        "png","png","png","png","png","png","png"};
    static_assert(std::size(StageGroups)==std::size(Names));
    constexpr size_t TextLength(const char* text) noexcept {size_t n=0;while(text[n])++n;return n;}
    constexpr bool BoundedLabels() noexcept
    {for(size_t i=0;i<std::size(Names);++i)if(TextLength(Names[i])>24 || TextLength(StageGroups[i])>16)return false;return true;}
    static_assert(BoundedLabels());

    // Formatting only. Sink is called once per complete short line; no output
    // line budget exists. Each sample is an indivisible, bounded token and is
    // emitted exactly once, with First/Last as well as sum/count/max/thread.
    template<class Sink> inline void FormatRecord(const Record& r,const char* kind,uint64_t dropped,Sink&& sink)
    {
        if(!kind || TextLength(kind)>24 || r.UploadCount>r.Uploads.size())
            throw std::length_error("timing record formatting identity/count invariant");
        const char* family=std::strncmp(kind,"capture",7)==0?"capture":"atlas";
        std::array<char,SafeLogLine+1> line{};
        const char* group="core";size_t used=0;unsigned part=1;bool populated=false;
        auto flush=[&]
        {
            if(!populated)return;
            sink(line.data());++part;used=0;populated=false;
        };
        auto header=[&]
        {
            if(used)return;
            const int n=std::snprintf(line.data(),line.size(),
                "MiniMap: %s timing %s: session=%llu id=%llu key=%016llX candidate=%llu published=%llu kind=%s ok=%u finished=%u part=%u scopes=inclusive-not-additive",
                family,group,r.Session,r.Id,r.Key,r.Candidate,r.Published,kind,unsigned(r.Ok),unsigned(r.Finished),part);
            // Known labels, five bounded integers and a 16-digit hash fit in
            // 320 bytes. A full sample token fits beside that header in 600.
            if(n<0 || size_t(n)>320)throw std::length_error("timing header formatting invariant");
            used=size_t(n);
        };
        static_assert(320+FormatTokenBytes<=SafeLogLine);
        auto field=[&](const char* format,auto... args)
        {
            std::array<char,FormatTokenBytes> token{};
            const int n=std::snprintf(token.data(),token.size(),format,args...);
            // %.17g bounds every double, including DBL_MAX/NaN/infinity, while
            // retaining round-trip precision. Never clamp snprintf's length.
            if(n<0 || size_t(n)>=token.size())throw std::length_error("timing token formatting invariant");
            header();
            if(used+size_t(n)>SafeLogLine){flush();header();}
            std::memcpy(line.data()+used,token.data(),size_t(n));used+=size_t(n);line[used]=0;populated=true;
        };
        auto beginGroup=[&](const char* next){flush();group=next;};
        auto sample=[&](const char* role,const char* name,const Sample& value)
        {
            if(!value.Calls)return;
            field(" %s%s{wallMs=%.17g calls=%u maxMs=%.17g thread=%u firstMonoMs=%.17g lastMonoMs=%.17g}",
                role,name,value.Ms,value.Calls,value.Max,value.Thread,value.First,value.Last);
        };
        auto stages=[&](const char* target)
        {for(size_t i=0;i<PngStart;++i)if(std::strcmp(StageGroups[i],target)==0)sample("",Names[i],r.Samples[i]);};

        field(" thread=%u uiFrame=%llu",r.Thread,r.Frame);
        field(" rawServices=%u rawServiceMaxMs=%.17g uploadResult=%u uploadCount=%u dropped=%llu zeroCalls=omitted",
            r.Services,r.ServiceMax,r.UploadResult,r.UploadCount,dropped);
        stages("core");
        beginGroup("latency");
        field(" beginMonoMs=%.17g endMonoMs=%.17g elapsedLatencyMs=%.17g",r.Begin,r.End,r.End-r.Begin);
        field(" readbackLatencyMs=%.17g asyncHDR=%.17g asyncFinal=%.17g latency=elapsed-not-sync-work",r.ReadbackLatency,r.HdrLatency,r.FinalLatency);
        beginGroup("dimensions");
        field(" input=%ux%u size=%ux%u sources=%u probes=%u",r.InputWidth,r.InputHeight,r.Width,r.Height,r.Sources,r.Probes);
        field(" rgbaBytes=%llu readbackBytes=%llu paddedPixels=%llu",r.Bytes,r.ReadbackBytes,r.Padded);
        field(" encodedPayloadBytes=%llu encodedSourceBytes=%llu encodedAtlasBytes=%llu",r.Encoded[0],r.Encoded[1],r.Encoded[2]);
        for(const char* next:{"native","readback","processing","persistence","source-png","preparation","upload","publication","export"})
        {beginGroup(next);stages(next);}
        for(size_t p=0;p<r.Png.size();++p)
        {
            beginGroup(p==0?"payload-png":p==1?"source-png":"atlas-png");
            for(size_t i=0;i<PngCount;++i)sample(p==0?"payload.":p==1?"source.":"atlas.",Names[PngStart+i],r.Png[p][i]);
        }
        beginGroup("upload-attempts");
        for(uint32_t i=0;i<r.UploadCount;++i)
        {
            const auto& u=r.Uploads[i];
            field(" attempt=%u entryMonoMs=%.17g returnMonoMs=%.17g result=%u thread=%u",i,u.Entry,u.Return,u.Result,u.Thread);
            flush(); // Independently identify each attempt, even if another would fit.
        }
        flush();
    }

#if defined(MODLOADER_CLIENT_BUILD)
    struct Pending {Record Data;const char* Kind=nullptr;};
    inline std::mutex Access;
    inline std::array<Pending,8> PendingRecords{};
    inline size_t PendingCount{};inline uint64_t Dropped{};
    inline void Defer(const Record& r,const char* kind) noexcept
    {try{std::scoped_lock lock(Access);if(PendingCount<PendingRecords.size())PendingRecords[PendingCount++]={r,kind};else ++Dropped;}catch(...) {}}
    inline void Flush() noexcept
    {
        try
        {
        {std::scoped_lock lock(Access);if(!PendingCount)return;}
        std::array<Pending,8> copy{};size_t count=0;uint64_t dropped=0;
        {std::scoped_lock lock(Access);count=PendingCount;for(size_t i=0;i<count;++i)copy[i]=PendingRecords[i];PendingCount=0;dropped=Dropped;Dropped=0;}
        try{auto* self=GetSelf();if(!self||!self->logger)return;
            for(size_t n=0;n<count;++n)
            {
                const auto& r=copy[n].Data;
                try
                {
                    FormatRecord(r,copy[n].Kind,dropped,[&](const char* line){self->logger->Info(self,"%s",line);});
                }
                catch(const std::length_error&)
                {
                    self->logger->Info(self,"MiniMap: timing format invariant failed: session=%llu id=%llu key=%016llX candidate=%llu published=%llu dropped=%llu; no silently truncated line emitted",
                        r.Session,r.Id,r.Key,r.Candidate,r.Published,dropped);
                }
            }
        }catch(...){}
        }catch(...){} // Diagnostic synchronization failure cannot escape to a callback.
    }
#else
    inline void Defer(const Record&,const char*) noexcept {}
    inline void Flush() noexcept {}
#endif
}
