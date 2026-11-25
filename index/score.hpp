using Weights = std::array<double,D>;
double inverse_norm(const float* values, const Weights& weights) {
    double sum=0;
    for (size_t j=0;j<D;++j)
        sum+=(weights[j]*double(values[j]))*double(values[j]);
    require(sum>0 && std::isfinite(sum),"invalid weighted norm");
    return 1.0/std::sqrt(sum);
}
struct CosineCandidate {
    double cosine;
    uint32_t rank, id;
    const float* vector;
    bool operator<(const CosineCandidate& other) const {
        return cosine!=other.cosine ? cosine>other.cosine : id<other.id;
    }
};
double cosine_score(const Weights& weighted_q, double invq, const float* x, double invx) {
    double dot=0;
    for(size_t j=0;j<D;++j) dot+=weighted_q[j]*double(x[j]);
    return (dot*invx)*invq;
}
double weighted_l2(const float* q,const float* x,const Weights& weights) {
    double sum=0;
    for(size_t j=0;j<D;++j) {
        const double delta=double(q[j])-double(x[j]);
        sum+=(weights[j]*delta)*delta;
    }
    return std::sqrt(sum);
}
