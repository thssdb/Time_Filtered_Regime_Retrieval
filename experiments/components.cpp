#include "../storage/kd_reader.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
using namespace nanhang_weighted_fetch;
int main(int argc,char** argv){
    if(argc!=4) return 2;
    fs::path snap=argv[2];
    KDReader reader("",argv[1],snap/"stores");
    Mapping values(snap/"prepared"/argv[1]/"raw54.f32"), times(snap/"prepared"/argv[1]/"point_time_ns.i64");
    std::ifstream input(argv[3]);std::string line;std::getline(input,line);
    std::vector<std::array<uint32_t,10>> ids,ranks;
    while(std::getline(input,line)){
        std::replace(line.begin(),line.end(),',',' ');std::istringstream row(line);
        std::string query;row>>query;
        auto& iv=ids.emplace_back(); auto& rv=ranks.emplace_back();
        for(auto& id:iv) row>>id;
        for(auto& rank:rv) row>>rank;
        if(!row)return 3;
    }
    if(ids.size()!=24)return 4;
    std::array<float,540> v; std::array<int64_t,180> t;
    for(int rep=-5;rep<30;++rep)for(unsigned q=0;q<ids.size();++q){
        auto run=[&](bool is_value){
            auto start=Clock::now();
            for(unsigned i=0;i<10;++i){
                if(is_value) reader.vals(ranks[q][i],v.data()+54*i); else reader.ts(ranks[q][i],t.data()+18*i);
            }
            asm volatile(""::"m"(v),"m"(t):"memory");
            const auto duration=ns(start,Clock::now());
            for(unsigned i=0;i<10;++i){
                auto id=size_t(ids[q][i]);
                if(is_value) require(!memcmp(&v[54*i],values.data()+id*216,216),"value mismatch");
                else require(!memcmp(&t[18*i],times.data()+id*144,144),"timestamp mismatch");
            }
            if(rep>=0)std::cout<<(is_value?"values":"timestamps")<<","<<duration<<"\n";
        };
        bool first=(q+rep)%2==0;run(first);run(!first);
    }
}
