#pragma once
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

namespace MiniMapRepresentationCore
{
    inline constexpr size_t ComponentLimit=128,NodeLimit=256,MaterialLimit=32,
        ChildDepthLimit=4,DepthLimit=32,VisitLimit=8192,OutputLimit=1024*1024;
    struct ArrayHeader { uintptr_t Data{}; int32_t Num{},Max{}; };
    static_assert(sizeof(ArrayHeader)==16);
    struct ArrayRead
    {
        size_t Count{},Bytes{};
        const char* Status="invalid";
        bool Valid{},Truncated{};
    };
    inline ArrayRead BoundArray(ArrayHeader a,size_t stride,size_t alignment,size_t limit)
    {
        ArrayRead r;
        if(a.Num<0||a.Max<0||a.Num>a.Max||!stride||!alignment||(alignment&(alignment-1)))return r;
        if(size_t(a.Max)>(std::numeric_limits<size_t>::max)()/stride)return r;
        if(a.Data>(std::numeric_limits<uintptr_t>::max)()-size_t(a.Max)*stride)return r;
        if(a.Data&&(a.Data&(alignment-1)))return r;
        if(a.Max&&!a.Data)return r;
        if(!a.Num){r.Valid=true;r.Status="absent";return r;}
        r.Count=(std::min)(size_t(a.Num),limit);r.Bytes=r.Count*stride;r.Valid=true;
        r.Truncated=r.Count<size_t(a.Num);r.Status=r.Truncated?"truncated":"present";return r;
    }
    struct Graph
    {
        size_t Remaining=VisitLimit;
        bool Truncated=false;
        std::unordered_set<uintptr_t> Active,Seen;
        bool Spend()
        {if(Remaining){--Remaining;return true;}Truncated=true;return false;}
        const char* Enter(uintptr_t key,size_t depth,size_t limit=DepthLimit)
        {
            if(depth>limit){Truncated=true;return "truncated";}
            if(!Spend())return "truncated";
            if(Active.contains(key))return "cycle";
            if(Seen.contains(key))return "already_inspected";
            Seen.insert(key);Active.insert(key);return "present";
        }
        void Leave(uintptr_t key){Active.erase(key);}
    };
    inline std::string Quote(const std::string& s)
    {
        std::string out="\"";constexpr char hex[]="0123456789abcdef";
        for(unsigned char c:s)
        {
            if(c=='\"'||c=='\\'){out+='\\';out+=char(c);}
            else if(c<32||c>=127){out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}
            else out+=char(c);
        }
        return out+'\"';
    }
    inline std::string Number(double v)
    {
        if(!std::isfinite(v))return "null";
        char b[64]{};const auto result=std::to_chars(b,b+sizeof(b),v,std::chars_format::general,17);
        return result.ec==std::errc{}?std::string(b,result.ptr):"null";
    }
    inline std::string Object(std::initializer_list<std::pair<std::string,std::string>> fields)
    {
        std::string out="{";
        for(const auto& [k,v]:fields){if(out.size()>1)out+=',';out+=Quote(k)+":"+v;}
        return out+"}";
    }
    inline const char* Bool(bool value){return value?"true":"false";}
    struct JsonArray
    {
        std::vector<std::string> Records;
        size_t Bytes=2,Limit=64*1024;
        bool Truncated=false;
        bool Add(std::string record)
        {
            const auto extra=record.size()+(Records.empty()?0:1);
            if(extra>Limit||Bytes>Limit-extra){Truncated=true;return false;}
            Bytes+=extra;Records.push_back(std::move(record));return true;
        }
        std::string String()const
        {
            std::string out="[";
            for(const auto& r:Records){if(out.size()>1)out+=',';out+=r;}
            return out+"]";
        }
    };
}
