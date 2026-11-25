#include <cstdint>
#include <cstring>
#include <vector>
extern "C" int pack_codes(const uint32_t* codes,int n,int J,int interval,uint8_t* out,int cap){
    uint32_t nr=0;
    for(int r=0;r<n;++r) nr+=(r%interval>=2);
    for(int j=0;j<J;++j){
        uint64_t best=UINT64_MAX;
        for(unsigned k=0;k<=24;++k){
            uint64_t cost=uint64_t(nr)*(1+k);
            for(int r=2;r<n;++r) if(r%interval>=2)cost+=codes[r*J+j]>>k;
            if(cost<best){best=cost;out[j]=k;}
        }
    }
    int payload=J+2*nr; uint64_t bit=uint64_t(payload)*8;unsigned idx=0;
    auto put=[&](unsigned v,unsigned w){for(unsigned i=0;i<w;++i,++bit)out[bit/8]|=((v>>i)&1)<<(bit%8);};
    for(int r=2;r<n;++r)if(r%interval>=2){
        uint64_t off=bit/8-payload;if(off>65535)return -1;
        uint16_t v=off;std::memcpy(out+J+2*idx++,&v,2);
        for(int j=0;j<J;++j){
            auto u=codes[r*J+j];unsigned k=out[j],q=u>>k;
            if(bit+q+1+k>uint64_t(cap)*8)return -2;
            bit+=q;put(1,1);put(u,k);
        }
        bit=(bit+7)&~7ull;
    }
    return bit/8;
}
