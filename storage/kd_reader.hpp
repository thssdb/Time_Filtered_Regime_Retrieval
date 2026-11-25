#pragma once
#include "aligned_pages.hpp"
#include <cstdlib>
namespace nanhang_weighted_fetch {
using Wide=__int128_t;
template<class T> inline T little(const Byte* p){T v;std::memcpy(&v,p,sizeof(v));return v;}
inline Wide signed_value(const Byte* p,unsigned w){
    switch(w){case 1:return little<int8_t>(p);case 2:return little<int16_t>(p);case 4:return little<int32_t>(p);case 8:return little<int64_t>(p);case 16:return little<Wide>(p);default:fail("invalid integer width");}
}
inline uint32_t key_value(const Byte* p,unsigned w){
    if(w==1)return *p;if(w==2)return little<uint16_t>(p);require(w==4,"key width");return little<uint32_t>(p);
}
struct KDStream {
    const Byte *ptr,*end;unsigned bit=0;
    uint64_t word() const{
        const Byte* p=ptr+(bit>>3);uint64_t x=0;
        if(end-p>=8)std::memcpy(&x,p,8);else {require(end>p,"short Rice stream");std::memcpy(&x,p,end-p);}
        return x>>(bit&7);
    }
    unsigned get(unsigned k){
        unsigned q=0;auto x=word();while(x==0){unsigned step=64-(bit&7);q+=step;bit+=step;x=word();}
        unsigned z=std::countr_zero(x);bit+=z+1;
        unsigned tail=k?unsigned(word()&((uint64_t(1)<<k)-1)):0;bit+=k;
        return ((q+z)<<k)|tail;
    }
};
struct KDColumn {
    unsigned aw,dw,kw; double scale;
    std::vector<int64_t> small;std::vector<Wide> large;
    void open(const fs::path& file){
        Mapping m(file);Span s{m.data(),m.data()+m.size()};
        require(!std::memcmp(s.take(4),"KDG1",4),"dictionary magic");
        unsigned e=s.byte();scale=1;for(unsigned j=0;j<e;j++)scale*=10;
        aw=s.byte();dw=s.byte();kw=s.byte();auto n=little<uint32_t>(s.take(4));
        for(unsigned i=0;i<n;++i){auto x=signed_value(s.take(dw),dw);if(dw<=8&&aw<=8)small.push_back((int64_t)x);else large.push_back(x);}
        require(!s.left(),"dictionary trailer");
    }
};
struct KDPage {
    const Byte *anchors,*references,*params,*offsets,*payload,*end;
    bool previous;unsigned interval,shift,rows;
    void open(const Page& src,const KDColumn& d,unsigned n){
        require(src.codec==0,"KD16 TsFile page is not uncompressed");Span s{src.compressed,src.compressed+src.size};
        const Byte* valid=nullptr;require(bitmap(s,valid)==n*kSamples,"aligned length");
        require(!std::memcmp(s.take(4),"VDR1",4),"page magic");previous=s.byte()!=0;interval=s.byte();require(interval>=2 && interval<=32 && std::has_single_bit(interval),"restart interval");shift=std::countr_zero(interval);rows=n;
        unsigned groups=(n+interval-1)>>shift;unsigned direct=(n>>shift)*2+std::min(2u,n&(interval-1));
        anchors=s.take(groups*kSamples*d.aw);references=s.take(groups*kSamples*d.kw);
        params=s.take(kSamples);offsets=s.take(2*(n-direct));payload=s.p;end=s.end;
    }
    template<bool Previous,class I> void read(const KDColumn& d,unsigned row,I* out) const{
        const unsigned group=row>>shift, rem=row&(interval-1);
        const Byte* anchor=anchors+group*kSamples*d.aw;
        const Byte* ref=references+group*kSamples*d.kw;
        const auto& dict=[&]() -> const std::vector<I>& {
            if constexpr(sizeof(I)==8)return d.small;else return d.large;
        }();
        for(unsigned j=0;j<kSamples;++j)out[j]=I(signed_value(anchor+j*d.aw,d.aw));
        if(rem==0)return;
        std::array<int32_t,kSamples> first_key;
        for(unsigned j=0;j<kSamples;++j)first_key[j]=key_value(ref+j*d.kw,d.kw);
        if(Previous || rem==1){
            for(unsigned j=0;j<kSamples;++j)out[j]+=dict[first_key[j]];
        }
        // Anchor residuals need just the target row; adjacent residuals need its prefix.
        const unsigned begin=Previous?2:std::max(2u,rem);
        for(unsigned off=begin;off<=rem;++off){
            const unsigned idx=group*(interval-2)+off-2;
            KDStream bits{payload+little<uint16_t>(offsets+2*idx),end};
            for(unsigned j=0;j<kSamples;++j){
                const int key=first_key[j]+decode_zigzag(bits.get(params[j]));
                out[j]+=dict[key];
            }
        }
    }
    void values(const KDColumn& c,unsigned row,float* out) const{
        if(c.large.empty()){
            std::array<int64_t,kSamples> a;if(previous)read<true>(c,row,a.data());else read<false>(c,row,a.data());
            for(unsigned j=0;j<kSamples;++j)out[j]=float(double(a[j])/c.scale);
        }else{
            std::array<Wide,kSamples> a;if(previous)read<true>(c,row,a.data());else read<false>(c,row,a.data());
            for(unsigned j=0;j<kSamples;++j)out[j]=float(double(a[j])/c.scale);
        }
    }
};
class KDReader {
    static constexpr unsigned G=kDimensions/kSamples;
    AddressFile loc_;
    std::array<KDColumn,G> cols_;
    std::vector<std::unique_ptr<Mapping>> maps_;
    std::vector<Block> blocks_;
    std::vector<std::array<KDPage,G>> pages_;
    std::vector<Byte> scratch_;
public:
    KDReader(std::string_view,std::string_view db,const fs::path& root):loc_(root/db/"sim_common/address.bin"){
        const char* base = std::getenv("KD_STORE");
        require(base && *base, "set KD_STORE");
        auto dir=fs::path(base)/db;
        for(unsigned c=0;c<G;++c)cols_[c].open(dir/(std::to_string(c)+".dict"));
        std::map<int,Block> by_id;
        for(const auto& p:files_with_suffix(dir/"data",".tsfile")){
            maps_.push_back(std::make_unique<Mapping>(p));index_file(*maps_.back(),by_id);
        }
        for(auto& [id,b]:by_id){
            b.count=ids_from(b.ids,scratch_).size();if(b.template_groups.empty() && !b.plain_time && b.time.codec==0) index_time_restarts(b);
            std::array<KDPage,G> ps;
            for(unsigned c=0;c<G;++c)ps[c].open(b.signals[c],cols_[c],b.count);
            pages_.push_back(ps);blocks_.push_back(std::move(b));
        }
        require(loc_.blocks==blocks_.size(),"block count");
    }
    size_t rows()const{return loc_.rows;}
    void ts(uint32_t rank,int64_t* out){
        require(rank<loc_.rows,"timestamp rank outside locator");
        const auto a=loc_.locations[rank];
        read_times(blocks_[a>>14],(a>>7)&127,out,scratch_);
    }
    void vals(uint32_t rank,float* out) const{
        require(rank<loc_.rows,"value rank outside locator");
        auto a=loc_.locations[rank];for(unsigned c=0;c<G;++c)pages_[a>>14][c].values(cols_[c],a&127,out+kSamples*c);
    }
    void fetch_batch(const uint32_t* ranks,size_t n,Returned54* out,FetchTiming&){
        for(size_t i=0;i<n;++i){
            require(ranks[i] < loc_.rows, "time rank out of range");
            auto a=loc_.locations[ranks[i]];
            read_times(blocks_[a>>14],(a>>7)&127,out[i].times.data(),scratch_);vals(ranks[i],out[i].values.data());}
    }
};
}
