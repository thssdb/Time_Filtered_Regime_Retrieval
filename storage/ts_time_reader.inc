// Included after the persisted page and block types.
struct TimePackView {
  const Byte* payload;
  unsigned deltas;
  unsigned width;
  U64 minimum;
  U64 first;
};

TimePackView parse_time_pack(Span& in) {
  const Byte* h = in.take(24);
  unsigned n = be_u32(h), w = be_u32(h + 4);
  require(n <= 128 && w <= 64, "invalid timestamp pack extent");
  const Byte* p = in.take((std::size_t(n) * w + 7) / 8);
  return {p, n, w, be_word(h + 8,8), be_word(h + 16,8)};
}

template <bool FullWord>
inline U64 time_residual(const Byte* p, const Byte* end, unsigned bit, unsigned width) {
  const Byte* src = p + bit / 8;
  const unsigned skip = bit % 8;
  U64 word = 0;
  if constexpr (FullWord) {
    std::memcpy(&word, src, 8);
  } else {
    const auto bytes = std::min<std::size_t>(8, static_cast<std::size_t>(end - src));
    std::memcpy(&word, src, bytes);
  }
  word = __builtin_bswap64(word) << skip;
  if (width > 64 - skip) word |= U64(src[8]) >> (8 - skip);
  return word >> (64 - width);
}

template <bool FullWord>
inline void extract_times(const TimePackView& pack, const Byte* end, unsigned cursor,
                          U64 value, unsigned start, unsigned count, std::int64_t* out) {
  unsigned bit = cursor * pack.width;
  for (; cursor < start; ++cursor, bit += pack.width)
    value += pack.minimum + time_residual<FullWord>(pack.payload,end,bit,pack.width);
  out[0] = std::bit_cast<std::int64_t>(value);
  for (unsigned i = 1; i < count; ++i, bit += pack.width) {
    value += pack.minimum + time_residual<FullWord>(pack.payload,end,bit,pack.width);
    out[i] = std::bit_cast<std::int64_t>(value);
  }
}

#ifdef TS_RESTART
void index_time_restarts(Block& block) {
  std::vector<Byte> unused;
  auto page = block.time.unpack(unused);
  Span scan = page;
  const unsigned rows = block.count * kSamples;
  block.time_packs.reserve((rows + 128)/129);
  unsigned start = 0;
  while (scan.left()) {
    const auto offset = static_cast<std::uint32_t>(scan.p - page.p);
    const auto pack = parse_time_pack(scan);
    require(start + pack.deltas + 1 <= rows,"timestamp pack row count");
    TimeRestart item{offset,static_cast<std::uint16_t>(start),static_cast<std::uint16_t>(pack.deltas+1),{}};
    U64 current = pack.first;
    if (pack.width != 0) {
      for(unsigned i=1; i<=pack.deltas; ++i) {
        current += pack.minimum + time_residual<false>(pack.payload,page.end,(i-1)*pack.width,pack.width);
        if (i % 32 == 0) item.values[i/32 - 1] = current;
      }
    }
    block.time_packs.push_back(item);
    start += pack.deltas + 1;
  }
  require(start == rows,"wrong timestamp row count");
}
#endif

void decode_random_times(const Block& block, Span in, unsigned start, unsigned n, std::int64_t* out) {
  unsigned pack_id = 0;
#ifdef TS_RESTART
  const auto& directory = block.time_packs;
  const Byte* base = in.p;
  // A full TS2DIFF pack has 128 deltas and one first value.
  pack_id = start / 129;
  const auto fits = [&](unsigned i) {
    return i < directory.size() && directory[i].start <= start &&
           start < unsigned(directory[i].start) + directory[i].count;
  };
  if (!fits(pack_id)) {
    auto it = std::upper_bound(directory.begin(), directory.end(),start,
           [](unsigned row, const TimeRestart& item) {return row < item.start;});
    require(it != directory.begin(),"timestamp before first pack");
    pack_id = static_cast<unsigned>(it - directory.begin() - 1);
  }
  in.p = base + directory[pack_id].offset;
  start -= directory[pack_id].start;
#endif
  for (;;) {
    const auto pack = parse_time_pack(in);
    unsigned rows = pack.deltas + 1;
    if (start >= rows) {
      start -= rows;
      continue;
    }
    const unsigned count = std::min(n,rows-start);
    if (pack.width == 0) {
      U64 value = pack.first + U64(start) * pack.minimum;
      for(unsigned i=0; i<count; ++i,value+=pack.minimum)
        out[i] = std::bit_cast<std::int64_t>(value);
    } else {
      unsigned cursor = 0;
      U64 value = pack.first;
#ifdef TS_RESTART
      cursor = start / 32 * 32;
      if (cursor) value = directory[pack_id].values[cursor/32 - 1];
#endif
      if (start+count<=1 || in.end - pack.payload >= static_cast<std::ptrdiff_t>(((start+count-2)*pack.width)/8+8))
        extract_times<true>(pack,in.end,cursor,value,start,count,out);
      else
        extract_times<false>(pack,in.end,cursor,value,start,count,out);
    }
    n -= count;
    if (!n) return;
    out += count;
    start = 0;
    ++pack_id;
  }
}
