#include "kd_reader.hpp"
#include <iostream>
#include <sstream>
using namespace nanhang_weighted_fetch;
int main(int argc,char**argv){
    if(argc!=3)return 1;
    KDReader reader("",argv[1],argv[2]);
    std::cout<<"READY\n"<<std::flush;
    size_t n;while(std::cin>>n){
        uint32_t ranks[10];if(n>10)return 2;
        for(size_t i=0;i<n;++i)std::cin>>ranks[i];
        auto start=std::chrono::steady_clock::now();
        std::array<Returned54,10> records;FetchTiming timing;
        reader.fetch_batch(ranks,n,records.data(),timing);
        const int64_t elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
        std::cout.write((char*)&elapsed,8);
        for(size_t i=0;i<n;++i){
            std::cout.write((char*)records[i].times.data(),records[i].times.size()*8);
            std::cout.write((char*)records[i].values.data(),records[i].values.size()*4);
        }
        std::cout.flush();
    }
}
