// Export the paper's coarse graph, or search an exported graph loaded from OceanBase.
#include <faiss/IndexHNSW.h>
#include <faiss/IndexIVFFlat.h>
#include <faiss/index_io.h>
#include "route.hpp"
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

constexpr int dimensions = 42;
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }

int main(int argc, char** argv) {
    const bool exporting = argc == 7 && std::string(argv[1]) == "export";
    if (!exporting && !(argc == 4 && std::string(argv[1]) == "query")) return 2;
    std::unique_ptr<faiss::Index> source(faiss::read_index(argv[2]));
    if (exporting) {
        const auto& ivf = dynamic_cast<faiss::IndexIVFFlat&>(*source);
        const auto& coarse = dynamic_cast<faiss::IndexHNSW&>(*ivf.quantizer);
        check(ivf.d == dimensions && ivf.nlist >= 16 && ivf.ntotal > 0, "expected a 42D IVF index with at least 16 lists");
        std::vector<int32_t> assignments(ivf.ntotal, -1);
        for (size_t list = 0; list < ivf.nlist; ++list) {
            faiss::InvertedLists::ScopedIds ids(ivf.invlists, list);
            for (size_t i = 0; i < ivf.invlists->list_size(list); ++i) {
                auto rank = ids[i];
                check(rank >= 0 && rank < ivf.ntotal && assignments[rank] == -1, "invalid posting rank");
                assignments[rank] = static_cast<int32_t>(list);
            }
        }
        check(std::find(assignments.begin(), assignments.end(), -1) == assignments.end(), "missing rank");
        std::ofstream mapping(argv[5], std::ios::binary);
        mapping.write(reinterpret_cast<const char*>(assignments.data()), assignments.size()*sizeof(int32_t));
        std::ofstream counts(argv[6]);
        counts << ivf.nlist << '\n';
        check(bool(mapping) && bool(counts), "failed to write posting assignments");
        std::ifstream input(argv[3], std::ios::binary);
        std::array<double, dimensions> weights;
        check(bool(input.read(reinterpret_cast<char*>(weights.data()), sizeof(weights))), "weights");
        std::vector<float> centers(ivf.nlist*dimensions);
        coarse.reconstruct_n(0,ivf.nlist,centers.data());
        for (size_t i=0; i<ivf.nlist; ++i) {
            float* c=centers.data()+i*dimensions;
            double sum=0;
            for (int j=0;j<dimensions;++j) sum+=(weights[j]*double(c[j]))*double(c[j]);
            check(!ivf.invlists->list_size(i) || (sum > 0 && std::isfinite(sum)), "invalid weighted centroid norm");
            const double inv=ivf.invlists->list_size(i)?1.0/std::sqrt(sum):0;
            for (int j=0;j<dimensions;++j) c[j]=float(double(c[j])*inv);
        }
        faiss::IndexHNSWFlat graph(dimensions,16,faiss::METRIC_INNER_PRODUCT);
        graph.storage->add(ivf.nlist,centers.data());
        graph.ntotal=ivf.nlist; graph.hnsw=coarse.hnsw; graph.hnsw.efSearch=256;
        faiss::write_index(&graph,argv[4]);
        return 0;
    }
    check(std::string(argv[1])=="query","unknown mode");
    const auto& graph=dynamic_cast<faiss::IndexHNSW&>(*source);
    std::ifstream input(argv[3]);
    std::vector<int64_t> lo(graph.ntotal), hi(graph.ntotal);
    std::vector<size_t> count(graph.ntotal);
    for (size_t i=0;i<count.size();++i) check(bool(input>>count[i]>>lo[i]>>hi[i]),"list bounds");
    nanhang::ivf_hop_route::Metadata metadata(graph.hnsw,lo,hi,count);
    std::cout<<"READY\n"<<std::flush;
    int64_t start, end;
    while (std::cin>>start>>end) {
        std::array<float,dimensions> q;
        for(auto& x:q) check(bool(std::cin>>x),"short query");
        std::array<float,16> scores;
        std::array<faiss::idx_t,16> lists;
        if (graph.hnsw.max_level>0) {
            nh_hop::Counts counters;
            auto params=metadata.parameters(start,end,&counters);
            graph.search(1,q.data(),16,scores.data(),lists.data(),&params);
            check(params.reached,"one-hop gate unavailable");
        } else graph.search(1,q.data(),16,scores.data(),lists.data());
        for (auto id:lists) if (id>=0) std::cout<<id<<' ';
        std::cout<<'\n'<<std::flush;
    }
}
