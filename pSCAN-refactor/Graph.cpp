#include "Graph.h"

#if defined(__INTEL_COMPILER)
#include <malloc.h>
#else

#include <mm_malloc.h>
#include <x86intrin.h>

#endif // defined(__GNUC__)

#include <immintrin.h>

#include <cassert>
#include <cmath>
#include <cstring>

#include <algorithm>

#include "playground/pretty_print.h"
#include "ThreadPool.h"

using namespace std::chrono;
using namespace yche;

Graph::Graph(const char *dir_string, const char *eps_s, int min_u) {
    io_helper_ptr = yche::make_unique<InputOutput>(dir_string);
    io_helper_ptr->ReadGraph();

    auto tmp_start = high_resolution_clock::now();
    // 1st: parameter
    std::tie(eps_a2, eps_b2) = io_helper_ptr->ParseEps(eps_s);
    this->min_u = min_u;

    // 2nd: graph
    // csr representation
    n = static_cast<ui>(io_helper_ptr->n);
    out_edge_start = std::move(io_helper_ptr->offset_out_edges);
    out_edges = std::move(io_helper_ptr->out_edges);

    // vertex properties
    degree = std::move(io_helper_ptr->degree);
    core_status_lst = vector<char>(n, UN_KNOWN);

    // edge properties
    min_cn = static_cast<int *>(_mm_malloc(io_helper_ptr->m * sizeof(int), 32));
#define PTR_TO_UINT64(x) (uint64_t)(uintptr_t)(x)
    assert(PTR_TO_UINT64(min_cn) % 32 == 0);

    // 3rd: disjoint-set, make-set at the beginning
    disjoint_set_ptr = yche::make_unique<DisjointSets>(n);

    // 4th: cluster_dict
    cluster_dict = static_cast<int *>(_mm_malloc(io_helper_ptr->n * sizeof(int), 32));
    for (auto i = 0; i < io_helper_ptr->n; i++) {
        cluster_dict[i] = n;
    }
    assert(PTR_TO_UINT64(cluster_dict) % 32 == 0);

    auto all_end = high_resolution_clock::now();
    cout << "other construct time:" << duration_cast<milliseconds>(all_end - tmp_start).count()
         << " ms\n";
}

Graph::~Graph() {
    _mm_free(min_cn);
    _mm_free(cluster_dict);
}

void Graph::Output(const char *eps_s, const char *miu) {
    io_helper_ptr->Output(eps_s, miu, noncore_cluster, core_status_lst, cluster_dict, *disjoint_set_ptr);
}

int Graph::ComputeCnLowerBound(int du, int dv) {
    auto c = (int) (sqrtl((((long double) du) * ((long double) dv) * eps_a2) / eps_b2));
    if (((long long) c) * ((long long) c) * eps_b2 < ((long long) du) * ((long long) dv) * eps_a2) { ++c; }
    return c;
}

int Graph::IntersectNeighborSetsSSE(int u, int v, int min_cn_num) {
    int cn = 2; // count for self and v, count for self and u
    int du = out_edge_start[u + 1] - out_edge_start[u] + 2, dv =
            out_edge_start[v + 1] - out_edge_start[v] + 2; // count for self and v, count for self and u

    auto offset_nei_u = out_edge_start[u], offset_nei_v = out_edge_start[v];

    // correctness guaranteed by two pruning previously in computing min_cn
    constexpr int parallelism = 4;
    while (true) {
        // sse4.2, out_edges[offset_nei_v] as the pivot
        __m128i pivot_u = _mm_set1_epi32(out_edges[offset_nei_v]);
        while (offset_nei_u + parallelism < out_edge_start[u + 1]) {
            __m128i inspected_ele = _mm_loadu_si128(reinterpret_cast<const __m128i *>(&out_edges[offset_nei_u]));
            __m128i cmp_res = _mm_cmpgt_epi32(pivot_u, inspected_ele);
            auto mask = _mm_movemask_epi8(cmp_res);
            auto count = mask == 0xffff ? parallelism : _popcnt32(mask) >> 2;
            // update offset_nei_u and du
            offset_nei_u += count;
            du -= count;
            if (du < min_cn_num) {
                return NOT_SIMILAR;
            }
            if (count < parallelism) {
                break;
            }
        }
        if (offset_nei_u + parallelism >= out_edge_start[u + 1]) {
            break;
        }

        // sse4.2, out_edges[offset_nei_u] as the pivot
        __m128i pivot_v = _mm_set1_epi32(out_edges[offset_nei_u]);
        while (offset_nei_v + parallelism < out_edge_start[v + 1]) {
            __m128i inspected_ele = _mm_loadu_si128(reinterpret_cast<const __m128i *>(&out_edges[offset_nei_v]));
            __m128i cmp_res = _mm_cmpgt_epi32(pivot_v, inspected_ele);
            auto mask = _mm_movemask_epi8(cmp_res);
            auto count = mask == 0xffff ? parallelism : _popcnt32(mask) >> 2;

            // update offset_nei_u and du
            offset_nei_v += count;
            dv -= count;
            if (dv < min_cn_num) {
                return NOT_SIMILAR;
            }
            if (count < parallelism) {
                break;
            }
        }
        if (offset_nei_v + parallelism >= out_edge_start[v + 1]) {
            break;
        }

        // find possible equality
        if (out_edges[offset_nei_u] == out_edges[offset_nei_v]) {
            ++cn;
            if (cn >= min_cn_num) {
                return SIMILAR;
            }
            ++offset_nei_u;
            ++offset_nei_v;
        }
    }

    while (true) {
        // left ones
        while (out_edges[offset_nei_u] < out_edges[offset_nei_v]) {
            --du;
            if (du < min_cn_num) { return NOT_SIMILAR; }
            ++offset_nei_u;
        }
        while (out_edges[offset_nei_u] > out_edges[offset_nei_v]) {
            --dv;
            if (dv < min_cn_num) { return NOT_SIMILAR; }
            ++offset_nei_v;
        }
        if (out_edges[offset_nei_u] == out_edges[offset_nei_v]) {
            ++cn;
            if (cn >= min_cn_num) {
                return SIMILAR;
            }
            ++offset_nei_u;
            ++offset_nei_v;
        }
    }
}

#if defined(ENABLE_AVX2)
int Graph::IntersectNeighborSetsAVX2(int u, int v, int min_cn_num) {
    int cn = 2; // count for self and v, count for self and u
    int du = out_edge_start[u + 1] - out_edge_start[u] + 2, dv =
            out_edge_start[v + 1] - out_edge_start[v] + 2; // count for self and v, count for self and u

    auto offset_nei_u = out_edge_start[u], offset_nei_v = out_edge_start[v];

    // correctness guaranteed by two pruning previously in computing min_cn
    constexpr int parallelism = 8;
    while (true) {
        // avx2, out_edges[offset_nei_v] as the pivot
        __m256i pivot_u = _mm256_set1_epi32(out_edges[offset_nei_v]);
        while (offset_nei_u + parallelism < out_edge_start[u + 1]) {
            __m256i inspected_ele = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(&out_edges[offset_nei_u]));
            __m256i cmp_res = _mm256_cmpgt_epi32(pivot_u, inspected_ele);
            auto mask = _mm256_movemask_epi8(cmp_res);
            auto count = mask == 0xffffffff ? parallelism : _popcnt32(mask) >> 2;
            // update offset_nei_u and du
            offset_nei_u += count;
            du -= count;
            if (du < min_cn_num) {
                return NOT_SIMILAR;
            }
            if (count < parallelism) {
                break;
            }
        }
        if (offset_nei_u + parallelism >= out_edge_start[u + 1]) {
            break;
        }

        // avx2, out_edges[offset_nei_u] as the pivot
        __m256i pivot_v = _mm256_set1_epi32(out_edges[offset_nei_u]);
        while (offset_nei_v + parallelism < out_edge_start[v + 1]) {
            __m256i inspected_ele = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(&out_edges[offset_nei_v]));
            __m256i cmp_res = _mm256_cmpgt_epi32(pivot_v, inspected_ele);
            auto mask = _mm256_movemask_epi8(cmp_res);
            auto count = mask == 0xffffffff ? parallelism : _popcnt32(mask) >> 2;

            // update offset_nei_u and du
            offset_nei_v += count;
            dv -= count;
            if (dv < min_cn_num) {
                return NOT_SIMILAR;
            }
            if (count < parallelism) {
                break;
            }
        }
        if (offset_nei_v + parallelism >= out_edge_start[v + 1]) {
            break;
        }

        // find possible equality
        if (out_edges[offset_nei_u] == out_edges[offset_nei_v]) {
            ++cn;
            if (cn >= min_cn_num) {
                return SIMILAR;
            }
            ++offset_nei_u;
            ++offset_nei_v;
        }
    }

    while (true) {
        // left ones
        while (out_edges[offset_nei_u] < out_edges[offset_nei_v]) {
            --du;
            if (du < min_cn_num) { return NOT_SIMILAR; }
            ++offset_nei_u;
        }
        while (out_edges[offset_nei_u] > out_edges[offset_nei_v]) {
            --dv;
            if (dv < min_cn_num) { return NOT_SIMILAR; }
            ++offset_nei_v;
        }
        if (out_edges[offset_nei_u] == out_edges[offset_nei_v]) {
            ++cn;
            if (cn >= min_cn_num) {
                return SIMILAR;
            }
            ++offset_nei_u;
            ++offset_nei_v;
        }
    }
}
#endif

#if defined(ENABLE_AVX2_MERGE)
int Graph::IntersectNeighborSetsAVX2MergePopCnt(int u, int v, int min_cn_num) {
    if (degree[u] > degree[v]) {
        auto tmp = u;
        u = v;
        v = tmp;
    }
    auto off_nei_u = out_edge_start[u], off_nei_v = out_edge_start[v];
    auto off_u_end = out_edge_start[u + 1], off_v_end = out_edge_start[v + 1];

    auto cn_count = 2;
    __m256i two_per_rule = _mm256_set_epi32(1, 1, 1, 1, 0, 0, 0, 0);
    __m256i four_per_rule = _mm256_set_epi32(3, 2, 1, 0, 3, 2, 1, 0);

    auto size1 = (off_v_end - off_nei_v) / (off_u_end - off_nei_u);
    if (size1 > 2) {
        if (off_nei_u < off_u_end && off_nei_v + 7 < off_v_end) {
            __m256i u_elements = _mm256_set1_epi32(out_edges[off_nei_u]);
            __m256i v_elements = _mm256_loadu_si256((__m256i *) (&out_edges[off_nei_v]));

            while (true) {
                __m256i tmp_res = _mm256_cmpeq_epi32(u_elements, v_elements);
                int mask = _mm256_movemask_epi8(tmp_res);
                cn_count += _popcnt32(mask) >> 2;

                if (out_edges[off_nei_u] > out_edges[off_nei_v + 7]) {
                    off_nei_v += 8;
                    if (off_nei_v + 7 >= off_v_end) {
                        break;
                    }
                    v_elements = _mm256_loadu_si256((__m256i *) (&out_edges[off_nei_v]));
                } else {
                    off_nei_u++;
                    if (off_nei_u >= off_u_end) {
                        break;
                    }
                    u_elements = _mm256_set1_epi32(out_edges[off_nei_u]);
                }
            }
        }
    } else {
        if (off_nei_u + 1 < off_u_end && off_nei_v + 3 < off_v_end) {
            __m256i u_elements = _mm256_loadu_si256((__m256i *) (&out_edges[off_nei_u]));
            __m256i u_elements_per = _mm256_permutevar8x32_epi32(u_elements, two_per_rule);
            __m256i v_elements = _mm256_loadu_si256((__m256i *) (&out_edges[off_nei_v]));
            __m256i v_elements_per = _mm256_permutevar8x32_epi32(v_elements, four_per_rule);

            while (true) {
                __m256i tmp_res = _mm256_cmpeq_epi32(u_elements_per, v_elements_per);
                int mask = _mm256_movemask_epi8(tmp_res);
                cn_count += _popcnt32(mask) >> 2;

                if (out_edges[off_nei_u + 1] > out_edges[off_nei_v + 3]) {
                    off_nei_v += 4;
                    if (off_nei_v + 3 >= off_v_end) {
                        break;
                    }
                    v_elements = _mm256_loadu_si256((__m256i *) (&out_edges[off_nei_v]));
                    v_elements_per = _mm256_permutevar8x32_epi32(v_elements, four_per_rule);
                } else if (out_edges[off_nei_u + 1] < out_edges[off_nei_v + 3]) {
                    off_nei_u += 2;
                    if (off_nei_u + 1 >= off_u_end) {
                        break;
                    }
                    u_elements = _mm256_loadu_si256((__m256i *) (&out_edges[off_nei_u]));
                    u_elements_per = _mm256_permutevar8x32_epi32(u_elements, two_per_rule);
                } else {
                    off_nei_u += 2;
                    off_nei_v += 4;
                    if (off_nei_u + 1 >= off_u_end || off_nei_v + 3 >= off_v_end) {
                        break;
                    }
                    u_elements = _mm256_loadu_si256((__m256i *) (&out_edges[off_nei_u]));
                    u_elements_per = _mm256_permutevar8x32_epi32(u_elements, two_per_rule);
                    v_elements = _mm256_loadu_si256((__m256i *) (&out_edges[off_nei_v]));
                    v_elements_per = _mm256_permutevar8x32_epi32(v_elements, four_per_rule);
                }
            }
        }
    }
    if (off_nei_u < off_u_end && off_nei_v < off_v_end) {
        while (true) {
            while (out_edges[off_nei_u] < out_edges[off_nei_v]) {
                ++off_nei_u;
                if (off_nei_u >= off_u_end) {
                    return cn_count >= min_cn_num ? SIMILAR : NOT_SIMILAR;
                }
            }
            while (out_edges[off_nei_u] > out_edges[off_nei_v]) {
                ++off_nei_v;
                if (off_nei_v >= off_v_end) {
                    return cn_count >= min_cn_num ? SIMILAR : NOT_SIMILAR;
                }
            }
            if (out_edges[off_nei_u] == out_edges[off_nei_v]) {
                cn_count++;
                ++off_nei_u;
                ++off_nei_v;
                if (off_nei_u >= off_u_end || off_nei_v >= off_v_end) {
                    return cn_count >= min_cn_num ? SIMILAR : NOT_SIMILAR;
                }
            }
        }
    }
    return cn_count >= min_cn_num ? SIMILAR : NOT_SIMILAR;
}
#endif

#if defined(ENABLE_AVX512)
int Graph::IntersectNeighborSetsAVX512(int u, int v, int min_cn_num) {
    int cn = 2; // count for self and v, count for self and u
    int du = out_edge_start[u + 1] - out_edge_start[u] + 2, dv =
            out_edge_start[v + 1] - out_edge_start[v] + 2; // count for self and v, count for self and u

    auto offset_nei_u = out_edge_start[u], offset_nei_v = out_edge_start[v];

    // correctness guaranteed by two pruning previously in computing min_cn
    constexpr int parallelism = 16;
    while (true) {
        // avx512(knl), out_edges[offset_nei_v] as the pivot
        while (offset_nei_u + parallelism < out_edge_start[u + 1]) {
            __m512i pivot = _mm512_set1_epi32(out_edges[offset_nei_v]);
            __m512i inspected_ele = _mm512_loadu_si512(reinterpret_cast<const __m512i *>(&out_edges[offset_nei_u]));
            __mmask16 cmp_res = _mm512_cmpgt_epi32_mask(pivot, inspected_ele);
            auto count = _mm_popcnt_u32(cmp_res);

            // update offset_nei_u and du
            offset_nei_u += count;
            du -= count;
            if (du < min_cn_num) {
                return NOT_SIMILAR;
            }
            if (count < parallelism) {
                break;
            }
        }
        if (offset_nei_u + parallelism >= out_edge_start[u + 1]) {
            break;
        }

        // avx512(knl), out_edges[offset_nei_u] as the pivot
        while (offset_nei_v + parallelism < out_edge_start[v + 1]) {
            __m512i pivot = _mm512_set1_epi32(out_edges[offset_nei_u]);
            __m512i inspected_ele = _mm512_loadu_si512(reinterpret_cast<const __m512i *>(&out_edges[offset_nei_v]));
            __mmask16 cmp_res = _mm512_cmpgt_epi32_mask(pivot, inspected_ele);
            auto count = _mm_popcnt_u32(cmp_res);

            // update offset_nei_u and du
            offset_nei_v += count;
            dv -= count;
            if (dv < min_cn_num) {
                return NOT_SIMILAR;
            }
            if (count < parallelism) {
                break;
            }
        }
        if (offset_nei_v + parallelism >= out_edge_start[v + 1]) {
            break;
        }

        // find possible equality
        if (out_edges[offset_nei_u] == out_edges[offset_nei_v]) {
            ++cn;
            if (cn >= min_cn_num) {
                return SIMILAR;
            }
            ++offset_nei_u;
            ++offset_nei_v;
        }
    }

    while (true) {
        // left ones
        while (out_edges[offset_nei_u] < out_edges[offset_nei_v]) {
            --du;
            if (du < min_cn_num) { return NOT_SIMILAR; }
            ++offset_nei_u;
        }
        while (out_edges[offset_nei_u] > out_edges[offset_nei_v]) {
            --dv;
            if (dv < min_cn_num) { return NOT_SIMILAR; }
            ++offset_nei_v;
        }
        if (out_edges[offset_nei_u] == out_edges[offset_nei_v]) {
            ++cn;
            if (cn >= min_cn_num) {
                return SIMILAR;
            }
            ++offset_nei_u;
            ++offset_nei_v;
        }
    }
}
#endif

#if defined(ENABLE_AVX512_NO_DU_DV)
int Graph::IntersectNeighborSetsAVX512NoDuDv(int u, int v, int min_cn_num) {
    int cn_count = 2; // count for self and v, count for self and u

    auto off_nei_u = out_edge_start[u], off_nei_v = out_edge_start[v];
    auto off_u_end = out_edge_start[u + 1], off_v_end = out_edge_start[v + 1];
    // correctness guaranteed by two pruning previously in computing min_cn
    constexpr int parallelism = 16;
    while (true) {
        // avx512(knl), out_edges[offset_nei_v] as the pivot
        while (off_nei_u + parallelism < off_u_end) {
            __m512i pivot = _mm512_set1_epi32(out_edges[off_nei_v]);
            __m512i inspected_ele = _mm512_loadu_si512(reinterpret_cast<const __m512i *>(&out_edges[off_nei_u]));
            __mmask16 cmp_res = _mm512_cmpgt_epi32_mask(pivot, inspected_ele);
            auto count = _mm_popcnt_u32(cmp_res);

            // update offset_nei_u and du
            off_nei_u += count;
            if (count < parallelism) {
                break;
            }
        }
        if (off_nei_u + parallelism >= out_edge_start[u + 1]) {
            break;
        }

        // avx512(knl), out_edges[offset_nei_u] as the pivot
        while (off_nei_v + parallelism < off_v_end) {
            __m512i pivot = _mm512_set1_epi32(out_edges[off_nei_u]);
            __m512i inspected_ele = _mm512_loadu_si512(reinterpret_cast<const __m512i *>(&out_edges[off_nei_v]));
            __mmask16 cmp_res = _mm512_cmpgt_epi32_mask(pivot, inspected_ele);
            auto count = _mm_popcnt_u32(cmp_res);

            // update offset_nei_u and du
            off_nei_v += count;
            if (count < parallelism) {
                break;
            }
        }
        if (off_nei_v + parallelism >= out_edge_start[v + 1]) {
            break;
        }

        // find possible equality
        if (out_edges[off_nei_u] == out_edges[off_nei_v]) {
            ++cn_count;
            if (cn_count >= min_cn_num) {
                return SIMILAR;
            }
            ++off_nei_u;
            ++off_nei_v;
        }
    }

    if (off_nei_u < off_u_end && off_nei_v < off_v_end) {
        while (true) {
            while (out_edges[off_nei_u] < out_edges[off_nei_v]) {
                ++off_nei_u;
                if (off_nei_u >= off_u_end) {
                    return cn_count >= min_cn_num ? SIMILAR : NOT_SIMILAR;
                }
            }
            while (out_edges[off_nei_u] > out_edges[off_nei_v]) {
                ++off_nei_v;
                if (off_nei_v >= off_v_end) {
                    return cn_count >= min_cn_num ? SIMILAR : NOT_SIMILAR;
                }
            }
            if (out_edges[off_nei_u] == out_edges[off_nei_v]) {
                cn_count++;
                ++off_nei_u;
                ++off_nei_v;
                if (off_nei_u >= off_u_end || off_nei_v >= off_v_end) {
                    return cn_count >= min_cn_num ? SIMILAR : NOT_SIMILAR;
                }
            }
        }
    }
}
#endif

#if defined(ENABLE_AVX512_MERGE)
int Graph::IntersectNeighborSetsAVX512MergePopCnt(int u, int v, int min_cn_num) {
    if (degree[u] > degree[v]) {
        auto tmp = u;
        u = v;
        v = tmp;
    }
    auto off_nei_u = out_edge_start[u], off_nei_v = out_edge_start[v];
    auto off_u_end = out_edge_start[u + 1], off_v_end = out_edge_start[v + 1];

    auto cn_count = 2;
    constexpr int parallelism = 16;
    __m512i st = _mm512_set_epi32(3, 3, 3, 3, 2, 2, 2, 2, 1, 1, 1, 1, 0, 0, 0, 0);

    auto size1 = (off_v_end - off_nei_v) / (off_u_end - off_nei_u);
    if (size1 > 2) {
        if (off_nei_u < off_u_end && off_nei_v + 15 < off_v_end) {
            __m512i u_elements = _mm512_set1_epi32(out_edges[off_nei_u]);
            __m512i v_elements = _mm512_loadu_si512((__m512i *) (&out_edges[off_nei_v]));

            while (true) {
                __mmask16 mask = _mm512_cmpeq_epi32_mask(u_elements, v_elements);
                cn_count += _mm_popcnt_u32(mask);

                if (out_edges[off_nei_u] > out_edges[off_nei_v + 15]) {
                    off_nei_v += 16;
                    if (off_nei_v + 15 >= off_v_end) {
                        break;
                    }
                    v_elements = _mm512_loadu_si512((__m512i *) (&out_edges[off_nei_v]));
                } else {
                    off_nei_u++;
                    if (off_nei_u >= off_u_end) {
                        break;
                    }
                    u_elements = _mm512_set1_epi32(out_edges[off_nei_u]);
                }
            }
        }
    } else {
        if (off_nei_u + 3 < off_u_end && off_nei_v + 3 < off_v_end) {
            __m512i u_elements = _mm512_loadu_si512((__m512i *) (&out_edges[off_nei_u]));
            __m512i u_elements_per = _mm512_permutevar_epi32(st, u_elements);
            __m512i v_elements = _mm512_loadu_si512((__m512i *) (&out_edges[off_nei_v]));
            __m512i v_elements_per = _mm512_permute4f128_epi32(v_elements, 0b00000000);

            while (true) {
                __mmask16 mask = _mm512_cmpeq_epi32_mask(u_elements_per, v_elements_per);
                cn_count += _mm_popcnt_u32(mask);

                if (out_edges[off_nei_u + 3] > out_edges[off_nei_v + 3]) {
                    off_nei_v += 4;
                    if (off_nei_v + 3 >= off_v_end) {
                        break;
                    }
                    v_elements = _mm512_loadu_si512((__m512i *) (&out_edges[off_nei_v]));
                    v_elements_per = _mm512_permute4f128_epi32(v_elements, 0b00000000);
                } else if (out_edges[off_nei_u + 3] < out_edges[off_nei_v + 3]) {
                    off_nei_u += 4;
                    if (off_nei_u + 3 >= off_u_end) {
                        break;
                    }
                    u_elements = _mm512_loadu_si512((__m512i *) (&out_edges[off_nei_u]));
                    u_elements_per = _mm512_permutevar_epi32(st, u_elements);
                } else {
                    off_nei_u += 4;
                    off_nei_v += 4;
                    if (off_nei_u + 3 >= off_u_end || off_nei_v + 3 >= off_v_end) {
                        break;
                    }
                    u_elements = _mm512_loadu_si512((__m512i *) (&out_edges[off_nei_u]));
                    u_elements_per = _mm512_permutevar_epi32(st, u_elements);
                    v_elements = _mm512_loadu_si512((__m512i *) (&out_edges[off_nei_v]));
                    v_elements_per = _mm512_permute4f128_epi32(v_elements, 0b00000000);
                }
            }
        }
    }
    if (off_nei_u < off_u_end && off_nei_v < off_v_end) {
        while (true) {
            while (out_edges[off_nei_u] < out_edges[off_nei_v]) {
                ++off_nei_u;
                if (off_nei_u >= off_u_end) {
                    return cn_count >= min_cn_num ? SIMILAR : NOT_SIMILAR;
                }
            }
            while (out_edges[off_nei_u] > out_edges[off_nei_v]) {
                ++off_nei_v;
                if (off_nei_v >= off_v_end) {
                    return cn_count >= min_cn_num ? SIMILAR : NOT_SIMILAR;
                }
            }
            if (out_edges[off_nei_u] == out_edges[off_nei_v]) {
                cn_count++;
                ++off_nei_u;
                ++off_nei_v;
                if (off_nei_u >= off_u_end || off_nei_v >= off_v_end) {
                    return cn_count >= min_cn_num ? SIMILAR : NOT_SIMILAR;
                }
            }
        }
    }
    return cn_count >= min_cn_num ? SIMILAR : NOT_SIMILAR;
}
#endif

int Graph::IntersectNeighborSets(int u, int v, int min_cn_num) {
    int cn = 2; // count for self and v, count for self and u
    int du = out_edge_start[u + 1] - out_edge_start[u] + 2, dv =
            out_edge_start[v + 1] - out_edge_start[v] + 2; // count for self and v, count for self and u

    auto offset_nei_u = out_edge_start[u], offset_nei_v = out_edge_start[v];

    // correctness guaranteed by two pruning previously in computing min_cn
    while (true) {
        while (out_edges[offset_nei_u] < out_edges[offset_nei_v]) {
            --du;
            if (du < min_cn_num) { return NOT_SIMILAR; }
            ++offset_nei_u;
        }
        while (out_edges[offset_nei_u] > out_edges[offset_nei_v]) {
            --dv;
            if (dv < min_cn_num) { return NOT_SIMILAR; }
            ++offset_nei_v;
        }
        if (out_edges[offset_nei_u] == out_edges[offset_nei_v]) {
            ++cn;
            if (cn >= min_cn_num) {
                return SIMILAR;
            }
            ++offset_nei_u;
            ++offset_nei_v;
        }
    }
}

int Graph::EvalSimilarity(int u, ui edge_idx) {
    int v = out_edges[edge_idx];
#if defined(ENABLE_AVX512)
    return IntersectNeighborSetsAVX512(u, v, min_cn[edge_idx]);
#elif defined(ENABLE_AVX512_NO_DU_DV)
    return IntersectNeighborSetsAVX512NoDuDv(u, v, min_cn[edge_idx]);
#elif defined(ENABLE_AVX512_MERGE)
    return IntersectNeighborSetsAVX512MergePopCnt(u, v, min_cn[edge_idx]);
#elif defined(ENABLE_AVX2)
    return IntersectNeighborSetsAVX2(u, v, min_cn[edge_idx]);
#elif defined(ENABLE_AVX2_MERGE)
    return IntersectNeighborSetsAVX2MergePopCnt(u, v, min_cn[edge_idx]);
#elif defined(ENABLE_SSE)
    return IntersectNeighborSetsSSE(u, v, min_cn[edge_idx]);
#else
    return IntersectNeighborSets(u, v, min_cn[edge_idx]);
#endif
}

bool Graph::IsDefiniteCoreVertex(int u) {
    return core_status_lst[u] == CORE;
}

ui Graph::BinarySearch(vector<int> &array, ui offset_beg, ui offset_end, int val) {
    auto mid = static_cast<ui>((static_cast<unsigned long>(offset_beg) + offset_end) / 2);
    if (array[mid] == val) { return mid; }
    return val < array[mid] ? BinarySearch(array, offset_beg, mid, val) : BinarySearch(array, mid + 1, offset_end, val);
}

void Graph::PruneDetail(int u) {
    auto sd = 0;
    auto ed = degree[u] - 1;
    for (auto edge_idx = out_edge_start[u]; edge_idx < out_edge_start[u + 1]; edge_idx++) {
        auto v = out_edges[edge_idx];
        int deg_a = degree[u], deg_b = degree[v];
        if (deg_a > deg_b) { swap(deg_a, deg_b); }
        if (((long long) deg_a) * eps_b2 < ((long long) deg_b) * eps_a2) {
            min_cn[edge_idx] = NOT_SIMILAR;
            ed--;
        } else {
            int c = ComputeCnLowerBound(deg_a, deg_b);
            auto is_similar_flag = c <= 2;
            min_cn[edge_idx] = is_similar_flag ? SIMILAR : c;
            if (is_similar_flag) {
                sd++;
            }
        }
    }
    if (sd >= min_u) {
        core_status_lst[u] = CORE;
    } else if (ed < min_u) {
        core_status_lst[u] = NON_CORE;
    }
}

void Graph::CheckCoreFirstBSP(int u) {
    if (core_status_lst[u] == UN_KNOWN) {
        auto sd = 0;
        auto ed = degree[u] - 1;
        for (auto edge_idx = out_edge_start[u]; edge_idx < out_edge_start[u + 1]; edge_idx++) {
            // be careful, the next line can only be commented when memory load/store of min_cn is atomic, no torn read
//        auto v = out_edges[edge_idx];
//        if (u <= v) {
            if (min_cn[edge_idx] == SIMILAR) {
                ++sd;
                if (sd >= min_u) {
                    core_status_lst[u] = CORE;
                    return;
                }
            } else if (min_cn[edge_idx] == NOT_SIMILAR) {
                --ed;
                if (ed < min_u) {
                    core_status_lst[u] = NON_CORE;
                    return;
                }
            }
//        }
        }

        for (auto edge_idx = out_edge_start[u]; edge_idx < out_edge_start[u + 1]; edge_idx++) {
            auto v = out_edges[edge_idx];
            if (u <= v && min_cn[edge_idx] > 0) {
                min_cn[edge_idx] = EvalSimilarity(u, edge_idx);
                min_cn[BinarySearch(out_edges, out_edge_start[v], out_edge_start[v + 1], u)] = min_cn[edge_idx];
                if (min_cn[edge_idx] == SIMILAR) {
                    ++sd;
                    if (sd >= min_u) {
                        core_status_lst[u] = CORE;
                        return;
                    }
                } else {
                    --ed;
                    if (ed < min_u) {
                        core_status_lst[u] = NON_CORE;
                        return;
                    }
                }
            }
        }
    }
}

void Graph::CheckCoreSecondBSP(int u) {
    if (core_status_lst[u] == UN_KNOWN) {
        auto sd = 0;
        auto ed = degree[u] - 1;
        for (auto edge_idx = out_edge_start[u]; edge_idx < out_edge_start[u + 1]; edge_idx++) {
            if (min_cn[edge_idx] == SIMILAR) {
                ++sd;
                if (sd >= min_u) {
                    core_status_lst[u] = CORE;
                    return;
                }
            }
            if (min_cn[edge_idx] == NOT_SIMILAR) {
                --ed;
                if (ed < min_u) {
                    return;
                }
            }
        }

        for (auto edge_idx = out_edge_start[u]; edge_idx < out_edge_start[u + 1]; edge_idx++) {
            auto v = out_edges[edge_idx];
            if (min_cn[edge_idx] > 0) {
                min_cn[edge_idx] = EvalSimilarity(u, edge_idx);
                min_cn[BinarySearch(out_edges, out_edge_start[v], out_edge_start[v + 1], u)] = min_cn[edge_idx];
                if (min_cn[edge_idx] == SIMILAR) {
                    ++sd;
                    if (sd >= min_u) {
                        core_status_lst[u] = CORE;
                        return;
                    }
                } else {
                    --ed;
                    if (ed < min_u) {
                        return;
                    }
                }
            }
        }
    }
}

void Graph::ClusterCoreFirstPhase(int u) {
    for (auto j = out_edge_start[u]; j < out_edge_start[u + 1]; j++) {
        auto v = out_edges[j];
        if (u < v && IsDefiniteCoreVertex(v) && !disjoint_set_ptr->IsSameSet(static_cast<uint32_t>(u),
                                                                             static_cast<uint32_t>(v))) {
            if (min_cn[j] == SIMILAR) {
                disjoint_set_ptr->Union(static_cast<uint32_t>(u), static_cast<uint32_t>(v));
            }
        }
    }
}

void Graph::ClusterCoreSecondPhase(int u) {
    for (auto edge_idx = out_edge_start[u]; edge_idx < out_edge_start[u + 1]; edge_idx++) {
        auto v = out_edges[edge_idx];
        if (u < v && IsDefiniteCoreVertex(v) && !disjoint_set_ptr->IsSameSet(static_cast<uint32_t>(u),
                                                                             static_cast<uint32_t>(v))) {
            if (min_cn[edge_idx] > 0) {
                min_cn[edge_idx] = EvalSimilarity(u, edge_idx);
                if (min_cn[edge_idx] == SIMILAR) {
                    disjoint_set_ptr->Union(static_cast<uint32_t>(u), static_cast<uint32_t>(v));
                }
            }
        }
    }
}

void Graph::ClusterNonCoreDetail(int u, vector<pair<int, int>> &tmp_cluster) {
    for (auto j = out_edge_start[u]; j < out_edge_start[u + 1]; j++) {
        auto v = out_edges[j];
        if (!IsDefiniteCoreVertex(v)) {
            auto root_of_u = disjoint_set_ptr->FindRoot(static_cast<uint32_t>(u));
            if (min_cn[j] > 0) {
                min_cn[j] = EvalSimilarity(u, j);
            }
            if (min_cn[j] == SIMILAR) {
                tmp_cluster.emplace_back(cluster_dict[root_of_u], v);
            }
        }
    }
}

void Graph::pSCANFirstPhasePrune() {
    auto prune_start = high_resolution_clock::now();
    {
        auto thread_num = std::thread::hardware_concurrency();
        ThreadPool pool(thread_num);

        auto v_start = 0;
        long deg_sum = 0;
        for (auto v_i = 0; v_i < n; v_i++) {
            deg_sum += degree[v_i];
            if (deg_sum > 64 * 1024) {
                deg_sum = 0;

                pool.enqueue([this](int i_start, int i_end) {
                    for (auto u = i_start; u < i_end; u++) {
                        PruneDetail(u);
                    }
                }, v_start, v_i + 1);
                v_start = v_i + 1;
            }
        }
        pool.enqueue([this](int i_start, int i_end) {
            for (auto u = i_start; u < i_end; u++) {
                PruneDetail(u);
            }
        }, v_start, n);
    }
    auto prune_end = high_resolution_clock::now();
    cout << "1st: prune execution time:" << duration_cast<milliseconds>(prune_end - prune_start).count() << " ms\n";
}

void Graph::pSCANSecondPhaseCheckCore() {
    // check-core 1st phase
    auto find_core_start = high_resolution_clock::now();
    auto thread_num = std::thread::hardware_concurrency();
    {
        ThreadPool pool(thread_num);

        auto v_start = 0;
        long deg_sum = 0;
        for (auto v_i = 0; v_i < n; v_i++) {
            if (core_status_lst[v_i] == UN_KNOWN) {
                deg_sum += degree[v_i];
                if (deg_sum > 32 * 1024) {
                    deg_sum = 0;
                    pool.enqueue([this](int i_start, int i_end) {
                        for (auto i = i_start; i < i_end; i++) { CheckCoreFirstBSP(i); }
                    }, v_start, v_i + 1);
                    v_start = v_i + 1;
                }
            }
        }

        pool.enqueue([this](int i_start, int i_end) {
            for (auto i = i_start; i < i_end; i++) { CheckCoreFirstBSP(i); }
        }, v_start, n);
    }
    auto first_bsp_end = high_resolution_clock::now();
    cout << "2nd: check core first-phase bsp time:"
         << duration_cast<milliseconds>(first_bsp_end - find_core_start).count() << " ms\n";

    // check-core 2nd phase
    {
        ThreadPool pool(thread_num);

        auto v_start = 0;
        long deg_sum = 0;
        for (auto v_i = 0; v_i < n; v_i++) {
            if (core_status_lst[v_i] == UN_KNOWN) {
                deg_sum += degree[v_i];
                if (deg_sum > 64 * 1024) {
                    deg_sum = 0;
                    pool.enqueue([this](int i_start, int i_end) {
                        for (auto i = i_start; i < i_end; i++) { CheckCoreSecondBSP(i); }
                    }, v_start, v_i + 1);
                    v_start = v_i + 1;
                }
            }
        }

        pool.enqueue([this](int i_start, int i_end) {
            for (auto i = i_start; i < i_end; i++) { CheckCoreSecondBSP(i); }
        }, v_start, n);
    }

    auto second_bsp_end = high_resolution_clock::now();
    cout << "2nd: check core second-phase bsp time:"
         << duration_cast<milliseconds>(second_bsp_end - first_bsp_end).count() << " ms\n";
}

void Graph::pSCANThirdPhaseClusterCore() {
    // trivial: prepare data
    auto tmp_start = high_resolution_clock::now();
    for (auto i = 0; i < n; i++) {
        if (IsDefiniteCoreVertex(i)) { cores.emplace_back(i); }
    }
    cout << "core size:" << cores.size() << "\n";
    auto tmp_end0 = high_resolution_clock::now();
    cout << "3rd: copy time: " << duration_cast<milliseconds>(tmp_end0 - tmp_start).count() << " ms\n";

    // cluster-core 1st phase
    {
        ThreadPool pool(std::thread::hardware_concurrency());

        auto v_start = 0;
        long deg_sum = 0;
        for (auto core_index = 0; core_index < cores.size(); core_index++) {
            deg_sum += degree[cores[core_index]];
            if (deg_sum > 128 * 1024) {
                deg_sum = 0;
                pool.enqueue([this](int i_start, int i_end) {
                    for (auto i = i_start; i < i_end; i++) {
                        auto u = cores[i];
                        ClusterCoreFirstPhase(u);
                    }
                }, v_start, core_index + 1);
                v_start = core_index + 1;
            }
        }

        pool.enqueue([this](int i_start, int i_end) {
            for (auto i = i_start; i < i_end; i++) {
                auto u = cores[i];
                ClusterCoreFirstPhase(u);
            }
        }, v_start, cores.size());
    }

    auto tmp_end = high_resolution_clock::now();
    cout << "3rd: prepare time: " << duration_cast<milliseconds>(tmp_end - tmp_start).count() << " ms\n";

    // cluster-core 2nd phase
    {
        ThreadPool pool(std::thread::hardware_concurrency());

        auto v_start = 0;
        long deg_sum = 0;
        for (auto core_index = 0; core_index < cores.size(); core_index++) {
            deg_sum += degree[cores[core_index]];
            if (deg_sum > 128 * 1024) {
                deg_sum = 0;
                pool.enqueue([this](int i_start, int i_end) {
                    for (auto i = i_start; i < i_end; i++) {
                        auto u = cores[i];
                        ClusterCoreSecondPhase(u);
                    }
                }, v_start, core_index + 1);
                v_start = core_index + 1;
            }
        }

        pool.enqueue([this](int i_start, int i_end) {
            for (auto i = i_start; i < i_end; i++) {
                auto u = cores[i];
                ClusterCoreSecondPhase(u);
            }
        }, v_start, cores.size());
    }
    auto end_core_cluster = high_resolution_clock::now();
    cout << "3rd: core clustering time:" << duration_cast<milliseconds>(end_core_cluster - tmp_start).count()
         << " ms\n";
}

void Graph::MarkClusterMinEleAsId() {
    auto thread_num = std::thread::hardware_concurrency();
    ThreadPool pool(thread_num);
    auto step = max(1u, n / thread_num);
    for (auto outer_i = 0u; outer_i < n; outer_i += step) {
        pool.enqueue([this](ui i_start, ui i_end) {
            for (auto i = i_start; i < i_end; i++) {
                if (IsDefiniteCoreVertex(i)) {
                    int x = disjoint_set_ptr->FindRoot(i);
                    int cluster_min_ele;
                    do {
                        // assume no torn read of cluster_dict[x]
                        cluster_min_ele = cluster_dict[x];
                        if (i >= cluster_dict[x]) {
                            break;
                        }
                    } while (!__sync_bool_compare_and_swap(&cluster_dict[x], cluster_min_ele, i));
                }
            }
        }, outer_i, min(outer_i + step, n));
    }
}

void Graph::pSCANFourthPhaseClusterNonCore() {
    // mark cluster label
    noncore_cluster = std::vector<pair<int, int>>();
    noncore_cluster.reserve(n);

    auto tmp_start = high_resolution_clock::now();
    MarkClusterMinEleAsId();

    auto tmp_next_start = high_resolution_clock::now();
    cout << "4th: marking cluster id cost in cluster-non-core:"
         << duration_cast<milliseconds>(tmp_next_start - tmp_start).count() << " ms\n";

    // cluster non-core 2nd phase
    {
        ThreadPool pool(std::thread::hardware_concurrency());

        auto v_start = 0;
        long deg_sum = 0;
        vector<future<vector<pair<int, int>>>> future_vec;
        for (auto core_index = 0; core_index < cores.size(); core_index++) {
            deg_sum += degree[cores[core_index]];
            if (deg_sum > 32 * 1024) {
                deg_sum = 0;
                future_vec.emplace_back(pool.enqueue([this](int i_start, int i_end) -> vector<pair<int, int>> {
                    auto tmp_cluster = vector<pair<int, int>>();
                    for (auto i = i_start; i < i_end; i++) {
                        auto u = cores[i];
                        ClusterNonCoreDetail(u, tmp_cluster);
                    }
                    return tmp_cluster;
                }, v_start, core_index + 1));
                v_start = core_index + 1;
            }
        }

        future_vec.emplace_back(pool.enqueue([this](int i_start, int i_end) -> vector<pair<int, int>> {
            auto tmp_cluster = vector<pair<int, int>>();
            for (auto i = i_start; i < i_end; i++) {
                auto u = cores[i];
                ClusterNonCoreDetail(u, tmp_cluster);
            }
            return tmp_cluster;
        }, v_start, cores.size()));

        for (auto &future: future_vec) {
            for (auto ele:future.get()) {
                noncore_cluster.emplace_back(ele);
            };
        }
    }

    auto all_end = high_resolution_clock::now();
    cout << "4th: non-core clustering time:" << duration_cast<milliseconds>(all_end - tmp_start).count()
         << " ms\n";
}

void Graph::pSCAN() {
    cout << "new algorithm ppSCAN" << endl;
    pSCANFirstPhasePrune();

    pSCANSecondPhaseCheckCore();

    pSCANThirdPhaseClusterCore();

    pSCANFourthPhaseClusterNonCore();
}