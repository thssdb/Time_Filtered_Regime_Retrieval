#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Byte = unsigned char;
using Wide = __int128_t;

[[noreturn]] static void fail(const char* message) { throw std::runtime_error(message); }
static void require(bool condition, const char* message) { if (!condition) fail(message); }

template<class T> static T little(const Byte* source) {
    T value;
    std::memcpy(&value, source, sizeof(value));
    return value;
}

static std::vector<Byte> load_bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "open input");
    std::vector<Byte> output(fs::file_size(path));
    require(bool(input.read(reinterpret_cast<char*>(output.data()), output.size())), "read input");
    return output;
}

static Wide signed_value(const Byte* source, unsigned width) {
    switch (width) {
        case 1: return little<int8_t>(source);
        case 2: return little<int16_t>(source);
        case 4: return little<int32_t>(source);
        case 8: return little<int64_t>(source);
        case 16: return little<Wide>(source);
        default: fail("invalid signed width");
    }
}

static uint32_t unsigned_value(const Byte* source, unsigned width) {
    switch (width) {
        case 1: return *source;
        case 2: return little<uint16_t>(source);
        case 4: return little<uint32_t>(source);
        default: fail("invalid key width");
    }
}

static int32_t unzigzag(uint32_t value) {
    return int32_t((value >> 1) ^ uint32_t(-int32_t(value & 1)));
}

static std::string wide_string(Wide value) {
    bool negative = value < 0;
    __uint128_t magnitude = negative ? __uint128_t(-(value + 1)) + 1 : __uint128_t(value);
    std::string output;
    do { output.push_back(char('0' + magnitude % 10)); magnitude /= 10; } while (magnitude);
    if (negative) output.push_back('-');
    std::reverse(output.begin(), output.end());
    return output;
}

struct RiceInput {
    const Byte* pointer;
    const Byte* end;
    unsigned bit = 0;
    uint64_t word() const {
        const Byte* source = pointer + (bit >> 3);
        require(source < end, "short Rice stream");
        uint64_t value = 0;
        std::memcpy(&value, source, std::min<size_t>(8, end - source));
        return value >> (bit & 7);
    }
    uint32_t get(unsigned width) {
        unsigned quotient = 0;
        auto value = word();
        while (value == 0) {
            unsigned step = 64 - (bit & 7);
            quotient += step;
            bit += step;
            value = word();
        }
        unsigned zeros = std::countr_zero(value);
        bit += zeros + 1;
        uint32_t tail = width ? uint32_t(word() & ((uint64_t(1) << width) - 1)) : 0;
        bit += width;
        return ((quotient + zeros) << width) | tail;
    }
};

struct Page {
    unsigned rows;
    const Byte* anchors;
    const Byte* references;
    const Byte* parameters;
    const Byte* offsets;
    const Byte* payload;
    const Byte* end;
};

struct Column {
    std::vector<Byte> storage;
    unsigned exponent, anchor_width, dictionary_width, key_width, coordinates, records;
    double scale;
    std::vector<Wide> dictionary;
    std::vector<Page> pages;

    explicit Column(const fs::path& path) : storage(load_bytes(path)) {
        const Byte* pointer = storage.data();
        const Byte* end = pointer + storage.size();
        require(end - pointer >= 26 && std::memcmp(pointer, "PFR32V1\0", 8) == 0, "pack magic");
        pointer += 8;
        exponent = *pointer++; anchor_width = *pointer++; dictionary_width = *pointer++; key_width = *pointer++;
        coordinates = little<uint16_t>(pointer); pointer += 2;
        records = little<uint32_t>(pointer); pointer += 4;
        unsigned page_count = little<uint32_t>(pointer); pointer += 4;
        unsigned dictionary_size = little<uint32_t>(pointer); pointer += 4;
        require(coordinates == 60 && dictionary_size > 0, "pack dimensions");
        require(size_t(end - pointer) >= size_t(dictionary_size) * dictionary_width, "short dictionary");
        dictionary.reserve(dictionary_size);
        for (unsigned index = 0; index < dictionary_size; ++index) {
            dictionary.push_back(signed_value(pointer, dictionary_width));
            pointer += dictionary_width;
        }
        pages.reserve(page_count);
        for (unsigned page_index = 0; page_index < page_count; ++page_index) {
            require(end - pointer >= 6, "short page header");
            unsigned rows = little<uint16_t>(pointer); pointer += 2;
            unsigned bytes = little<uint32_t>(pointer); pointer += 4;
            require(rows > 0 && rows <= 128 && size_t(end - pointer) >= bytes, "page extent");
            const Byte* page_end = pointer + bytes;
            require(bytes >= 6 && std::memcmp(pointer, "VDR1", 4) == 0 && pointer[4] == 0 && pointer[5] == 32,
                    "page identity");
            pointer += 6;
            unsigned groups = (rows + 31) / 32;
            const Byte* anchors = pointer; pointer += size_t(groups) * coordinates * anchor_width;
            const Byte* references = pointer; pointer += size_t(groups) * coordinates * key_width;
            const Byte* parameters = pointer; pointer += coordinates;
            unsigned direct = (rows / 32) * 2 + std::min(2u, rows % 32);
            const Byte* offsets = pointer; pointer += 2 * (rows - direct);
            require(pointer <= page_end, "page metadata extent");
            pages.push_back(Page{rows, anchors, references, parameters, offsets, pointer, page_end});
            pointer = page_end;
        }
        require(pointer == end, "pack trailer");
        scale = 1.0;
        for (unsigned index = 0; index < exponent; ++index) scale *= 10.0;
    }

    void integers(unsigned page_index, unsigned row, Wide* output) const {
        const Page& page = pages.at(page_index);
        require(row < page.rows, "value address");
        unsigned group = row / 32, remainder = row % 32;
        for (unsigned coordinate = 0; coordinate < coordinates; ++coordinate)
            output[coordinate] = signed_value(page.anchors +
                (size_t(group) * coordinates + coordinate) * anchor_width, anchor_width);
        if (remainder == 0) return;
        const Byte* reference = page.references + size_t(group) * coordinates * key_width;
        if (remainder == 1) {
            for (unsigned coordinate = 0; coordinate < coordinates; ++coordinate) {
                uint32_t first = unsigned_value(reference + coordinate * key_width, key_width);
                require(first < dictionary.size(), "first key");
                output[coordinate] += dictionary[first];
            }
            return;
        }
        unsigned index = group * 30 + remainder - 2;
        uint16_t byte_offset = little<uint16_t>(page.offsets + 2 * index);
        RiceInput bits{page.payload + byte_offset, page.end};
        for (unsigned coordinate = 0; coordinate < coordinates; ++coordinate) {
            uint32_t first = unsigned_value(reference + coordinate * key_width, key_width);
            int64_t key = int64_t(first) + unzigzag(bits.get(page.parameters[coordinate]));
            require(key >= 0 && size_t(key) < dictionary.size(), "residual key");
            output[coordinate] += dictionary[key];
        }
    }

    void values(unsigned page, unsigned row, float* output) const {
        std::array<Wide,60> integers_buffer;
        integers(page, row, integers_buffer.data());
        for (unsigned coordinate = 0; coordinate < coordinates; ++coordinate)
            output[coordinate] = float(double(integers_buffer[coordinate]) / scale);
    }
};

static uint64_t fnv(uint64_t hash, uint32_t value) {
    hash ^= value;
    return hash * 0x100000001b3ULL;
}

static std::vector<std::array<uint32_t,10>> load_winners(const fs::path& path) {
    std::ifstream input(path);
    require(bool(input), "open winners");
    std::vector<std::array<uint32_t,10>> winners;
    std::string line;
    while (std::getline(input, line)) {
        auto tab = line.find('\t');
        require(tab != std::string::npos, "winner line");
        std::stringstream values(line.substr(tab + 1));
        std::array<uint32_t,10> ids{};
        std::string item;
        for (unsigned index = 0; index < ids.size(); ++index) {
            require(bool(std::getline(values, item, ',')), "short winner list");
            ids[index] = std::stoul(item);
        }
        require(!std::getline(values, item, ','), "long winner list");
        winners.push_back(ids);
    }
    require(winners.size() == 24, "query count");
    return winners;
}

int main(int argc, char** argv) {
    try {
        require(argc == 5, "usage: public_first32 DATASET_DIR RAW54 COLUMNS DATASET");
        fs::path root = argv[1];
        unsigned column_count = std::stoul(argv[3]);
        std::vector<Column> columns;
        for (unsigned column = 0; column < column_count; ++column)
            columns.emplace_back(root / ("c" + std::to_string(column) + ".r32"));
        unsigned records = columns.front().records;
        for (const auto& column : columns)
            require(column.records == records && column.pages.size() == columns.front().pages.size(), "column shape");
        auto address_bytes = load_bytes(root / "address.u32");
        require(address_bytes.size() == size_t(records) * 4, "address size");
        const auto* address = reinterpret_cast<const uint32_t*>(address_bytes.data());
        auto raw_bytes = load_bytes(argv[2]);
        unsigned dimensions = column_count * 60;
        require(raw_bytes.size() == size_t(records) * dimensions * 4, "raw size");
        const auto* raw = reinterpret_cast<const uint32_t*>(raw_bytes.data());
        auto golden = load_bytes(root / "golden.i128");
        require(golden.size() == size_t(records) * dimensions * 16, "golden size");
        uint64_t integer_hash = 0xcbf29ce484222325ULL;
        uint64_t float_hash = 0xcbf29ce484222325ULL;
        for (unsigned record = 0; record < records; ++record) {
            unsigned page = address[record] >> 14, row = address[record] & 127;
            require(page < columns.front().pages.size(), "address page");
            for (unsigned column = 0; column < column_count; ++column) {
                std::array<Wide,60> decoded_integers;
                std::array<float,60> decoded_values;
                columns[column].integers(page, row, decoded_integers.data());
                columns[column].values(page, row, decoded_values.data());
                for (unsigned coordinate = 0; coordinate < 60; ++coordinate) {
                    unsigned dimension = column * 60 + coordinate;
                    Wide value = decoded_integers[coordinate];
                    Wide expected = little<Wide>(golden.data() + (size_t(record) * dimensions + dimension) * 16);
                    if (value != expected) {
                        throw std::runtime_error("exact integer mismatch record=" + std::to_string(record)
                            + " column=" + std::to_string(column) + " coordinate=" + std::to_string(coordinate)
                            + " page=" + std::to_string(page) + " row=" + std::to_string(row)
                            + " got=" + wide_string(value) + " expected=" + wide_string(expected));
                    }
                    uint64_t low = uint64_t(value), high = uint64_t((__uint128_t(value)) >> 64);
                    integer_hash = fnv(fnv(integer_hash, uint32_t(low)), uint32_t(low >> 32));
                    integer_hash = fnv(fnv(integer_hash, uint32_t(high)), uint32_t(high >> 32));
                    float decoded = decoded_values[coordinate];
                    uint32_t bits;
                    std::memcpy(&bits, &decoded, 4);
                    require(bits == raw[size_t(record) * dimensions + dimension], "FP32 mismatch");
                    float_hash = fnv(float_hash, bits);
                }
            }
        }
        auto winners = load_winners(root / "winners.tsv");
        std::vector<float> output(10 * dimensions);
        auto decode = [&](const std::array<uint32_t,10>& ids) {
            for (unsigned position = 0; position < ids.size(); ++position) {
                unsigned record = ids[position];
                unsigned page = address[record] >> 14, row = address[record] & 127;
                for (unsigned column = 0; column < column_count; ++column)
                    columns[column].values(page, row,
                        output.data() + size_t(position) * dimensions + column * 60);
            }
        };
        volatile uint32_t sink = 0;
        for (unsigned warmup = 0; warmup < 5; ++warmup)
            for (const auto& query : winners) { decode(query); uint32_t bits; std::memcpy(&bits, output.data(), 4); sink ^= bits; }
        std::vector<long long> nanoseconds;
        nanoseconds.reserve(30 * winners.size());
        for (unsigned round = 0; round < 30; ++round) {
            for (const auto& query : winners) {
                auto begin = std::chrono::steady_clock::now();
                decode(query);
                auto end = std::chrono::steady_clock::now();
                nanoseconds.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
                uint32_t bits; std::memcpy(&bits, output.data(), 4); sink ^= bits;
            }
        }
        uint64_t query_hash = 0xcbf29ce484222325ULL;
        for (const auto& query : winners) {
            decode(query);
            for (float value : output) { uint32_t bits; std::memcpy(&bits, &value, 4); query_hash = fnv(query_hash, bits); }
        }
        std::cout << "VERIFY,dataset=" << argv[4] << ",records=" << records
                  << ",scalars=" << uint64_t(records) * dimensions
                  << ",integer_hash=" << integer_hash << ",float_hash=" << float_hash
                  << ",query_hash=" << query_hash << "\n";
        for (auto value : nanoseconds) std::cout << "SAMPLE," << value << "\n";
        std::cout << "DONE,samples=" << nanoseconds.size() << ",sink=" << sink << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
