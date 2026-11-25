// Export centroid coordinates and L0 adjacency for synthetic figure examples.
#include <faiss/IndexHNSW.h>
#include <faiss/IndexIVFFlat.h>
#include <faiss/index_io.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: inspect_index INDEX CENTROIDS_F32 NEIGHBORS_TSV\n";
        return 1;
    }
    std::unique_ptr<faiss::Index> owner(faiss::read_index(argv[1]));
    auto* ivf = dynamic_cast<faiss::IndexIVFFlat*>(owner.get());
    if (!ivf) return 2;
    auto* graph = dynamic_cast<faiss::IndexHNSW*>(ivf->quantizer);
    if (!graph) return 2;
    std::vector<float> coordinates(ivf->d * ivf->nlist);
    graph->reconstruct_n(0, ivf->nlist, coordinates.data());
    std::ofstream floats(argv[2], std::ios::binary), edges(argv[3]);
    floats.write(reinterpret_cast<const char*>(coordinates.data()), coordinates.size()*sizeof(float));
    for (size_t node=0; node<ivf->nlist; ++node) {
        size_t start, stop;
        graph->hnsw.neighbor_range(node, 0, &start, &stop);
        edges << node;
        for (size_t i=start; i<stop && graph->hnsw.neighbors[i]>=0; ++i)
            edges << '\t' << graph->hnsw.neighbors[i];
        edges << '\n';
    }
    return floats && edges ? 0 : 3;
}
