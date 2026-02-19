// __builtin_assume_aligned - not use no real gain autovectorization better both
// in real and bench scenarios need more tedting
// Need prefetch testinng

#ifndef STRING_HPP
#define STRING_HPP

#include <cctype>
#include <immintrin.h>
#include <iostream>
#include <stdexcept>
#include <type_traits>

#ifdef _WIN32
#include <memoryapi.h>
#include <windows.h>

// PrefetchVirtualMemory
struct MY_WIN32_MEMORY_RANGE_ENTRY
{
    PVOID VirtualAddress;
    SIZE_T NumberOfBytes;
};

// func sig
typedef BOOL(WINAPI *P_PREFETCH_VM)(HANDLE, ULONG_PTR,
                                    MY_WIN32_MEMORY_RANGE_ENTRY *, ULONG);

#else
#include <sys/mman.h>
#endif

// maybe o3 caused
typedef size_t __attribute__((__may_alias__)) size_t_a;

#if defined(__clang__) || defined(__GNUC__)
// batch p
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define VECTORIZE _Pragma("clang loop vectorize(enable) interleave(enable)")
#define ALWAYS_INLINE __attribute__((always_inline))
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#define VECTORIZE
#define ALWAYS_INLINE
#endif

namespace String_lib
{
namespace CPU_Features
{
inline bool has_512()
{
    static bool val = __builtin_cpu_supports("avx512bw");
    return val;
}
inline bool has_avx2()
{
    static bool val = __builtin_cpu_supports("avx2");
    return val;
}
} // namespace CPU_Features

namespace internal
{
ALWAYS_INLINE static inline int _process_diff_reg(const unsigned char *s1,
                                                  const unsigned char *s2,
                                                  uint64_t mask)
{
    if (mask == 0)
        return 0;
    int pos = __builtin_ctzll(mask);
    return s1[pos] - s2[pos];
}

ALWAYS_INLINE static inline int _process_diff_reg(__m512i v1, __m512i v2,
                                                  uint64_t mask)
{
    int pos = __builtin_ctzll(mask);

    // mask translation
    uint8_t b1, b2;
    _mm512_mask_storeu_epi8(&b1, (1ULL << pos), v1);
    _mm512_mask_storeu_epi8(&b2, (1ULL << pos), v2);

    return (int)b1 - (int)b2;
}

inline int _process_diff_avx2(const uint8_t *s1, const uint8_t *s2,
                              uint32_t mask)
{
    // firsy dif index search
    uint32_t index = __builtin_ctz(mask);
    if (s1[index] < s2[index])
        return -1;
    if (s1[index] > s2[index])
        return 1;
    return 0;
}

ALWAYS_INLINE static inline __m512i _group_or(__m512i a, __m512i b)
{
    return _mm512_or_si512(a, b);
}

ALWAYS_INLINE static inline int
_refine_8_vectors(const unsigned char *s1, const unsigned char *s2,
                  __m512i v1_0, __m512i v2_0, __m512i v1_1, __m512i v2_1,
                  __m512i v1_2, __m512i v2_2, __m512i v1_3, __m512i v2_3,
                  __m512i v1_4, __m512i v2_4, __m512i v1_5, __m512i v2_5,
                  __m512i v1_6, __m512i v2_6, __m512i v1_7, __m512i v2_7)
{
    uint64_t m;
    if ((m = _mm512_cmpneq_epi8_mask(v1_0, v2_0)))
        return internal::_process_diff_reg(s1, s2, m);
    if ((m = _mm512_cmpneq_epi8_mask(v1_1, v2_1)))
        return internal::_process_diff_reg(s1 + 64, s2 + 64, m);
    if ((m = _mm512_cmpneq_epi8_mask(v1_2, v2_2)))
        return internal::_process_diff_reg(s1 + 128, s2 + 128, m);
    if ((m = _mm512_cmpneq_epi8_mask(v1_3, v2_3)))
        return internal::_process_diff_reg(s1 + 192, s2 + 192, m);
    if ((m = _mm512_cmpneq_epi8_mask(v1_4, v2_4)))
        return internal::_process_diff_reg(s1 + 256, s2 + 256, m);
    if ((m = _mm512_cmpneq_epi8_mask(v1_5, v2_5)))
        return internal::_process_diff_reg(s1 + 320, s2 + 320, m);
    if ((m = _mm512_cmpneq_epi8_mask(v1_6, v2_6)))
        return internal::_process_diff_reg(s1 + 384, s2 + 384, m);
    return internal::_process_diff_reg(s1 + 448, s2 + 448,
                                       _mm512_cmpneq_epi8_mask(v1_7, v2_7));
}

ALWAYS_INLINE static inline bool compare_sso_64(const char *s1, const char *s2,
                                                size_t_a n)
{
    if (n == 0)
        return true;

    typedef uint64_t __attribute__((__may_alias__)) uint64_a;
    const uint64_a *a = (const uint64_a *)s1;
    const uint64_a *b = (const uint64_a *)s2;

    // 32 byte XOR
    if (n > 32)
    {
        if ((a[0] ^ b[0]) | (a[1] ^ b[1]) | (a[2] ^ b[2]) | (a[3] ^ b[3]))
            return false;
        a += 4;
        b += 4;
        n -= 32;
    }

    // 8 byte
    while (n > 8)
    {
        if (*a != *b)
            return false;
        a++;
        b++;
        n -= 8;
    }

    // tail
    uint64_t mask = 0xFFFFFFFFFFFFFFFFULL >> (64 - n * 8);
    return ((*a ^ *b) & mask) == 0;
}
} // namespace internal
// TODO: CHECK PERF OF LIGHT TICKETS WITH NEW ARENA
template <size_t Align> class UniversalArena
{
    static_assert((Align & (Align - 1)) == 0, "Align must be power of 2");

  private:
    uint8_t *buffer;
    size_t capacity;
    size_t offset;

  public:
    UniversalArena(const UniversalArena &) = delete;
    explicit UniversalArena(size_t size) : capacity(size), offset(0)
    {
        size = (size + Align - 1) & ~(Align - 1);
#if defined(_WIN32)
        buffer = static_cast<uint8_t *>(_aligned_malloc(size, Align));
#else
        buffer = static_cast<uint8_t *>(std::aligned_alloc(Align, size));
#endif
    }
    ~UniversalArena()
    {
#if defined(_WIN32)
        _aligned_free(buffer);
#else
        free(buffer);
#endif
    }

    [[nodiscard]] ALWAYS_INLINE void *alloc(size_t n)
    {

        constexpr size_t mask = Align - 1;
        size_t aligned_n = (n + mask) & ~mask;

        if (UNLIKELY(offset + aligned_n > capacity))
        {
            printf("FATAL: Arena Out of M! Requested: %zu, Left: %zu",
                   aligned_n, capacity - offset);
            return nullptr;
        }

        void *ptr = buffer + offset;
        offset += aligned_n;
        return ptr;
    }

    void reset()
    {
        offset = 0;
    }
    size_t get_used() const
    {
        return offset;
    }
    void *get_current_ptr()
    {
        return buffer + offset;
    }
    void set_offset(size_t new_offset)
    {
        offset = new_offset;
    }
    size_t current_offset() const
    {
        return offset;
    }
};

class Arena
{
  private:
    uint8_t *buffer;
    size_t capacity;
    size_t offset;

  public:
    // NOTE: cap > 16MB
    explicit Arena(size_t size) : capacity(size), offset(0)
    {

#if defined(_WIN32)
        buffer = static_cast<uint8_t *>(_aligned_malloc(size, 128));
#else
        buffer = static_cast<uint8_t *>(std::aligned_alloc(128, size));
#endif
    }

    ~Arena()
    {
#if defined(_WIN32)
        _aligned_free(buffer);
#else
        free(buffer);
#endif
    }
    Arena(const Arena &) = delete;
    Arena &operator=(const Arena &) = delete;

    size_t get_capacity() const
    {
        return capacity;
    }

    [[nodiscard]] ALWAYS_INLINE void *alloc(size_t n)
    {
        // align
        size_t aligned_n = (n + 127) & ~size_t(127);

        if (UNLIKELY(offset + aligned_n > capacity))
        {
            return nullptr; // error aniway or need chain arena
        }

        void *ptr = buffer + offset;
        offset += aligned_n;
        return ptr;
    }

    void reset()
    {
        offset = 0;
    }

    size_t get_used() const
    {
        return offset;
    }
};

struct PoolChunk
{
    void *free_list;
    uint32_t slot_size;

    PoolChunk() : free_list(nullptr), slot_size(0)
    {
    }

    void init_manual(uint32_t sz, void *block, size_t block_size)
    {
        slot_size = sz;
        uint32_t count = block_size / sz;
        uint8_t *curr = static_cast<uint8_t *>(block);

        free_list = curr;
        for (uint32_t i = 0; i < count - 1; ++i)
        {
            *(void **)curr = curr + sz;
            curr += sz;
        }
        *(void **)curr = nullptr;
    }

    ALWAYS_INLINE void *alloc()
    {
        // TODO: iniy guarantee -d TODO: test more
        void *ptr = free_list;
        if (UNLIKELY(!ptr))
            return nullptr;

        void *next_ptr = *(void **)ptr;

        // L1
        if (LIKELY(next_ptr != nullptr))
        {
            __builtin_prefetch(next_ptr, 0, 3);
        }

        free_list = next_ptr;
        return ptr;
    }

    ALWAYS_INLINE void free(void *ptr)
    {

        *(void **)ptr = free_list;

        free_list = ptr;
    }
};

struct GigaPool
{
    static constexpr uint32_t SLAB_COUNT = 9;
    static constexpr uint32_t FIRST_SLOT_SIZE = 128;
    // static constexpr size_t TOTAL_POOL_SIZE = 128 * 1024 * 1024;
    static constexpr size_t TOTAL_POOL_SIZE =
        64 * 1024 * 1024; // FOR TESTS // TODO: test vakues 512 * 1024 * 1024;
    static constexpr size_t SUPER_BLOCK_SIZE = 256 * 1024; // 64

    PoolChunk slabs[SLAB_COUNT];
    uint8_t *master_block;
    uint8_t *pool_ptr; // next free superblock
    uint8_t *pool_end;

    GigaPool()
    {
#ifdef _WIN32
        master_block = (uint8_t *)VirtualAlloc(
            NULL, TOTAL_POOL_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

        if (master_block)
        {
            HMODULE hKernel = GetModuleHandleA("kernel32.dll");
            // runtime pfunc i am on clang so yes
            auto pPrefetch =
                (P_PREFETCH_VM)GetProcAddress(hKernel, "PrefetchVirtualMemory");

            if (pPrefetch)
            {
                MY_WIN32_MEMORY_RANGE_ENTRY entry;
                entry.VirtualAddress = master_block;
                entry.NumberOfBytes = TOTAL_POOL_SIZE;

                pPrefetch(GetCurrentProcess(), 1, &entry, 0);
            }
            else
            {
                // Fallback for my test pc
                for (size_t i = 0; i < TOTAL_POOL_SIZE; i += 4096)
                {
                    // 1 nyte per page
                    ((volatile uint8_t *)master_block)[i] = 0;
                }
            }
        }
#else
        master_block =
            (uint8_t *)mmap(NULL, TOTAL_POOL_SIZE, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (master_block != MAP_FAILED)
        {

            madvise(master_block, TOTAL_POOL_SIZE,
                    MADV_HUGEPAGES | MADV_WILLNEED);
        }
#endif
        pool_ptr = master_block;
        pool_end = master_block + TOTAL_POOL_SIZE;

        for (int i = 0; i < SLAB_COUNT; ++i)
        {
            slabs[i].slot_size = FIRST_SLOT_SIZE << i;
            slabs[i].free_list = nullptr;

            // 1 lene of slabs available
            refill_slab(i);
        }
    }

    ~GigaPool()
    {
        if (!master_block)
            return;
#if defined(_WIN32)
        VirtualFree(master_block, 0, MEM_RELEASE);
#else
        munmap(master_block, TOTAL_POOL_SIZE);
#endif
    }

    __attribute__((noinline)) void *refill_slab(uint32_t slab_idx)
    {
        if (UNLIKELY(pool_ptr + SUPER_BLOCK_SIZE > pool_end))
            return nullptr;

        uint8_t *block_ptr = pool_ptr;
        pool_ptr += SUPER_BLOCK_SIZE;

        const uint32_t sz = slabs[slab_idx].slot_size;
        uint8_t *curr = block_ptr;

        const uint32_t num_blocks = SUPER_BLOCK_SIZE / sz;
        const uint32_t bulk_count = (num_blocks > 4) ? (num_blocks - 4) : 0;
        uint8_t *last_bulk = block_ptr + bulk_count * sz;

        // Unroll
        while (curr < last_bulk)
        {
            *(void **)curr = (curr + sz);
            *(void **)(curr + sz) = (curr + sz * 2);
            *(void **)(curr + sz * 2) = (curr + sz * 3);
            *(void **)(curr + sz * 3) = (curr + sz * 4);
            curr += sz * 4;
        }

        // tail
        uint8_t *last_possible = block_ptr + (num_blocks - 1) * sz;
        while (curr < last_possible)
        {
            *(void **)curr = (curr + sz);
            curr += sz;
        }

        *(void **)curr = slabs[slab_idx].free_list;
        slabs[slab_idx].free_list = block_ptr;

        return slabs[slab_idx].alloc();
    }
};
static thread_local GigaPool t_gigaPool;

// TODO: consider template for forward ref. -d
template <typename T>
ALWAYS_INLINE inline std::remove_reference_t<T> &&move(T &&input) noexcept
{
    return static_cast<std::remove_reference_t<T> &&>(input);
}
/*
// no gain only pain
template<typename T> ALWAYS_INLINE
inline void swap(T& str1, T& str2) noexcept{
    T temp  =  move(str1);
      str1  =  move(str2);
      str2  =  move(temp);
}*/
/* Legacy
ALWAYS_INLINE inline void c_memcpy(char* dest, const char* src, size_t_a n)
{
    char* d = static_cast<char*>(dest);
    const char* s = static_cast<const char*>(src);
    // TODO: xheck on ARM
    while(n > 0 && (uintptr_t)d & (sizeof(size_t_a) - 1))
    {
        *d++ = *s++; --n;
    }

    size_t_a blocks = n / sizeof(size_t_a);
    size_t_a* d8 = reinterpret_cast<size_t_a*>(d);
    const size_t_a* s8 = reinterpret_cast<const size_t_a*>(s);

    VECTORIZE
    for(size_t_a i = 0; i < blocks; ++i)
    {
        d8[i] = s8[i];
    }

    d = reinterpret_cast<char*>(d8 + blocks);
    s = reinterpret_cast<const char*>(s8 + blocks);
    size_t_a tail = n % sizeof(size_t_a);
    for(size_t_a i = 0; i < tail; ++i) d[i] = s[i];
}*/

// Prefer memcpy due to known len
/*ALWAYS_INLINE inline void c_strcpy(char* dest, const char* src)
{
    char* d = dest;
    const char* s = src;

    while((uintptr_t)d & (sizeof(size_t_a) - 1))
    {
        if(!(*d++ = *s++)) return;
    }

    size_t_a* d8 = reinterpret_cast<size_t_a*>(d);

    if (((uintptr_t)s & (sizeof(size_t_a) - 1)) == 0)
    {
        const size_t_a* s8 = reinterpret_cast<const size_t_a*>(s);
        while (true)
        {
            size_t_a v = *s8;
            if((v - 0x0101010101010101ULL) & ~v & 0x8080808080808080ULL) break;
            *d8++ = *s8++;
        }
        d = reinterpret_cast<char*>(d8);
        s = reinterpret_cast<const char*>(s8);
    }

    while((*d++ = *s++));
}*/

[[clang::always_inline]]
static uint64_t compute_hash(const char *str, size_t len)
{
    // const
    uint64_t h = 0x9e3779b97f4a7c15ULL;

    size_t blocks = len / 8;
    const uint64_t *p = reinterpret_cast<const uint64_t *>(str);

    // CRC32
    for (size_t i = 0; i < blocks; ++i)
    {
        // mov
        uint64_t block;
        __builtin_memcpy(&block, p + i, 8);
        h = _mm_crc32_u64(h, block);
    }

    // tail swar
    if (len & 7)
    {
        uint64_t tail = 0;

        __builtin_memcpy(&tail, str + (len & ~7ULL), len & 7);
        h = _mm_crc32_u64(h, tail);
    }

    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;

    return h;
}

inline __attribute__((target("avx512f,avx512bw,avx512vl,avx512dq,bmi2"))) void
c_memcpy_avx512(char *__restrict dest, const char *__restrict src, size_t_a n)
{
    if (UNLIKELY(n == 0))
        return;

    // Prepass
    if (n <= 64)
    {
        uint64_t mask = _bzhi_u64(-1ULL, n);
        __m512i v = _mm512_maskz_loadu_epi8(mask, src);
        _mm512_mask_storeu_epi8(dest, mask, v);
        return;
    }

    char *d = dest;
    const char *s = src;

    // 64 bytes
    while (n >= 64)
    {
        _mm512_storeu_si512((__m512i *)d,
                            _mm512_loadu_si512((const __m512i *)s));
        d += 64;
        s += 64;
        n -= 64;
    }
    // AVX512 mask
    if (n > 0)
    {
        uint64_t mask = _bzhi_u64(-1ULL, n);
        __m512i v = _mm512_maskz_loadu_epi8(mask, s);
        _mm512_mask_storeu_epi8(d, mask, v);
    }
}

inline __attribute__((target("avx2,bmi2"))) void
c_memcpy_avx2(char *__restrict dest, const char *__restrict src, size_t_a n)
{
    if (UNLIKELY(n == 0))
        return;

    // Prepass
    if (n <= 32)
    {
        if (n >= 16)
        {
            __m128i v1 = _mm_loadu_si128((const __m128i *)src);
            __m128i v2 = _mm_loadu_si128((const __m128i *)(src + n - 16));
            _mm_storeu_si128((__m128i *)dest, v1);
            _mm_storeu_si128((__m128i *)(dest + n - 16), v2);
        }
        else if (n >= 8)
        {
            uint64_t v1 = *(const uint64_t *)src;
            uint64_t v2 = *(const uint64_t *)(src + n - 8);
            *(uint64_t *)dest = v1;
            *(uint64_t *)(dest + n - 8) = v2;
        }
        else if (n >= 4)
        {
            uint32_t v1 = *(const uint32_t *)src;
            uint32_t v2 = *(const uint32_t *)(src + n - 4);
            *(uint32_t *)dest = v1;
            *(uint32_t *)(dest + n - 4) = v2;
        }
        else
        {
            dest[0] = src[0];
            if (n > 1)
            {
                dest[n - 1] = src[n - 1];
                if (n > 2)
                    dest[1] = src[1];
            }
        }
        return;
    }

    // 32 bytes
    char *d = dest;
    const char *s = src;
    size_t_a initial_n = n;

    // manual unroll TODO: check performance
    while (n >= 128)
    {
        __m256i v0 = _mm256_loadu_si256((const __m256i *)(s + 0));
        __m256i v1 = _mm256_loadu_si256((const __m256i *)(s + 32));
        __m256i v2 = _mm256_loadu_si256((const __m256i *)(s + 64));
        __m256i v3 = _mm256_loadu_si256((const __m256i *)(s + 96));
        _mm256_storeu_si256((__m256i *)(d + 0), v0);
        _mm256_storeu_si256((__m256i *)(d + 32), v1);
        _mm256_storeu_si256((__m256i *)(d + 64), v2);
        _mm256_storeu_si256((__m256i *)(d + 96), v3);
        d += 128;
        s += 128;
        n -= 128;
    }

    while (n >= 32)
    {
        _mm256_storeu_si256((__m256i *)d,
                            _mm256_loadu_si256((const __m256i *)s));
        d += 32;
        s += 32;
        n -= 32;
    }

    // tail
    if (n > 0)
    {
        _mm256_storeu_si256(
            (__m256i *)(dest + initial_n - 32),
            _mm256_loadu_si256((const __m256i *)(src + initial_n - 32)));
    }
}

inline __attribute__((target("default"))) void
c_memcpy_fallback(char *__restrict dest, const char *__restrict src, size_t_a n)
{
    if (UNLIKELY(n == 0))
        return;

    typedef uint64_t __attribute__((__may_alias__)) uint64_a;

    // Prepass
    if (n <= 32)
    {
        if (n >= 16)
        {
            *(uint64_a *)(dest) = *(const uint64_a *)(src);
            *(uint64_a *)(dest + 8) = *(const uint64_a *)(src + 8);
            *(uint64_a *)(dest + n - 16) = *(const uint64_a *)(src + n - 16);
            *(uint64_a *)(dest + n - 8) = *(const uint64_a *)(src + n - 8);
        }
        else if (n >= 8)
        {
            *(uint64_a *)(dest) = *(const uint64_a *)(src);
            *(uint64_a *)(dest + n - 8) = *(const uint64_a *)(src + n - 8);
        }
        else if (n >= 4)
        {
            *(uint32_t *)(dest) = *(const uint32_t *)(src);
            *(uint32_t *)(dest + n - 4) = *(const uint32_t *)(src + n - 4);
        }
        else
        {
            dest[0] = src[0];
            if (n > 1)
            {
                dest[n - 1] = src[n - 1];
                if (n > 2)
                    dest[1] = src[1];
            }
        }
        return;
    }

    // 2. 32 byte SWAR TODO: need more testing
    char *d = dest;
    const char *s = src;
    size_t_a initial_n = n;

    while (n >= 32)
    {
        uint64_a v0 = *(const uint64_a *)(s + 0);
        uint64_a v1 = *(const uint64_a *)(s + 8);
        uint64_a v2 = *(const uint64_a *)(s + 16);
        uint64_a v3 = *(const uint64_a *)(s + 24);
        *(uint64_a *)(d + 0) = v0;
        *(uint64_a *)(d + 8) = v1;
        *(uint64_a *)(d + 16) = v2;
        *(uint64_a *)(d + 24) = v3;
        d += 32;
        s += 32;
        n -= 32;
    }

    // tail
    if (n > 0)
    {
        size_t_a off = initial_n - 32;
        *(uint64_a *)(dest + off + 0) = *(const uint64_a *)(src + off + 0);
        *(uint64_a *)(dest + off + 8) = *(const uint64_a *)(src + off + 8);
        *(uint64_a *)(dest + off + 16) = *(const uint64_a *)(src + off + 16);
        *(uint64_a *)(dest + off + 24) = *(const uint64_a *)(src + off + 24);
    }
}
// USE __builtin_memcpy INSTEAD!!!!!!!!!!!!!!!!!!!!!!!!
ALWAYS_INLINE inline void c_memcpy(char *__restrict dest,
                                   const char *__restrict src, size_t_a n)
{
    static const bool fast_512 = CPU_Features::has_512();
    static const bool fast_avx2 = CPU_Features::has_avx2();

    if (fast_512)
        c_memcpy_avx512(dest, src, n);
    else if (fast_avx2)
        c_memcpy_avx2(dest, src, n);

    else
        c_memcpy_fallback(dest, src, n);
}

ALWAYS_INLINE inline size_t_a c_strLen(const char *str)
{
    if (UNLIKELY(!str || *str == '\0'))
        return 0;
    const char *s = str;

    while ((uintptr_t)s & 7)
    {
        if (*s == '\0')
            return s - str;
        ++s;
    }

    // TODO: swAR -d : CHECK
    const size_t_a *swar_ptr = reinterpret_cast<const size_t_a *>(s);
    while (true)
    {
        if (UNLIKELY(((uintptr_t)swar_ptr & 0xFFF) > (4096 - sizeof(size_t_a))))
        {
            const char *curr = reinterpret_cast<const char *>(swar_ptr);
            while ((uintptr_t)curr & 0xFFF)
            {
                if (*curr == '\0')
                    return curr - str;
                curr++;
            }
            swar_ptr = reinterpret_cast<const size_t_a *>(curr);
            if (*curr == '\0')
                return curr - str;
        }
        size_t_a v = *swar_ptr;
        if ((v - 0x0101010101010101ULL) & ~v & 0x8080808080808080ULL)
            break;
        ++swar_ptr;
    }

    s = reinterpret_cast<const char *>(swar_ptr);
    while (*s != '\0')
        s++;
    return s - str;
}

// DO NOT USE (is per byte)
/*ALWAYS_INLINE inline char* c_strcat(char* dest, const char* src)
{
    char* rdest = dest;
    while (*dest) dest++;
    VECTORIZE
    while ((*dest++ = *src++));
    return rdest;
}*/

// TODO: need exttensive testing
ALWAYS_INLINE inline int c_strcmp_erms(const char *s1, const char *s2, size_t n)
{
    int res = 0;
    // repz cmpsb
    // rdi = s1, rsi = s2, rcx = n
    asm volatile("repe cmpsb\n\t"
                 "seta %%al\n\t"
                 "setb %%dl\n\t"
                 "subb %%dl, %%al\n\t"
                 "movsbl %%al, %0"
                 : "=r"(res)
                 : "D"(s1), "S"(s2), "c"(n)
                 : "cc", "memory");
    return res;
}

// TODO: optimize
/*inline __attribute__((target("avx512f,avx512bw,avx512vl,avx512dq,bmi2"))) int
c_strcmp_unaligned_avx512(const char *str1, const char *str2, size_t n)
{
    if (UNLIKELY(n == 0))
        return 0;
    if (UNLIKELY(str1 == str2))
        return 0;

    const unsigned char *s1 = reinterpret_cast<const unsigned char *>(str1);
    const unsigned char *s2 = reinterpret_cast<const unsigned char *>(str2);

    // Orepass
    __m512i v1 = _mm512_loadu_si512(s1);
    __m512i v2 = _mm512_loadu_si512(s2);
    uint64_t m = _mm512_cmpneq_epi8_mask(v1, v2);
    if (UNLIKELY(m))
        return internal::_process_diff_reg(s1, s2, m);

    v1 = _mm512_loadu_si512(s1 + 64);
    v2 = _mm512_loadu_si512(s2 + 64);
    m = _mm512_cmpneq_epi8_mask(v1, v2);
    if (UNLIKELY(m))
        return internal::_process_diff_reg(s1 + 64, s2 + 64, m);

    s1 += 128;
    s2 += 128;
    n -= 128;

    while (n >= 64)
    {
        __m512i v1 = _mm512_loadu_si512(s1);
        __m512i v2 = _mm512_loadu_si512(s2);

        // mask
        uint64_t m = _mm512_cmpneq_epi8_mask(v1, v2);

        if (UNLIKELY(m))
        {
            return internal::_process_diff_reg(s1, s2, m);
        }

        s1 += 64;
        s2 += 64;
        n -= 64;

        if (UNLIKELY(n == 2048))
        {
            _mm_prefetch((const char *)(s1 + 512), _MM_HINT_T0);
            _mm_prefetch((const char *)(s2 + 512), _MM_HINT_T0);
        }
    }

    // taul mask
    if (n > 0)
    {
        uint64_t mask = _bzhi_u64(-1ULL, n);
        __m512i v1 = _mm512_maskz_loadu_epi8(mask, s1);
        __m512i v2 = _mm512_maskz_loadu_epi8(mask, s2);
        uint64_t m = _mm512_mask_cmpneq_epi8_mask(mask, v1, v2);
        if (m)
        {
            uint64_t p = __builtin_ctzll(m);
            return (int)s1[p] - (int)s2[p];
        }
    }

    return 0;
}*/

inline __attribute__((target("avx512f,avx512bw,avx512vl,avx512dq,bmi2"))) int
c_strcmp_unaligned_avx512(const char *str1, const char *str2, size_t n)
{
    if (UNLIKELY(n == 0))
        return 0;

    const unsigned char *s1 = reinterpret_cast<const unsigned char *>(str1);
    const unsigned char *s2 = reinterpret_cast<const unsigned char *>(str2);

    while (n >= 64)
    {
        __m512i v1 = _mm512_loadu_si512(s1);
        __m512i v2 = _mm512_loadu_si512(s2);
        uint64_t m = _mm512_cmpneq_epi8_mask(v1, v2);
        if (UNLIKELY(m))
            return internal::_process_diff_reg(s1, s2, m);

        s1 += 64;
        s2 += 64;
        n -= 64;

        if (UNLIKELY(n == 2048))
        {
            _mm_prefetch((const char *)(s1 + 512), _MM_HINT_T0);
            _mm_prefetch((const char *)(s2 + 512), _MM_HINT_T0);
        }
    }

    if (n > 0)
    {
        uint64_t mask = _bzhi_u64(-1ULL, n);
        __m512i v1 = _mm512_maskz_loadu_epi8(mask, s1);
        __m512i v2 = _mm512_maskz_loadu_epi8(mask, s2);
        uint64_t m = _mm512_mask_cmpneq_epi8_mask(mask, v1, v2);
        if (m)
        {
            uint64_t p = __builtin_ctzll(m);
            return (int)s1[p] - (int)s2[p];
        }
    }
    return 0;
}

inline __attribute__((target("avx2,bmi2"))) int
c_strcmp_unaligned_avx2(const char *str1, const char *str2, size_t n)
{

    if (UNLIKELY(str1 == str2))
        return 0;
    if (UNLIKELY(n == 0))
        return 0;

    const uint8_t *s1 = reinterpret_cast<const uint8_t *>(str1);
    const uint8_t *s2 = reinterpret_cast<const uint8_t *>(str2);

// Prepass
#define CHECK_32_AVX2(offset)                                                  \
    {                                                                          \
        __m256i a = _mm256_loadu_si256((const __m256i *)(s1 + (offset)));      \
        __m256i b = _mm256_loadu_si256((const __m256i *)(s2 + (offset)));      \
        unsigned int m =                                                       \
            ~((unsigned int)_mm256_movemask_epi8(_mm256_cmpeq_epi8(a, b)));    \
        if (UNLIKELY(m))                                                       \
            return (int)s1[(offset) + __builtin_ctz(m)] -                      \
                   (int)s2[(offset) + __builtin_ctz(m)];                       \
    }

    CHECK_32_AVX2(0);
    CHECK_32_AVX2(32);
    CHECK_32_AVX2(64);
    CHECK_32_AVX2(96);

    s1 += 128;
    s2 += 128;
    n -= 128;

    // 256 bytes
    while (n >= 256)
    {
        __m256i a0 = _mm256_loadu_si256((const __m256i *)(s1 + 0));
        __m256i b0 = _mm256_loadu_si256((const __m256i *)(s2 + 0));
        __m256i a1 = _mm256_loadu_si256((const __m256i *)(s1 + 32));
        __m256i b1 = _mm256_loadu_si256((const __m256i *)(s2 + 32));
        __m256i a2 = _mm256_loadu_si256((const __m256i *)(s1 + 64));
        __m256i b2 = _mm256_loadu_si256((const __m256i *)(s2 + 64));
        __m256i a3 = _mm256_loadu_si256((const __m256i *)(s1 + 96));
        __m256i b3 = _mm256_loadu_si256((const __m256i *)(s2 + 96));

        __m256i a4 = _mm256_loadu_si256((const __m256i *)(s1 + 128));
        __m256i b4 = _mm256_loadu_si256((const __m256i *)(s2 + 128));
        __m256i a5 = _mm256_loadu_si256((const __m256i *)(s1 + 160));
        __m256i b5 = _mm256_loadu_si256((const __m256i *)(s2 + 160));
        __m256i a6 = _mm256_loadu_si256((const __m256i *)(s1 + 192));
        __m256i b6 = _mm256_loadu_si256((const __m256i *)(s2 + 192));
        __m256i a7 = _mm256_loadu_si256((const __m256i *)(s1 + 224));
        __m256i b7 = _mm256_loadu_si256((const __m256i *)(s2 + 224));

        __m256i c0 = _mm256_cmpeq_epi8(a0, b0);
        __m256i c1 = _mm256_cmpeq_epi8(a1, b1);
        __m256i c2 = _mm256_cmpeq_epi8(a2, b2);
        __m256i c3 = _mm256_cmpeq_epi8(a3, b3);
        __m256i c4 = _mm256_cmpeq_epi8(a4, b4);
        __m256i c5 = _mm256_cmpeq_epi8(a5, b5);
        __m256i c6 = _mm256_cmpeq_epi8(a6, b6);
        __m256i c7 = _mm256_cmpeq_epi8(a7, b7);

        __m256i res =
            _mm256_and_si256(_mm256_and_si256(_mm256_and_si256(c0, c1),
                                              _mm256_and_si256(c2, c3)),
                             _mm256_and_si256(_mm256_and_si256(c4, c5),
                                              _mm256_and_si256(c6, c7)));

        if (UNLIKELY((unsigned int)_mm256_movemask_epi8(res) != 0xFFFFFFFF))
        {
            if ((unsigned int)_mm256_movemask_epi8(c0) != 0xFFFFFFFF)
                CHECK_32_AVX2(0);
            if ((unsigned int)_mm256_movemask_epi8(c1) != 0xFFFFFFFF)
                CHECK_32_AVX2(32);
            if ((unsigned int)_mm256_movemask_epi8(c2) != 0xFFFFFFFF)
                CHECK_32_AVX2(64);
            if ((unsigned int)_mm256_movemask_epi8(c3) != 0xFFFFFFFF)
                CHECK_32_AVX2(96);
            if ((unsigned int)_mm256_movemask_epi8(c4) != 0xFFFFFFFF)
                CHECK_32_AVX2(128);
            if ((unsigned int)_mm256_movemask_epi8(c5) != 0xFFFFFFFF)
                CHECK_32_AVX2(160);
            if ((unsigned int)_mm256_movemask_epi8(c6) != 0xFFFFFFFF)
                CHECK_32_AVX2(192);
            CHECK_32_AVX2(224);
        }
        s1 += 256;
        s2 += 256;
        n -= 256;
    }

    // 32 byte tail
    while (n >= 32)
    {
        CHECK_32_AVX2(0);
        s1 += 32;
        s2 += 32;
        n -= 32;
    }

    if (n > 0)
    {
        __m256i v1 = _mm256_loadu_si256((const __m256i *)s1);
        __m256i v2 = _mm256_loadu_si256((const __m256i *)s2);
        unsigned int mask =
            ~((unsigned int)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v1, v2)));
        mask = _bzhi_u32(mask, n);
        if (mask)
        {
            return (int)s1[__builtin_ctz(mask)] - (int)s2[__builtin_ctz(mask)];
        }
    }

#undef CHECK_32_AVX2
    return 0;
}

inline __attribute__((target("default"))) int
c_strcmp_unaligned_fallback(const char *__restrict str1,
                            const char *__restrict str2, size_t n)
{
    if (UNLIKELY(str1 == str2))
        return true;
    if (UNLIKELY(n == 0))
        return true;

    const unsigned char *s1 = reinterpret_cast<const unsigned char *>(str1);
    const unsigned char *s2 = reinterpret_cast<const unsigned char *>(str2);

    // Fallback
    typedef uint64_t __attribute__((__may_alias__)) uint64_a;
    uint64_t v1, v2;

    // 32 byte
    while (n >= 32)
    {
        v1 = *(const uint64_a *)(s1);
        v2 = *(const uint64_a *)(s2);
        if (UNLIKELY(v1 != v2))
            goto found_diff;
        v1 = *(const uint64_a *)(s1 + 8);
        v2 = *(const uint64_a *)(s2 + 8);
        if (UNLIKELY(v1 != v2))
        {
            s1 += 8;
            s2 += 8;
            goto found_diff;
        }
        v1 = *(const uint64_a *)(s1 + 16);
        v2 = *(const uint64_a *)(s2 + 16);
        if (UNLIKELY(v1 != v2))
        {
            s1 += 16;
            s2 += 16;
            goto found_diff;
        }
        v1 = *(const uint64_a *)(s1 + 24);
        v2 = *(const uint64_a *)(s2 + 24);
        if (UNLIKELY(v1 != v2))
        {
            s1 += 24;
            s2 += 24;
            goto found_diff;
        }

        s1 += 32;
        s2 += 32;
        n -= 32;
    }

    // 8 byte
    while (n >= 8)
    {
        v1 = *(const uint64_a *)s1;
        v2 = *(const uint64_a *)s2;
        if (UNLIKELY(v1 != v2))
            goto found_diff;
        s1 += 8;
        s2 += 8;
        n -= 8;
    }

    // yail safe mask
    if (n > 0)
    {
        v1 = *(const uint64_a *)s1;
        v2 = *(const uint64_a *)s2;

        // BIG E!
        v1 = __builtin_bswap64(v1);
        v2 = __builtin_bswap64(v2);

        // n upper bytes
        uint64_t mask = ~0ULL << (64 - 8 * n);
        v1 &= mask;
        v2 &= mask;

        if (v1 != v2)
            return (v1 < v2) ? -1 : 1;
    }

    return 0;

found_diff:
    v1 = __builtin_bswap64(v1);
    v2 = __builtin_bswap64(v2);
    return (v1 < v2) ? -1 : 1;
}

inline __attribute__((target("avx512f,avx512bw,avx512vl,avx512dq,bmi2"))) int
c_strcmp_avx512(const char *__restrict str1, const char *__restrict str2,
                size_t_a n)
{
    if (UNLIKELY(str1 == str2))
        return 0;
    if (UNLIKELY(n == 0))
        return 0;

    const unsigned char *s1 = reinterpret_cast<const unsigned char *>(str1);
    const unsigned char *s2 = reinterpret_cast<const unsigned char *>(str2);

    // Prepass
    __m512i v1 = _mm512_load_si512(s1);
    __m512i v2 = _mm512_load_si512(s2);
    uint64_t m = _mm512_cmpneq_epi8_mask(v1, v2);
    if (UNLIKELY(m))
        return internal::_process_diff_reg(s1, s2, m);

    v1 = _mm512_load_si512(s1 + 64);
    v2 = _mm512_load_si512(s2 + 64);
    m = _mm512_cmpneq_epi8_mask(v1, v2);
    if (UNLIKELY(m))
        return internal::_process_diff_reg(s1 + 64, s2 + 64, m);

    s1 += 128;
    s2 += 128;
    n -= 128;

    while (n >= 64)
    {
        __m512i v1 = _mm512_load_si512(s1);
        __m512i v2 = _mm512_load_si512(s2);

        // mask
        uint64_t m = _mm512_cmpneq_epi8_mask(v1, v2);

        if (UNLIKELY(m))
        {
            return internal::_process_diff_reg(s1, s2, m);
        }

        s1 += 64;
        s2 += 64;
        n -= 64;

        if (UNLIKELY(n == 2048))
        {
            _mm_prefetch((const char *)(s1 + 512), _MM_HINT_T0);
            _mm_prefetch((const char *)(s2 + 512), _MM_HINT_T0);
        }
    }

    // tail mask
    if (n)
    {
        uint64_t mask = _bzhi_u64(-1ULL, n);
        __m512i v1 = _mm512_maskz_loadu_epi8(mask, s1);
        __m512i v2 = _mm512_maskz_loadu_epi8(mask, s2);
        uint64_t m = _mm512_mask_cmpneq_epi8_mask(mask, v1, v2);
        if (m)
        {
            uint64_t p = __builtin_ctzll(m);
            return (int)s1[p] - (int)s2[p];
        }
    }

    return 0;
}

inline __attribute__((target("avx512f,avx512bw,avx512vl,avx512dq,bmi2"))) bool
c_quick_strcmp_avx512(const char *__restrict str1, const char *__restrict str2,
                      size_t_a n)
{
    if (UNLIKELY(str1 == str2))
        return true;
    if (UNLIKELY(n == 0))
        return true;

    const unsigned char *s1 = reinterpret_cast<const unsigned char *>(str1);
    const unsigned char *s2 = reinterpret_cast<const unsigned char *>(str2);

    __m512i a0 = _mm512_load_si512(s1);
    __m512i b0 = _mm512_load_si512(s2);
    __m512i a1 = _mm512_load_si512(s1 + 64);
    __m512i b1 = _mm512_load_si512(s2 + 64);

    uint64_t d =
        _mm512_cmpneq_epi8_mask(a0, b0) | _mm512_cmpneq_epi8_mask(a1, b1);

    if (n <= 128 || d)
        return d == 0;

    s1 += 128;
    s2 += 128;
    n -= 128;

    // 256 byte
    while (n >= 256)
    {
        _mm_prefetch((const char *)(s1 + 256), _MM_HINT_T0); // _MM_HINT_NTA
        _mm_prefetch((const char *)(s2 + 256), _MM_HINT_T0);

        __m512i v1_0 = _mm512_load_si512(s1);
        __m512i v2_0 = _mm512_load_si512(s2);
        __m512i v1_1 = _mm512_load_si512(s1 + 64);
        __m512i v2_1 = _mm512_load_si512(s2 + 64);
        __m512i v1_2 = _mm512_load_si512(s1 + 128);
        __m512i v2_2 = _mm512_load_si512(s2 + 128);
        __m512i v1_3 = _mm512_load_si512(s1 + 192);
        __m512i v2_3 = _mm512_load_si512(s2 + 192);

        uint64_t diff = _mm512_cmpneq_epi8_mask(v1_0, v2_0) |
                        _mm512_cmpneq_epi8_mask(v1_1, v2_1) |
                        _mm512_cmpneq_epi8_mask(v1_2, v2_2) |
                        _mm512_cmpneq_epi8_mask(v1_3, v2_3);

        if (UNLIKELY(diff))
            return false;

        s1 += 256;
        s2 += 256;
        n -= 256;
    }

    // tail mas;
    __mmask64 k = _cvtu64_mask64(-(uint64_t)(n != 0));

    __m512i fa0 = _mm512_maskz_loadu_epi8(k, s1);
    __m512i fb0 = _mm512_maskz_loadu_epi8(k, s2);
    __m512i fa1 = _mm512_maskz_loadu_epi8(k, s1 + 64);
    __m512i fb1 = _mm512_maskz_loadu_epi8(k, s2 + 64);

    __mmask64 diff =
        _mm512_cmpneq_epi8_mask(fa0, fb0) | _mm512_cmpneq_epi8_mask(fa1, fb1);

    if (diff)
        return false;

    return true;
}

// TODO: optimize especually branch;ess tail
inline __attribute__((target("avx2,bmi2"))) int
c_strcmp_avx2(const char *__restrict str1, const char *__restrict str2,
              size_t_a n)
{
    if (UNLIKELY(str1 == str2))
        return 0;
    if (UNLIKELY(n == 0))
        return 0;

    const unsigned char *s1 = reinterpret_cast<const unsigned char *>(str1);
    const unsigned char *s2 = reinterpret_cast<const unsigned char *>(str2);

#define CHECK_32_AVX2(offset)                                                  \
    {                                                                          \
        __m256i a = _mm256_load_si256((const __m256i *)(s1 + (offset)));       \
        __m256i b = _mm256_load_si256((const __m256i *)(s2 + (offset)));       \
        unsigned int m =                                                       \
            ~((unsigned int)_mm256_movemask_epi8(_mm256_cmpeq_epi8(a, b)));    \
        if (UNLIKELY(m))                                                       \
            return (int)s1[(offset) + __builtin_ctz(m)] -                      \
                   (int)s2[(offset) + __builtin_ctz(m)];                       \
    }

    // Prepass
    CHECK_32_AVX2(0);
    CHECK_32_AVX2(32);
    CHECK_32_AVX2(64);
    CHECK_32_AVX2(96);

    s1 += 128;
    s2 += 128;
    n -= 128;

    // 256 bytes
    while (n >= 256)
    {
        __m256i a0 = _mm256_load_si256((const __m256i *)(s1 + 0));
        __m256i b0 = _mm256_load_si256((const __m256i *)(s2 + 0));
        __m256i a1 = _mm256_load_si256((const __m256i *)(s1 + 32));
        __m256i b1 = _mm256_load_si256((const __m256i *)(s2 + 32));
        __m256i a2 = _mm256_load_si256((const __m256i *)(s1 + 64));
        __m256i b2 = _mm256_load_si256((const __m256i *)(s2 + 64));
        __m256i a3 = _mm256_load_si256((const __m256i *)(s1 + 96));
        __m256i b3 = _mm256_load_si256((const __m256i *)(s2 + 96));

        __m256i a4 = _mm256_load_si256((const __m256i *)(s1 + 128));
        __m256i b4 = _mm256_load_si256((const __m256i *)(s2 + 128));
        __m256i a5 = _mm256_load_si256((const __m256i *)(s1 + 160));
        __m256i b5 = _mm256_load_si256((const __m256i *)(s2 + 160));
        __m256i a6 = _mm256_load_si256((const __m256i *)(s1 + 192));
        __m256i b6 = _mm256_load_si256((const __m256i *)(s2 + 192));
        __m256i a7 = _mm256_load_si256((const __m256i *)(s1 + 224));
        __m256i b7 = _mm256_load_si256((const __m256i *)(s2 + 224));

        __m256i c0 = _mm256_cmpeq_epi8(a0, b0);
        __m256i c1 = _mm256_cmpeq_epi8(a1, b1);
        __m256i c2 = _mm256_cmpeq_epi8(a2, b2);
        __m256i c3 = _mm256_cmpeq_epi8(a3, b3);
        __m256i c4 = _mm256_cmpeq_epi8(a4, b4);
        __m256i c5 = _mm256_cmpeq_epi8(a5, b5);
        __m256i c6 = _mm256_cmpeq_epi8(a6, b6);
        __m256i c7 = _mm256_cmpeq_epi8(a7, b7);

        __m256i res =
            _mm256_and_si256(_mm256_and_si256(_mm256_and_si256(c0, c1),
                                              _mm256_and_si256(c2, c3)),
                             _mm256_and_si256(_mm256_and_si256(c4, c5),
                                              _mm256_and_si256(c6, c7)));

        if (UNLIKELY((unsigned int)_mm256_movemask_epi8(res) != 0xFFFFFFFF))
        {
            if ((unsigned int)_mm256_movemask_epi8(c0) != 0xFFFFFFFF)
                CHECK_32_AVX2(0);
            if ((unsigned int)_mm256_movemask_epi8(c1) != 0xFFFFFFFF)
                CHECK_32_AVX2(32);
            if ((unsigned int)_mm256_movemask_epi8(c2) != 0xFFFFFFFF)
                CHECK_32_AVX2(64);
            if ((unsigned int)_mm256_movemask_epi8(c3) != 0xFFFFFFFF)
                CHECK_32_AVX2(96);
            if ((unsigned int)_mm256_movemask_epi8(c4) != 0xFFFFFFFF)
                CHECK_32_AVX2(128);
            if ((unsigned int)_mm256_movemask_epi8(c5) != 0xFFFFFFFF)
                CHECK_32_AVX2(160);
            if ((unsigned int)_mm256_movemask_epi8(c6) != 0xFFFFFFFF)
                CHECK_32_AVX2(192);
            CHECK_32_AVX2(224);
        }
        s1 += 256;
        s2 += 256;
        n -= 256;
    }

    // 32 byte tail
    while (n >= 32)
    {
        CHECK_32_AVX2(0);
        s1 += 32;
        s2 += 32;
        n -= 32;
    }

    if (n > 0)
    {
        __m256i v1 = _mm256_load_si256((const __m256i *)s1);
        __m256i v2 = _mm256_load_si256((const __m256i *)s2);
        unsigned int mask =
            ~((unsigned int)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v1, v2)));
        mask = _bzhi_u32(mask, n);
        if (mask)
        {
            return (int)s1[__builtin_ctz(mask)] - (int)s2[__builtin_ctz(mask)];
        }
    }

#undef CHECK_32_AVX2
    return 0;
}

inline __attribute__((target("avx2,bmi2"))) bool
c_quick_strcmp_avx2(const char *__restrict str1, const char *__restrict str2,
                    size_t_a n)
{
    if (UNLIKELY(str1 == str2))
        return true;
    if (UNLIKELY(n == 0))
        return true;

    const unsigned char *s1 = reinterpret_cast<const unsigned char *>(str1);
    const unsigned char *s2 = reinterpret_cast<const unsigned char *>(str2);

#define CHECK_EQUAL_32(p1, p2)                                                 \
    (_mm256_testc_si256(                                                       \
        _mm256_cmpeq_epi8(_mm256_load_si256((const __m256i *)(p1)),            \
                          _mm256_load_si256((const __m256i *)(p2))),           \
        _mm256_set1_epi8(-1)))

    if (!CHECK_EQUAL_32(s1, s2))
        return false;
    if (!CHECK_EQUAL_32(s1 + 32, s2 + 32))
        return false;
    if (!CHECK_EQUAL_32(s1 + 64, s2 + 64))
        return false;
    if (!CHECK_EQUAL_32(s1 + 96, s2 + 96))
        return false;

    s1 += 128;
    s2 += 128;
    n -= 128;
    while (n >= 256)
    {
        __m256i a0 = _mm256_load_si256((const __m256i *)(s1 + 0));
        __m256i b0 = _mm256_load_si256((const __m256i *)(s2 + 0));
        __m256i a1 = _mm256_load_si256((const __m256i *)(s1 + 32));
        __m256i b1 = _mm256_load_si256((const __m256i *)(s2 + 32));
        __m256i a2 = _mm256_load_si256((const __m256i *)(s1 + 64));
        __m256i b2 = _mm256_load_si256((const __m256i *)(s2 + 64));
        __m256i a3 = _mm256_load_si256((const __m256i *)(s1 + 96));
        __m256i b3 = _mm256_load_si256((const __m256i *)(s2 + 96));

        __m256i a4 = _mm256_load_si256((const __m256i *)(s1 + 128));
        __m256i b4 = _mm256_load_si256((const __m256i *)(s2 + 128));
        __m256i a5 = _mm256_load_si256((const __m256i *)(s1 + 160));
        __m256i b5 = _mm256_load_si256((const __m256i *)(s2 + 160));
        __m256i a6 = _mm256_load_si256((const __m256i *)(s1 + 192));
        __m256i b6 = _mm256_load_si256((const __m256i *)(s2 + 192));
        __m256i a7 = _mm256_load_si256((const __m256i *)(s1 + 224));
        __m256i b7 = _mm256_load_si256((const __m256i *)(s2 + 224));

        __m256i c0 = _mm256_cmpeq_epi8(a0, b0);
        __m256i c1 = _mm256_cmpeq_epi8(a1, b1);
        __m256i c2 = _mm256_cmpeq_epi8(a2, b2);
        __m256i c3 = _mm256_cmpeq_epi8(a3, b3);
        __m256i c4 = _mm256_cmpeq_epi8(a4, b4);
        __m256i c5 = _mm256_cmpeq_epi8(a5, b5);
        __m256i c6 = _mm256_cmpeq_epi8(a6, b6);
        __m256i c7 = _mm256_cmpeq_epi8(a7, b7);

        __m256i res =
            _mm256_and_si256(_mm256_and_si256(_mm256_and_si256(c0, c1),
                                              _mm256_and_si256(c2, c3)),
                             _mm256_and_si256(_mm256_and_si256(c4, c5),
                                              _mm256_and_si256(c6, c7)));

        if (UNLIKELY(!_mm256_testc_si256(res, _mm256_set1_epi8(-1))))
            return false;

        s1 += 256;
        s2 += 256;
        n -= 256;
    }

    // 32 bytes
    while (n >= 32)
    {
        if (!CHECK_EQUAL_32(s1, s2))
            return false;
        s1 += 32;
        s2 += 32;
        n -= 32;
    }

    // tail
    if (n > 0)
    {
        __m256i v1 = _mm256_load_si256((const __m256i *)s1);
        __m256i v2 = _mm256_load_si256((const __m256i *)s2);
        unsigned int mask =
            ~((unsigned int)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v1, v2)));
        if (_bzhi_u32(mask, n))
            return false;
    }

    return true;
}

// SWAR TODO: ARM is big E need fix
inline __attribute__((target("default"))) bool
c_quick_strcmp_fallback(const char *__restrict str1,
                        const char *__restrict str2, size_t_a n)
{
    if (UNLIKELY(str1 == str2))
        return true;
    if (UNLIKELY(n == 0))
        return true;

    const unsigned char *s1 = reinterpret_cast<const unsigned char *>(str1);
    const unsigned char *s2 = reinterpret_cast<const unsigned char *>(str2);

    // Fallback
    typedef uint64_t __attribute__((__may_alias__)) uint64_a;

    while (n >= 64)
    {
        uint64_t d = 0;
        d |= (*(const uint64_a *)(s1 + 0) ^ *(const uint64_a *)(s2 + 0));
        d |= (*(const uint64_a *)(s1 + 8) ^ *(const uint64_a *)(s2 + 8));
        d |= (*(const uint64_a *)(s1 + 16) ^ *(const uint64_a *)(s2 + 16));
        d |= (*(const uint64_a *)(s1 + 24) ^ *(const uint64_a *)(s2 + 24));
        d |= (*(const uint64_a *)(s1 + 32) ^ *(const uint64_a *)(s2 + 32));
        d |= (*(const uint64_a *)(s1 + 40) ^ *(const uint64_a *)(s2 + 40));
        d |= (*(const uint64_a *)(s1 + 48) ^ *(const uint64_a *)(s2 + 48));
        d |= (*(const uint64_a *)(s1 + 56) ^ *(const uint64_a *)(s2 + 56));

        if (UNLIKELY(d))
            return false;

        s1 += 64;
        s2 += 64;
        n -= 64;
    }

    while (n >= 8)
    {
        if (UNLIKELY(*(const uint64_a *)s1 != *(const uint64_a *)s2))
            return false;
        s1 += 8;
        s2 += 8;
        n -= 8;
    }

    // tail
    if (n > 0)
    {
        return *(const uint64_a *)(s1 + n - 8) ==
               *(const uint64_a *)(s2 + n - 8);
    }

    return true;
}

inline __attribute__((target("default"))) int
c_strcmp_fallback(const char *__restrict str1, const char *__restrict str2,
                  size_t_a n)
{
    if (UNLIKELY(str1 == str2))
        return 0;
    if (UNLIKELY(n == 0))
        return 0;

    const unsigned char *s1 = reinterpret_cast<const unsigned char *>(str1);
    const unsigned char *s2 = reinterpret_cast<const unsigned char *>(str2);

    // Fallback
    typedef uint64_t __attribute__((__may_alias__)) uint64_a;

    uint64_t v1, v2;

    while (n >= 32)
    {
#define CHECK_STEP(offset)                                                     \
    v1 = *(const uint64_a *)(s1 + (offset));                                   \
    v2 = *(const uint64_a *)(s2 + (offset));                                   \
    if (UNLIKELY(v1 != v2))                                                    \
        goto found_diff;

        CHECK_STEP(0);
        CHECK_STEP(8);
        CHECK_STEP(16);
        CHECK_STEP(24);
#undef CHECK_STEP

        s1 += 32;
        s2 += 32;
        n -= 32;
    }

    while (n >= 8)
    {
        v1 = *(const uint64_a *)s1;
        v2 = *(const uint64_a *)s2;
        if (UNLIKELY(v1 != v2))
            goto found_diff;
        s1 += 8;
        s2 += 8;
        n -= 8;
    }

    // tail
    if (n > 0)
    {
        v1 = *(const uint64_a *)(s1 + n - 8);
        v2 = *(const uint64_a *)(s2 + n - 8);
        if (v1 != v2)
            goto found_diff;
    }

    return 0;

found_diff:

    uint64_t diff_bits = v1 ^ v2;
    int byte_pos = __builtin_ctzll(diff_bits) >> 3;

    unsigned char c1 = reinterpret_cast<const unsigned char *>(&v1)[byte_pos];
    unsigned char c2 = reinterpret_cast<const unsigned char *>(&v2)[byte_pos];

    return (int)c1 - (int)c2;
}

[[clang::always_inline]] inline int c_strcmp_unaligned(const char *s1,
                                                       const char *s2, size_t n)
{

    static const bool fast_512 = CPU_Features::has_512();
    static const bool fast_avx2 = CPU_Features::has_avx2();

    if (fast_512)
        return c_strcmp_unaligned_avx512(s1, s2, n);
    if (fast_avx2)
        return c_strcmp_unaligned_avx2(s1, s2, n);

    return c_strcmp_unaligned_fallback(s1, s2, n);
}

// need align64 and size 128(maybe not checked yhink so)
[[clang::always_inline]] inline int c_strcmp(const char *s1, const char *s2,
                                             size_t n)
{

    // if (UNLIKELY(n > 1024 * 1024)) {
    //     return c_strcmp_erms(s1, s2, n);
    // }

    static const bool fast_512 = CPU_Features::has_512();
    static const bool fast_avx2 = CPU_Features::has_avx2();

    if (fast_512)
        return c_strcmp_avx512(s1, s2, n);
    if (fast_avx2)
        return c_strcmp_avx2(s1, s2, n);

    return c_strcmp_fallback(s1, s2, n);
}

[[clang::always_inline]] inline bool c_quick_strcmp(const char *s1,
                                                    const char *s2, size_t n)
{

    static const bool fast_512 = CPU_Features::has_512();
    static const bool fast_avx2 = CPU_Features::has_avx2();

    if (fast_512)
        return c_quick_strcmp_avx512(s1, s2, n);
    if (fast_avx2)
        return c_quick_strcmp_avx2(s1, s2, n);

    return c_quick_strcmp_fallback(s1, s2, n);
}

// ONLY IF STK = 55!!!!!!!!!!!!!!!
class alignas(64) String
{
  public:
    String()
    {
        set_len(0, false);
        i.data[0] = '\0';
    }

    [[nodiscard]] ALWAYS_INLINE void *internal_alloc(size_t_a req_cap,
                                                     uint8_t &out_pool_id)
    {
        size_t req = req_cap + 1;

        constexpr uint32_t shift = __builtin_ctz(GigaPool::FIRST_SLOT_SIZE);

        uint32_t p_id = 31 - __builtin_clz(req >> shift | 1);

        if (LIKELY(p_id < GigaPool::SLAB_COUNT))
        {
            void *ptr = t_gigaPool.slabs[p_id].alloc();

            if (UNLIKELY(!ptr))
            {
                ptr = t_gigaPool.refill_slab(p_id);
                if (UNLIKELY(!ptr))
                    goto SYSTEM_ALLOC;
            }

            out_pool_id = static_cast<uint8_t>(p_id);
            return ptr;
        }

    SYSTEM_ALLOC:

        out_pool_id = 0xFF;

#if defined(_WIN32)
        return _aligned_malloc(req, 128); // 64 TODO: Test with 128 align
#else
        return std::aligned_alloc(128, req);
#endif
    }

    [[clang::noinline]]
    void init_copy_heap_arena(const String &str, Arena *target_arena)
    {
        size_t_a len = str.get_len();

        void *ptr = target_arena->alloc(this->h.cap + 1);

        if (UNLIKELY(!ptr))
        {

            init_copy_heap(str);
            return;
        }

        this->h.data = static_cast<char *>(ptr);
        this->h.arena = target_arena;
        __builtin_memcpy(__builtin_assume_aligned(this->h.data, 64),
                         __builtin_assume_aligned(str.c_str(), 64), len + 1);
    }
    [[clang::noinline]]
    void init_copy_heap(const String &str)
    {
        size_t_a len = str.get_len();

        this->h.data =
            static_cast<char *>(internal_alloc(h.cap, this->h.pool_id));

        this->h.arena = nullptr;
        __builtin_memcpy(__builtin_assume_aligned(this->h.data, 64),
                         __builtin_assume_aligned(str.c_str(), 64), len + 1);
    }

    // TODO: these opts to other constructors ?
    String(const String &str)
    {
        __builtin_memcpy(__builtin_assume_aligned(this, 64),
                         __builtin_assume_aligned(&str, 64), 64);

        if (UNLIKELY(str.len_flags & 1))
        {

            init_copy_heap(str);
        }
    }

    ALWAYS_INLINE String(const String &str, Arena *target_arena)
    {
        size_t_a len = str.get_len();
        __builtin_memcpy(__builtin_assume_aligned(this, 64),
                         __builtin_assume_aligned(&str, 64), 64);
        if (UNLIKELY(str.len_flags & 1))
        {

            if (target_arena)
            {
                init_copy_heap_arena(str, target_arena);
            }
            else
            {
                init_copy_heap(str);
            }
        }
    }

    String(const String &s1, const String &s2)
    {
        size_t_a t_len = s1.get_len() + s2.get_len();
        set_len(t_len, t_len > Stk);
        char *dest;
        if (is_small())
        {
            dest = i.data;
        }
        else
        {
            h.cap = Calculate_cap(get_len());
            h.data = static_cast<char *>(internal_alloc(h.cap, h.pool_id));
        }

        __builtin_memcpy(__builtin_assume_aligned(dest, 64),
                         __builtin_assume_aligned(s1.c_str(), 64),
                         s1.get_len());
        __builtin_memcpy(__builtin_assume_aligned(dest + s1.get_len(), 64),
                         __builtin_assume_aligned(s2.c_str(), 64),
                         s2.get_len());
        dest[t_len] = '\0';
    }

    void init_heap_string(const char *str, size_t_a s_len)
    {
        h.cap = Calculate_cap(s_len);
        h.data = static_cast<char *>(internal_alloc(h.cap, h.pool_id));

        __builtin_memcpy(__builtin_assume_aligned(h.data, 64), str, s_len);
        h.data[s_len] = '\0';
        set_len(s_len, true);
    }

    [[clang::always_inline]]
    String(const char *str)
    {
        if (UNLIKELY(!str))
        {
            set_len(0, false);
            i.data[0] = '\0';
            return;
        }

        typedef uint64_t __attribute__((__may_alias__)) uint64_a;

        const uint64_a *src = reinterpret_cast<const uint64_a *>(str);

        // SWAR
        uint64_t v = src[0];
        uint64_t has_zero =
            (v - 0x0101010101010101ULL) & ~v & 0x8080808080808080ULL;

        if (has_zero)
        {
            uint64_a *dst = reinterpret_cast<uint64_a *>(i.data);
            dst[0] = v;
            size_t_a l = __builtin_ctzll(has_zero) >> 3;
            set_len(l, false);
            i.data[l] = '\0';
            return;
        }

        // SSO
        size_t_a s_len = c_strLen(str);

        if (LIKELY(s_len <= Stk))
        {
            set_len(s_len, false);
            __builtin_memcpy(__builtin_assume_aligned(i.data, 64), str,
                             s_len + 1);
        }
        else
        {
            init_heap_string(str, s_len);
        }
    }

    String(const char *str, size_t len)
    {
        if (UNLIKELY(!str))
        {
            set_len(0, false);
            i.data[0] = '\0';
            return;
        }

        if (LIKELY(len <= Stk))
        {
            set_len(len, false);
            __builtin_memcpy(__builtin_assume_aligned(i.data, 64), str,
                             len + 1);
        }
        else
        {
            init_heap_string(str, len);
        }
    }

    String(String &&other) noexcept
    {
        len_flags = other.len_flags;
        if (other.is_small())
        {
            __builtin_memcpy(__builtin_assume_aligned(&i, 64),
                             __builtin_assume_aligned(&other.i, 64),
                             sizeof(Inline));
        }
        else
        {
            h.data = other.h.data;
            h.cap = other.h.cap;
            other.h.data = nullptr;
        }
        other.set_len(0, false);
    }

    explicit String(size_t_a reserved_len)
    {
        set_len(reserved_len, reserved_len > Stk);
        if (is_small())
        {
            i.data[reserved_len] = '\0';
        }
        else
        {
            h.cap = Calculate_cap(reserved_len);
            h.data = static_cast<char *>(internal_alloc(h.cap, h.pool_id));
            h.data[reserved_len] = '\0';
        }
    }

    // cp
    String &operator=(const String &other)
    {
        if (this != &other)
        {
            String temp(other);
            std::swap(*this, temp);
        }

        return *this;
    }

    // mv
    String &operator=(String &&other) noexcept
    {
        if (this != &other)
        {
            if (!is_small())
            {
                this->release_resources();
            }
            len_flags = other.len_flags;
            if (other.is_small())
            {
                __builtin_memcpy(__builtin_assume_aligned(&i, 64),
                                 __builtin_assume_aligned(&other.i, 64),
                                 sizeof(Inline));
            }
            else
            {
                h = other.h;
                other.h.data = nullptr;
            }
            other.set_len(0, false);
        }

        return *this;
    }

    ALWAYS_INLINE void release_resources() noexcept
    {
        if (is_small() || !h.data)
            return;

        if (h.arena)
            return;

        if (LIKELY(h.pool_id != 0xFF))
        {
            t_gigaPool.slabs[h.pool_id].free(h.data);
        }

        else
        {
#if defined(_WIN32)
            _aligned_free(h.data);
#else
            free(h.data);
#endif
        }
    }

    ~String() noexcept
    {
        release_resources();
    }

    __attribute__((noinline)) void
    grow_and_append(const char *rhs_ptr, size_t_a rhs_len, size_t_a total_len)
    {
        size_t_a new_cap = Calculate_cap(total_len);
        uint8_t new_pool_id;
        char *newData =
            static_cast<char *>(internal_alloc(new_cap, new_pool_id));

        size_t_a cur_len = get_len();
        char *prev_data = is_small() ? i.data : h.data;

        // cp
        __builtin_memcpy(__builtin_assume_aligned(newData + cur_len, 64),
                         rhs_ptr, rhs_len);

        __builtin_memcpy(__builtin_assume_aligned(newData, 64),
                         __builtin_assume_aligned(prev_data, 64), cur_len);

        newData[total_len] = '\0';

        if (!is_small())
        {
            this->release_resources();
        }

        h.data = newData;
        h.cap = new_cap;
        h.pool_id = new_pool_id;
        h.arena = nullptr;
        set_len(total_len, true);
    }

    [[clang::always_inline]]
    String &append(const char *rhs_ptr, size_t_a rhs_len)
    {
        if (UNLIKELY(rhs_len == 0))
            return *this;

        size_t_a cur_len = get_len();
        size_t_a total_len = cur_len + rhs_len;
        bool is_sm = is_small();
        size_t_a current_cap = is_small() ? Stk : h.cap;

        if (LIKELY(total_len <= current_cap))
        {
            char *dest = (is_sm ? i.data : h.data);

            __builtin_memmove(dest + cur_len, rhs_ptr, rhs_len);

            dest[total_len] = '\0';
            set_len(total_len, !is_sm);
            return *this;
        }

        grow_and_append(rhs_ptr, rhs_len, total_len);
        return *this;
    }

    // TODO: O(M+N) TO O(M) -d CHECK: on large strings
    [[clang::always_inline]]
    String &operator+=(const String &rhs)
    {
        return append(rhs.c_str(), rhs.get_len());
    }

    [[clang::always_inline]]
    String &operator+=(const char *rhs)
    {
        if (UNLIKELY(!rhs))
            return *this;
        return append(rhs, String_lib::c_strLen(rhs));
    }

    char &operator[](size_t_a index)
    {
        if (UNLIKELY(index >= get_len()))
            throw std::out_of_range("index oit of range.");
        return (is_small() ? i.data : h.data)[index];
    }

    const char &operator[](size_t_a index) const
    {
        if (UNLIKELY(index >= get_len()))
            throw std::out_of_range("index oit of range.");
        return (is_small() ? i.data : h.data)[index];
    }

    ALWAYS_INLINE bool operator==(const String &other) const
    {
        size_t_a len = get_len();
        if (len != other.get_len())
            return false;

        if (LIKELY(is_small()))
        {
            return String_lib::internal::compare_sso_64(i.data, other.i.data,
                                                        len);
        }

        return String_lib::c_quick_strcmp(c_str(), other.c_str(), len);
    }

    ALWAYS_INLINE bool operator!=(const String &other) const
    {
        size_t_a len = get_len();
        if (len != other.get_len())
            return true;

        if (is_small() && other.is_small())
        {
            return !String_lib::internal::compare_sso_64(i.data, other.i.data,
                                                         len);
        }

        return !String_lib::c_quick_strcmp(c_str(), other.c_str(), len);
    }

    friend std::ostream &operator<<(std::ostream &os, const String &str)
    {
        os << str.c_str();
        return os;
    }
    // TODO: mmap or fule read lije structure
    friend std::istream &operator>>(std::istream &is, String &str)
    {
        if (!str.is_small())
        {
            str.release_resources();
        }

        str.set_len(0, false);
        str.i.data[0] = '\0'; // SSO

        std::istream::sentry s(is);

        if (!is)
            return is;

        auto *rdbuf = is.rdbuf();
        constexpr size_t_a tmp_size = 512;
        char buffer[tmp_size];

        auto append_chunk = [&](const char *p, size_t_a n)
        {
            if (n == 0)
                return;
            size_t_a prev_len = str.get_len();
            size_t_a new_len = prev_len + n;
            str.Reserve(new_len);
            char *dest = str.is_small() ? str.i.data : str.h.data;
            __builtin_memcpy(dest + prev_len, p, n);
            str.set_len(new_len, !str.is_small());
            dest[new_len] = '\0';
        };

        while (true)
        {
            size_t_a count = 0;
            while (count < tmp_size)
            {
                int c = rdbuf->sgetc();

                if (c == EOF || std::isspace(static_cast<unsigned char>(c)))
                {
                    append_chunk(buffer, count);
                    goto TOHERE;
                }

                buffer[count++] = static_cast<char>(c);
                rdbuf->sbumpc();
            }

            append_chunk(buffer, count);
        }

    TOHERE:
        return is;
    }

    ALWAYS_INLINE size_t_a GetLength() const
    {
        return get_len();
    }

    ALWAYS_INLINE const char *c_str() const
    {
        return is_small() ? i.data : h.data;
    }

    ALWAYS_INLINE char *GetRawData()
    {
        return is_small() ? i.data : h.data;
    }

    __attribute__((noinline)) void _grow_capacity(size_t_a new_cap, bool is_sm)
    {
        size_t_a current_len = get_len();
        size_t_a actual_cap = Calculate_cap(new_cap);

        char *const prev_data = is_sm ? i.data : h.data;
        Arena *const prev_arena = is_sm ? nullptr : h.arena;
        uint8_t old_pool_id = is_sm ? 0 : h.pool_id;

        uint8_t new_pool_id;
        char *newData =
            static_cast<char *>(internal_alloc(actual_cap, new_pool_id));

        __builtin_memcpy(__builtin_assume_aligned(newData, 64),
                         __builtin_assume_aligned(prev_data, 64),
                         current_len + 1);

        if (!is_sm)
        {
            if (old_pool_id == 0xFF)
            {
#if defined(_WIN32)
                _aligned_free(prev_data);
#else
                free(prev_data);
#endif
            }
            else
            {
                t_gigaPool.slabs[old_pool_id].free(prev_data);
            }
        }

        h.data = newData;
        h.cap = actual_cap;
        h.pool_id = new_pool_id;
        h.arena = nullptr;
        set_len(current_len, true);
    }

    void Reserve(size_t_a new_cap)
    {
        bool is_sm = is_small();
        if (LIKELY(new_cap <= (is_sm ? Stk : h.cap)))
            return;

        _grow_capacity(new_cap, is_sm);
    }

    [[clang::always_inline]]
    ALWAYS_INLINE void push_back(char c)
    {

        size_t lf = len_flags;
        size_t sz = lf >> 1;

        size_t cp;
        char *data_ptr;

        if (lf & 1)
        { // Heap path
            cp = h.cap;
            data_ptr = h.data;
        }
        else
        { // SSO path
            cp = Stk;
            data_ptr = i.data;
        }

        if (LIKELY(sz + 1 < cp))
        {
            data_ptr[sz] = c;
            data_ptr[sz + 1] = '\0';

            this->len_flags = lf + 2;
        }
        else
        {

            _slow_push_back(c, sz);
        }
    }

    [[clang::noinline]] void _slow_push_back(char c, size_t sz)
    {
        size_t current_len = get_len();
        _grow_capacity(current_len * 2 + 2049, is_small());

        h.data[sz] = c;
        h.data[sz + 1] = '\0';
        set_len(sz + 1, true);
    }

  private:
    struct Heap
    {
        char *data;
        size_t_a cap;
        Arena *arena;
        uint8_t pool_id;
    };
    static constexpr size_t_a CAP_START = 127; // min 64 !!!!!!!!!!
    static constexpr size_t_a Stk = 55;
    struct Inline
    {
        char data[Stk + 1];
    };

    union
    {
        Heap h;
        Inline i;
    };

    size_t_a len_flags; // len << 1

    ALWAYS_INLINE bool is_small() const
    {
        return !(len_flags & 1);
    }

    ALWAYS_INLINE size_t_a get_len() const
    {
        return len_flags >> 1;
    }

    ALWAYS_INLINE void set_len(size_t_a l, bool heaped)
    {
        len_flags = (l << 1) | (heaped ? 1 : 0);
    }

    constexpr size_t_a Calculate_cap(size_t_a required_len) const noexcept
    {
        if (required_len <= CAP_START)
            return CAP_START;

#if defined(__clang__) || defined(__GNUC__)
        return (1ULL << (64 - __builtin_clzll(required_len))) - 1;
#else
        size_t_a new_cap = CAP_START;
        while (new_cap < required_len)
        {
            new_cap = (new_cap << 1) | 1;
        }
        return new_cap;
#endif

        return required_len; // ERR
    }
};

inline String operator+(String lhs, const String &rhs)
{
    return String(lhs, rhs);
}

inline String operator+(String lhs, const char *rhs)
{
    lhs += rhs;
    return lhs;
}

inline String operator+(const char *lhs, const String &rhs)
{
    String temp(lhs);
    temp += rhs;
    return temp;
}

class StringView
{
  private:
    const char *data;
    size_t_a len;

  public:
    StringView() : data(nullptr), len(0)
    {
    }
    StringView(const char *str, size_t_a l) : data(str), len(l)
    {
    }
    StringView(const String &str) : data(str.c_str()), len(str.GetLength())
    {
    }

    StringView(const StringView &other) : data(other.data), len(other.len)
    {
    }
    ~StringView()
    {
    }

    ALWAYS_INLINE const char *c_str() const
    {
        return data;
    }
    ALWAYS_INLINE size_t_a GetLength() const
    {
        return len;
    }

    ALWAYS_INLINE char operator[](size_t_a index) const
    {
        return data[index];
    }

    // need test
    ALWAYS_INLINE bool operator==(const StringView &rhs) const
    {
        if (len != rhs.len)
            return false;
        if (c_str() == rhs.c_str())
            return true;
        if (LIKELY(len < 55))
        {
            return String_lib::internal::compare_sso_64(data, rhs.data, len);
        }

        return String_lib::c_quick_strcmp(c_str(), rhs.c_str(), len);
    }
};
static_assert(sizeof(String) == 64, "String size mismatch");

} // namespace String_lib

#endif // STRING_HPP