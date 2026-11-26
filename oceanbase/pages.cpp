// Transfer encoded pages into OceanBase and decode only the SQL-selected blocks.
#include "kd_reader.hpp"
#include <iostream>
#include <fstream>
using namespace nanhang_weighted_fetch;
template<class T> void put(std::ostream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value),sizeof(T));
}
unsigned get(std::istream& in) {
    uint32_t value; require(bool(in.read(reinterpret_cast<char*>(&value),4)),"short page header"); return value;
}
void write_page(std::ostream& out,const Page& p) {
    put(out,p.size); put(out,p.plain); put(out,p.codec);
    out.write(reinterpret_cast<const char*>(p.compressed),p.size);
}
Page next_page(Span& bytes) {
    auto size=little<uint32_t>(bytes.take(4));
    auto plain=little<uint32_t>(bytes.take(4));
    auto codec=little<uint32_t>(bytes.take(4));
    return {bytes.take(size),size,plain,codec};
}
int main(int argc,char** argv) {
    if(argc!=4) return 2;
    if (std::string(argv[1])=="export") {
        std::map<int,Block> blocks;
        std::vector<std::unique_ptr<Mapping>> files;
        for(const auto& path:files_with_suffix(argv[2],".tsfile")) {
            files.push_back(std::make_unique<Mapping>(path));
            index_file(*files.back(),blocks);
        }
        std::vector<Byte> scratch;
        for(const auto& [id,b]:blocks) {
            require(b.template_groups.empty(), "this decoder expects the Group-16 store with original time pages");
            std::ofstream out(fs::path(argv[3])/std::to_string(id),std::ios::binary);
            put(out,static_cast<uint32_t>(ids_from(b.ids,scratch).size()));
            put(out,uint32_t(b.plain_time)); write_page(out,b.time);
            for(const auto& page:b.signals) write_page(out,page);
            require(bool(out),"could not export block");
        }
        return 0;
    }
    require(std::string(argv[1])=="read","mode");
    std::array<KDColumn,3> dict;
    for(unsigned i=0;i<3;++i) dict[i].open(fs::path(argv[2])/(std::to_string(i)+".dict"));
    std::cout<<"READY\n"<<std::flush;
    std::vector<Byte> scratch;
    while(std::cin.peek()!=EOF) {
        unsigned size=get(std::cin);
        require(size>0 && size<4*1024*1024,"invalid block size");
        std::vector<Byte> buffer(size);
        require(bool(std::cin.read(reinterpret_cast<char*>(buffer.data()),size)),"incomplete block");
        Span s{buffer.data(),buffer.data()+size};
        Block b;
        b.count=little<uint32_t>(s.take(4)); b.plain_time=little<uint32_t>(s.take(4))!=0;
        b.time=next_page(s);
        if(!b.plain_time && b.time.codec==0) index_time_restarts(b);
        std::array<KDPage,3> values;
        for(unsigned j=0;j<3;++j) values[j].open(next_page(s),dict[j],b.count);
        require(s.left()==0,"block trailer");
        unsigned n=get(std::cin);
        require(n<=10,"excess result count");
        for(unsigned k=0;k<n;++k) {
            unsigned at=get(std::cin), chrono=(at>>7)&127, sim=at&127;
            require(chrono<b.count && sim<b.count,"position outside block");
            Returned54 rec;
            read_times(b,chrono,rec.times.data(),scratch);
            for(unsigned j=0;j<3;++j) values[j].values(dict[j],sim,rec.values.data()+j*kSamples);
            std::cout.write(reinterpret_cast<const char*>(rec.times.data()),18*8);
            std::cout.write(reinterpret_cast<const char*>(rec.values.data()),54*4);
        }
        std::cout.flush();
    }
}
