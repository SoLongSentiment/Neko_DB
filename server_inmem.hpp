#include "SparseSet.hpp"
#include <fstream>
#include <map>
#include <queue>

#ifdef _WIN32
#include <process.h>
#include <windows.h>

inline size_t GetFileSize(const char *filename)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(filename, GetFileExInfoStandard, &fad))
    {
        return 0;
    }
    LARGE_INTEGER size;
    size.LowPart = fad.nFileSizeLow;
    size.HighPart = fad.nFileSizeHigh;
    return (size_t)size.QuadPart;
}
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#endif
// TODO: ADD TO OTHER WORKRT!!!!!!!!!!!!!!!!!!!
__attribute__((target("avx512f,avx512bw,avx512vl"))) [[clang::always_inline]]
static uint64_t compute_hash_resp_AVX_512(const char *str, size_t len)
{
    uint64_t h = 0x9e3779b97f4a7c15ULL;
    const uint64_t *p = reinterpret_cast<const uint64_t *>(str);

    size_t blocks = len >> 3; // len / 8

    for (size_t i = 0; i < blocks; ++i)
    {
        h = _mm_crc32_u64(h, p[i]);
    }

    if (len & 7)
    {
        uint64_t tail = 0;

        uint8_t mask = (1 << (len & 7)) - 1;
        _mm_mask_storeu_epi8(&tail, mask,
                             _mm_loadu_si128((__m128i *)(str + (len & ~7ULL))));
        h = _mm_crc32_u64(h, tail);
    }

    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;

    return h;
}

[[clang::always_inline]]
static uint64_t compute_hash_resp_AVX_2(const char *str, size_t len)
{
    uint64_t h = 0x9e3779b97f4a7c15ULL;

    size_t blocks = len >> 3;

    for (size_t i = 0; i < blocks; ++i)
    {
        uint64_t val;

        __builtin_memcpy(&val, str + (i << 3), 8);
        h = _mm_crc32_u64(h, val);
    }

    if (UNLIKELY(len & 7))
    {
        uint64_t tail = 0;

        __builtin_memcpy(&tail, str + (len & ~7ULL), len & 7);
        h = _mm_crc32_u64(h, tail);
    }

    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;

    return h;
}

[[clang::always_inline]]
static inline uint64_t compute_hash_resp(const char *str, size_t len)
{

    static const bool has_512 = String_lib::CPU_Features::has_512();
    static const bool has_avx2 = String_lib::CPU_Features::has_avx2();

    if (has_512)
    {
        return compute_hash_resp_AVX_512(str, len);
    }

    return compute_hash_resp_AVX_2(str, len);
}

// TODP: Test align64
template <size_t Align> class alignas(64) MmapArena
{
    static_assert((Align & (Align - 1)) == 0, "Align must be power of 2");

  private:
    uint8_t *buffer = nullptr;
    size_t capacity;
    size_t offset = 0;

#ifdef _WIN32
    HANDLE hFile = INVALID_HANDLE_VALUE;
    HANDLE hMapping = NULL;
#else
    int fd = -1;
#endif

  public:
    MmapArena(const MmapArena &) = delete;
    MmapArena &operator=(const MmapArena &) = delete;
    MmapArena(MmapArena &&) = default;

    // MmapArena(const MmapArena&) = delete;
    // MmapArena& operator=(const MmapArena&) = delete;
    explicit MmapArena(size_t size, const char *filename) : capacity(size)
    {
        size = (size + 4095) & ~4095;
        if (size > 1ULL * 1024 * 1024 * 1024 * 10 || size == 0)
        {

            buffer = nullptr;
            hFile = INVALID_HANDLE_VALUE;
            return;
        }
#ifdef _WIN32

        size_t aligned_size = (size + 65535) & ~65535;
        // printf("[MAP_DEBUG] TID: %lu | File: %s | hFile: %p | MapHandle: %p |
        // Buffer: %p\n", GetCurrentThreadId(), filename, hFile, hMapping,
        // buffer);
        hFile =
            CreateFileA(filename, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING, NULL);
        if (UNLIKELY(hFile == INVALID_HANDLE_VALUE))
        {
            printf("[FATAL] CreateFileA failed: %lu | File: %s\n",
                   GetLastError(), filename);
            return;
        }
        /*hFile = CreateFileA(filename, GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE |
        FILE_SHARE_DELETE , NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL |
        FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH |
        FILE_FLAG_DELETE_ON_CLOSE, NULL); if (UNLIKELY(hFile ==
        INVALID_HANDLE_VALUE)) { printf("[FATAL] CreateFileA failed: %lu | File:
        %s\n", GetLastError(), filename); return;
        }*/

        LARGE_INTEGER li;
        li.QuadPart = size;

        if (UNLIKELY(!SetFilePointerEx(hFile, li, NULL, FILE_BEGIN)))
        {
            printf("[FATAL] SetFilePointerEx failed: %lu\n", GetLastError());
            CloseHandle(hFile);
            hFile = INVALID_HANDLE_VALUE;
            return;
        }

        if (UNLIKELY(!SetEndOfFile(hFile)))
        {
            printf("[FATAL] SetEndOfFile failed: %lu | Size: %zu\n",
                   GetLastError(), size);
            CloseHandle(hFile);
            hFile = INVALID_HANDLE_VALUE;
            return;
        }

        hMapping = CreateFileMapping(
            hFile, NULL, PAGE_READWRITE | SEC_RESERVE,
            (DWORD)(aligned_size >> 32), (DWORD)(aligned_size & 0xFFFFFFFF),
            NULL); // SEC_RESERVE  (DWORD) 128*1024*1024 ?
        if (UNLIKELY(hMapping == NULL))
        {
            printf("[FATAL] CreateFileMapping failed: %lu | Size: %zu\n",
                   GetLastError(), size);
            CloseHandle(hFile);
            hFile = INVALID_HANDLE_VALUE;
            return;
        }

        buffer =
            (uint8_t *)MapViewOfFile(hMapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
        if (UNLIKELY(!buffer))
        {
            printf("[FATAL] MapViewOfFile failed: %lu\n", GetLastError());
        }
#else
        fd = open(filename, O_RDWR | O_CREAT | O_TRUNC, 0644);
        ftruncate(fd, size);
        buffer = (uint8_t *)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                                 fd, 0);
#endif
    }

    ~MmapArena()
    {
        if (!buffer)
            return;
#ifdef _WIN32
        if (LIKELY(buffer))
        {
            UnmapViewOfFile(buffer);
            buffer = nullptr;
        }
        if (LIKELY(hMapping))
        {
            CloseHandle(hMapping);
            hMapping = NULL;
        }
        if (LIKELY(hFile != INVALID_HANDLE_VALUE))
        {
            CloseHandle(hFile);
            hFile = INVALID_HANDLE_VALUE;
        }
#else
        if (buffer)
        {
            munmap(buffer, capacity);
            buffer = nullptr;
        }
        if (fd != -1)
        {
            close(fd);
            fd = -1;
        }
#endif
    }

    [[nodiscard]] ALWAYS_INLINE void *alloc(size_t n)
    {
        size_t aligned_n = (n + (Align - 1)) & ~(Align - 1);
        if (UNLIKELY(offset + aligned_n > capacity))
            return nullptr;

        void *ptr = buffer + offset;
        offset += aligned_n;
        return ptr;
    }

    void reset()
    {
        offset = 0;
    }
    size_t current_offset() const
    {
        return offset;
    }
    uint8_t *data()
    {
        return buffer;
    }
    void truncate_to(size_t final_size)
    {
#ifdef _WIN32
        if (LIKELY(buffer))
        {
            UnmapViewOfFile(buffer);
            buffer = nullptr;
        }
        if (LIKELY(hMapping))
        {
            CloseHandle(hMapping);
            hMapping = nullptr;
        }

        if (UNLIKELY(hFile == INVALID_HANDLE_VALUE))
            return;

        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)final_size;
        SetFilePointerEx(hFile, li, NULL, FILE_BEGIN);
        if (UNLIKELY(!SetEndOfFile(hFile)))
        {

            // printf("Truncate failed: %lu\n", GetLastError());
        }
#else
        if (buffer)
        {
            munmap(buffer, capacity);
            buffer = nullptr;
        }
        ftruncate(fd, final_size);
#endif
    }
};

struct alignas(64) ThreadMetrics
{
    uint64_t total_ops = 0;
    uint64_t parse_cycles = 0;
    uint64_t exec_cycles = 0;
    uint64_t send_cycles = 0;
    uint64_t recv_cycles = 0;
    uint64_t redirects = 0;
    uint64_t parse_fails = 0;
};

ThreadMetrics *global_metrics;

void PrintProfilerReport(int total_workers)
{

    for (int i = 0; i < total_workers; ++i)
    {
        ThreadMetrics &m = global_metrics[i];

        if (m.total_ops == 0)
            continue;

        printf("\n--- Thread %d Report ---\n", i);
        printf("Avg Parse: %llu cycles\n", m.parse_cycles / m.total_ops);
        printf("Avg Exec:  %llu cycles\n", m.exec_cycles / m.total_ops);
        printf("Avg Send:  %llu cycles (Syscalls)\n",
               m.send_cycles / m.total_ops);
        printf("Redirects: %llu | ParseFails: %llu\n", m.redirects,
               m.parse_fails);
    }
}

#pragma comment(lib, "ws2_32.lib")

#define LOG_STEP(step_name)                                                    \
    printf("[Core %d] %s | Bytes: %u | Key: %llx | Ov: %p\n", thread_id,       \
           step_name, (uint32_t)bytesTransferred,                              \
           (unsigned long long)completionKey, (void *)overlapped);             \
    fflush(stdout);
std::atomic<uint64_t> g_bench_ops{0};
std::atomic<uint64_t> g_errors{0};

// helper function for key making
inline std::string make_key(int thread_id, int op_id)
{
    return "key:" + std::to_string(thread_id) + ":" + std::to_string(op_id);
}

inline std::string make_val(int thread_id, int op_id)
{
    return "val:" + std::to_string(thread_id) + ":" + std::to_string(op_id) +
           "_long_suffix_to_trigger_arena_fill";
}

void stress_worker_set(int tid, int ops)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{AF_INET, (unsigned short)htons(6379)};
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(s, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        std::cout << "Thread " << tid << " failed to connect!" << std::endl;
        return;
    }

    std::string send_buf;
    send_buf.reserve(128 * 1024);
    char recv_tmp[4096];

    for (int i = 0; i < ops; ++i)
    {
        std::string k = make_key(tid, i);
        std::string v = make_val(tid, i);

        send_buf += "*3\r\n$3\r\nSET\r\n$" + std::to_string(k.length()) +
                    "\r\n" + k + "\r\n" + "$" + std::to_string(v.length()) +
                    "\r\n" + v + "\r\n";

        if (UNLIKELY(i % 64 == 0 || i == ops - 1))
        {
            if (send(s, send_buf.data(), (int)send_buf.size(), 0) ==
                SOCKET_ERROR)
                break;
            send_buf.clear();

            int total_expected = (i % 64 == 0 && i != 0) ? 64 : (i % 64);
            if (i == ops - 1)
                total_expected = 64; // tail

            recv(s, recv_tmp, sizeof(recv_tmp), 0);
        }

        g_bench_ops.fetch_add(1, std::memory_order_relaxed);
    }

    shutdown(s, SD_SEND);

    char drain[4096];
    while (recv(s, drain, sizeof(drain), 0) > 0)
        ;
    closesocket(s);
}

void stress_worker_get_verify(int tid, int ops)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{AF_INET, (unsigned short)htons(6379)};
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(s, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR)
        return;

    std::string send_buf;
    char recv_buf[8192];

    for (int i = 0; i < ops; ++i)
    {
        std::string k = make_key(tid, i);
        std::string expected_v = make_val(tid, i);

        send_buf = "*2\r\n$3\r\nGET\r\n$" + std::to_string(k.length()) +
                   "\r\n" + k + "\r\n";
        send(s, send_buf.data(), (int)send_buf.size(), 0);

        int len = recv(s, recv_buf, sizeof(recv_buf) - 1, 0);
        if (len > 0)
        {
            recv_buf[len] = '\0';
            std::string response(recv_buf, len);

            size_t first_nl = response.find("\r\n");
            if (first_nl != std::string::npos)
            {
                std::string actual_v =
                    response.substr(first_nl + 2, expected_v.length());

                if (actual_v != expected_v)
                {
                    g_errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
            else
            {
                g_errors.fetch_add(1, std::memory_order_relaxed);
            }
        }
        else
        {
            g_errors.fetch_add(1, std::memory_order_relaxed);
        }

        g_bench_ops.fetch_add(1, std::memory_order_relaxed);
    }

    shutdown(s, SD_SEND);

    char drain[4096];
    while (recv(s, drain, sizeof(drain), 0) > 0)
        ;
    closesocket(s);
}

// TODO: Blocks -d
__attribute__((always_inline)) inline bool
LexisReverseCompare(const String_lib::StringView &lhs,
                    const String_lib::StringView &rhs)
{
    size_t len1 = lhs.GetLength();
    size_t len2 = rhs.GetLength();
    size_t min_len = (len1 < len2) ? len1 : len2;

    const char *p1 = lhs.c_str();
    const char *p2 = rhs.c_str();

    // Prepass
    if (min_len >= 8)
    {
        uint64_t u1 =
            __builtin_bswap64(*reinterpret_cast<const uint64_t *>(p1));
        uint64_t u2 =
            __builtin_bswap64(*reinterpret_cast<const uint64_t *>(p2));
        if (u1 != u2)
            return u1 > u2;

        int res = String_lib::c_strcmp_unaligned(p1 + 8, p2 + 8, min_len - 8);
        if (res != 0)
            return res > 0;
    }
    else
    {

        int res = String_lib::c_strcmp_unaligned(p1, p2, min_len);
        if (res != 0)
            return res > 0;
    }

    return len1 > len2;
}

struct SoftTicket
{
    String_lib::StringView view;
    size_t original_idx;

    bool operator>(const SoftTicket &other) const
    {
        return LexisReverseCompare(view, other.view);
    }
};

struct MmapReader
{
    const char *m_data = nullptr;
    size_t m_size = 0;
    size_t m_offset = 0;

#ifdef _WIN32
    HANDLE hFile = INVALID_HANDLE_VALUE;
    HANDLE hMapping = NULL;
#else
    int fd = -1;
#endif

    MmapReader(const char *filename)
    {
#ifdef _WIN32
        hFile = CreateFileA(
            filename, GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            NULL); // FILE_FLAG_SEQUENTIAL_SCAN?
        if (hFile != INVALID_HANDLE_VALUE)
        {
            m_size = GetFileSize(hFile, NULL);
            hMapping =
                CreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
            if (hMapping)
                m_data = (const char *)MapViewOfFile(hMapping, FILE_MAP_READ, 0,
                                                     0, 0);
        }
#else
        fd = open(filename, O_RDONLY);
        struct stat st;
        if (fd != -1 && fstat(fd, &st) == 0)
        {
            m_size = st.st_size;
            m_data =
                (const char *)mmap(NULL, m_size, PROT_READ, MAP_PRIVATE, fd, 0);
        }
#endif
    }

    ~MmapReader()
    {
#ifdef _WIN32
        if (m_data)
            UnmapViewOfFile(m_data);
        if (hMapping)
            CloseHandle(hMapping);
        if (hFile != INVALID_HANDLE_VALUE)
            CloseHandle(hFile);
#else
        if (m_data)
            munmap((void *)m_data, m_size);
        if (fd != -1)
            close(fd);
#endif
    }

    // read [len_norm][data_norm][len_orig][data_orig]
    bool next_pair(String_lib::StringView &norm, String_lib::StringView &orig)
    {

        m_offset = (m_offset + 7) & ~7ULL;

        if (UNLIKELY(m_offset + 8 > m_size))
            return false;

        _mm_prefetch(m_data + m_offset + 512, _MM_HINT_T0);

        uint64_t header;
        __builtin_memcpy(&header, m_data + m_offset, 8);
        m_offset += 8;

        uint32_t n_len = (uint32_t)(header & 0xFFFFFFFF);
        uint32_t o_len = (uint32_t)(header >> 32);

        if (UNLIKELY(n_len == 0 && m_offset < m_size))
            return false;

        if (UNLIKELY(m_offset + n_len > m_size))
            return false;
        norm = String_lib::StringView(m_data + m_offset, n_len);
        m_offset += n_len;

        if (o_len == 0xFFFFFFFF)
        {
            orig = String_lib::StringView(nullptr, 0);
        }
        else
        {
            if (UNLIKELY(m_offset + o_len > m_size))
                return false;
            orig = String_lib::StringView(m_data + m_offset, o_len);
            m_offset += o_len;
        }

        return true;
    }
};

void bench_worker(const char *ip, int port, int ops_per_thread)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{AF_INET, htons(port)};
    addr.sin_addr.s_addr = inet_addr(ip);

    if (connect(s, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        std::cout << "Bench worker failed to connect!" << std::endl;
        return;
    }

    char buf[1024];
    char recv_buf[1024];

    for (int i = 0; i < ops_per_thread; ++i)
    {

        int key_id = rand() % 10000;

        int pay_len = sprintf(
            buf, "*3\r\n$3\r\nSET\r\n$8\r\nkey:%04d\r\n$4\r\nval1\r\n", key_id);

        if (send(s, buf, pay_len, 0) == SOCKET_ERROR)
            break;

        if (recv(s, recv_buf, 1024, 0) <= 0)
            break;

        g_bench_ops.fetch_add(1, std::memory_order_relaxed);
    }
    closesocket(s);
}

void bench_worker_pipeline(const char *ip, int port, int total_ops)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{AF_INET, htons(port)};
    addr.sin_addr.s_addr = inet_addr(ip);

    if (connect(s, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        std::cout << "Bench worker failed to connect!" << std::endl;
        return;
    }

    int pipeline_size = 4;
    char recv_buf[4096];

    for (int i = 0; i < total_ops / pipeline_size; ++i)
    {

        for (int j = 0; j < pipeline_size; j++)
        {
            char send_buf[128];
            int len = sprintf(
                send_buf, "*3\r\n$3\r\nSET\r\n$8\r\nkey:%04d\r\n$4\r\nval1\r\n",
                rand() % 10000);
            send(s, send_buf, len, 0);
        }

        int expected = pipeline_size * 5;
        int received_total = 0;

        while (received_total < expected)
        {
            int n = recv(s, recv_buf + received_total,
                         expected - received_total, 0);
            if (n <= 0)
                break;
            received_total += n;
        }

        g_bench_ops.fetch_add(pipeline_size, std::memory_order_relaxed);
    }
    closesocket(s);
}

/*void bench_worker_pipeline_fast(const char* ip, int port, int total_ops) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{AF_INET, htons(port)};
    addr.sin_addr.s_addr = inet_addr(ip);

    if (connect(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) return;

    int nodelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(int));

    const int pipeline_size = 256;
    std::string pipeline_buffer;

    int expected_bytes = pipeline_size * 5;
    std::vector<char> recv_buf(expected_bytes);

    for (int i = 0; i < total_ops / pipeline_size; ++i) {
        pipeline_buffer.clear();
        for(int j = 0; j < pipeline_size; j++) {
            char cmd[256];

            int current_key_id = (i * pipeline_size + j) % 1000000;

            int len = sprintf(cmd,
"*3\r\n$3\r\nSET\r\n$10\r\nkey:%06d\r\n$4\r\nval1\r\n", current_key_id);
            pipeline_buffer.append(cmd, len);
        }

        send(s, pipeline_buffer.data(), (int)pipeline_buffer.size(), 0);

        int received = 0;
        while (received < expected_bytes) {
            int n = recv(s, recv_buf.data() + received, expected_bytes -
received, 0); if (n <= 0) break; received += n;
        }
        g_bench_ops.fetch_add(pipeline_size, std::memory_order_relaxed);
    }
    closesocket(s);
}

void bench_worker_get_fast(const char* ip, int port, int total_ops) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{AF_INET, htons(port)};
    addr.sin_addr.s_addr = inet_addr(ip);

    if (connect(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) return;

    int nodelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(int));

    const int pipeline_size = 128;
    std::string pipeline_buffer;

    const std::string expected_resp = "$4\r\nval1\r\n";
    const int bytes_per_get = (int)expected_resp.size();
    const int total_expected = pipeline_size * bytes_per_get;

    std::vector<char> recv_buf(total_expected + 1024);

    for (int i = 0; i < total_ops / pipeline_size; ++i) {

        pipeline_buffer.clear();
        for(int j = 0; j < pipeline_size; j++) {
            char cmd[128];
            int current_key_id = (i * pipeline_size + j) % 1000000;

            int len = sprintf(cmd, "*2\r\n$3\r\nGET\r\n$10\r\nkey:%06d\r\n",
current_key_id); pipeline_buffer.append(cmd, len);
        }

        if (send(s, pipeline_buffer.data(), (int)pipeline_buffer.size(), 0) ==
SOCKET_ERROR) break;

        int received = 0;
        while (received < total_expected) {
            int n = recv(s, recv_buf.data() + received, total_expected -
received, 0); if (n <= 0) goto cleanup; received += n;
        }


        for (int j = 0; j < pipeline_size; ++j) {
            const char* current_ptr = recv_buf.data() + (j * bytes_per_get);

            if (memcmp(current_ptr, expected_resp.data(), bytes_per_get) != 0) {
                static std::atomic<bool> logged{false};
                if (!logged.exchange(true)) {
                    printf("\n!!! VERIFICATION FAILED !!!\n");
                    printf("Iteration: %d, Key Index: %d\n", i, j);
                    printf("Expected: %s", expected_resp.c_str());


                    char actual_str[11] = {0};
                    memcpy(actual_str, current_ptr, 10);
                    printf("Actual:   %s\n", actual_str);

                    if (actual_str[0] == '$' && actual_str[1] == '-' &&
actual_str[2] == '1') { printf("Reason: Key not found ($-1)\n");
                    }
                }
                g_errors.fetch_add(1, std::memory_order_relaxed);
            }
        }
        g_bench_ops.fetch_add(pipeline_size, std::memory_order_relaxed);
    }

cleanup:
    closesocket(s);
}*/

void bench_worker_pipeline_fast(const char *ip, int port, int total_ops)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{AF_INET, htons(port)};
    addr.sin_addr.s_addr = inet_addr(ip);

    if (connect(s, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        std::cout << "Bench worker failed to connect!" << std::endl;
        return;
    }

    int nodelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay,
               sizeof(int));

    int pipeline_size = 512;
    std::string pipeline_buffer;

    for (int j = 0; j < pipeline_size; j++)
    {
        char cmd[256];
        int len = sprintf(
            cmd, "*3\r\n$3\r\nSET\r\n$8\r\nkey:%04d\r\n$4\r\nval1\r\n", j);
        pipeline_buffer.append(cmd, len);
    }

    int expected_bytes = pipeline_size * 5;
    char recv_buf[8192];

    for (int i = 0; i < total_ops / pipeline_size; ++i)
    {

        send(s, pipeline_buffer.data(), pipeline_buffer.size(), 0);

        int received = 0;
        while (received < expected_bytes)
        {
            int n = recv(s, recv_buf + received, expected_bytes - received, 0);
            if (n <= 0)
                break;
            received += n;
        }
        g_bench_ops.fetch_add(pipeline_size, std::memory_order_relaxed);
    }

    closesocket(s);
}

void bench_worker_get_fast(const char *ip, int port, int total_ops)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{AF_INET, htons(port)};
    addr.sin_addr.s_addr = inet_addr(ip);

    if (connect(s, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        std::cout << "Bench GET worker failed to connect!" << std::endl;
        return;
    }

    int nodelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay,
               sizeof(int));

    int pipeline_size = 512;
    std::string pipeline_buffer;

    for (int j = 0; j < pipeline_size; j++)
    {
        char cmd[128];
        // RESP GET: *2\r\n$3\r\nGET\r\n$8\r\nkey:XXXX\r\n
        int len =
            sprintf(cmd, "*2\r\n$3\r\nGET\r\n$8\r\nkey:%04d\r\n", j % 10000);
        pipeline_buffer.append(cmd, len);
    }

    int bytes_per_ok_get = 10;
    int expected_bytes = pipeline_size * bytes_per_ok_get;

    char recv_buf[16384];

    for (int i = 0; i < total_ops / pipeline_size; ++i)
    {

        if (send(s, pipeline_buffer.data(), (int)pipeline_buffer.size(), 0) ==
            SOCKET_ERROR)
            break;

        int received = 0;
        while (received < expected_bytes)
        {
            int n = recv(s, recv_buf + received, expected_bytes - received, 0);
            if (n <= 0)
                break;
            received += n;
        }
        g_bench_ops.fetch_add(pipeline_size, std::memory_order_relaxed);
    }

    closesocket(s);
}

void start_benchmark(int num_threads, int ops_per_thread)
{
    std::vector<std::thread> threads;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_threads; ++i)
    {
        threads.emplace_back(bench_worker_pipeline_fast, "127.0.0.1", 6379,
                             ops_per_thread);
    }

    for (auto &t : threads)
        t.join();

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    std::cout << "Bench Results: " << g_bench_ops << " ops in " << diff.count()
              << "s\n";
    std::cout << "Avg RPS: " << (double)g_bench_ops / diff.count() << "\n";
}

void start_full_stress_test(int num_threads, int ops_per_thread)
{

    g_bench_ops.store(0, std::memory_order_relaxed);

    std::cout << "--- PHASE 1: WRITING (SET) ---" << std::endl;
    auto start_set = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> set_threads;
    for (int i = 0; i < num_threads; ++i)
    {
        set_threads.emplace_back(bench_worker_pipeline_fast, "127.0.0.1", 6379,
                                 ops_per_thread);
    }
    for (auto &t : set_threads)
        t.join();

    auto end_set = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff_set = end_set - start_set;
    uint64_t total_set = g_bench_ops.load();

    std::cout << "SET Done: " << total_set << " ops in " << diff_set.count()
              << "s | RPS: " << (double)total_set / diff_set.count() << "\n\n";

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    g_bench_ops.store(0, std::memory_order_relaxed);

    std::cout << "--- PHASE 2: READING (GET) ---" << std::endl;
    auto start_get = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> get_threads;
    for (int i = 0; i < num_threads; ++i)
    {
        get_threads.emplace_back(bench_worker_get_fast, "127.0.0.1", 6379,
                                 ops_per_thread);
    }
    for (auto &t : get_threads)
        t.join();

    auto end_get = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff_get = end_get - start_get;
    uint64_t total_get = g_bench_ops.load();

    std::cout << "GET Done: " << total_get << " ops in " << diff_get.count()
              << "s | RPS: " << (double)total_get / diff_get.count() << "\n";
    std::cout << "------------------------------" << std::endl;

    if (g_errors == 0)
        std::cout << "\n>>> TEST PASSED: DATA IS PERSISTENT AND CONSISTENT! <<<"
                  << std::endl;
    else
        std::cout << "\n>>> TEST FAILED: " << g_errors
                  << " DATA CORRUPTIONS! <<<" << std::endl;
}

struct GlobalStats
{
    std::atomic<uint64_t> total_ops{0};
    std::atomic<uint64_t> active_conns{0};
} g_stats;

namespace RESP_Utils
{

static const char digits100[200] = {
    '0', '0', '0', '1', '0', '2', '0', '3', '0', '4', '0', '5', '0', '6', '0',
    '7', '0', '8', '0', '9', '1', '0', '1', '1', '1', '2', '1', '3', '1', '4',
    '1', '5', '1', '6', '1', '7', '1', '8', '1', '9', '2', '0', '2', '1', '2',
    '2', '2', '3', '2', '4', '2', '5', '2', '6', '2', '7', '2', '8', '2', '9',
    '3', '0', '3', '1', '3', '2', '3', '3', '3', '4', '3', '5', '3', '6', '3',
    '7', '3', '8', '3', '9', '4', '0', '4', '1', '4', '2', '4', '3', '4', '4',
    '4', '5', '4', '6', '4', '7', '4', '8', '4', '9', '5', '0', '5', '1', '5',
    '2', '5', '3', '5', '4', '5', '5', '5', '6', '5', '7', '5', '8', '5', '9',
    '6', '0', '6', '1', '6', '2', '6', '3', '6', '4', '6', '5', '6', '6', '6',
    '7', '6', '8', '6', '9', '7', '0', '7', '1', '7', '2', '7', '3', '7', '4',
    '7', '5', '7', '6', '7', '7', '7', '8', '7', '9', '8', '0', '8', '1', '8',
    '2', '8', '3', '8', '4', '8', '5', '8', '6', '8', '7', '8', '8', '8', '9',
    '9', '0', '9', '1', '9', '2', '9', '3', '9', '4', '9', '5', '9', '6', '9',
    '7', '9', '8', '9', '9'};

ALWAYS_INLINE int write_len_bulk(char *buf, size_t len)
{
    char temp[32];
    char *p = temp + 30;

    *p-- = '\n';
    *p-- = '\r';

    size_t l = len;
    while (l >= 100)
    {
        const uint32_t i = (l % 100) * 2;
        l /= 100;
        *p-- = digits100[i + 1];
        *p-- = digits100[i];
    }
    if (l >= 10)
    {
        const uint32_t i = l * 2;
        *p-- = digits100[i + 1];
        *p-- = digits100[i];
    }
    else
    {
        *p-- = (char)('0' + l);
    }

    *p = '$';

    int written = (int)((temp + 30) - p + 1);
    __builtin_memcpy(buf, p, written);
    return written;
}
} // namespace RESP_Utils

namespace RESP_Parser
{

ALWAYS_INLINE bool is_set(const char *cmd, size_t len)
{
    if (len != 3 || !cmd)
        return false;

    uint32_t val = *reinterpret_cast<const uint32_t *>(cmd);
    return (val & 0x00FFFFFF) == 0x544553;
}
// TODO: address len in parse not inside functiobm
ALWAYS_INLINE bool is_get(const char *cmd, size_t len)
{
    if (len != 3 || !cmd)
        return false;

    uint32_t val = *reinterpret_cast<const uint32_t *>(cmd);
    return (val & 0x00FFFFFF) == 0x544547;
}

ALWAYS_INLINE bool is_del(const char *cmd, size_t len)
{
    if (len != 3 || !cmd)
        return false;

    uint32_t val = *reinterpret_cast<const uint32_t *>(cmd);

    return (val & 0x00FFFFFF) == 0x4C4544;
}

ALWAYS_INLINE bool is_wks(const char *cmd, size_t len)
{
    if (len != 3 || !cmd)
        return false;

    uint32_t val = *reinterpret_cast<const uint32_t *>(cmd);
    return (val & 0x00FFFFFF) == 0x534B57;
}

ALWAYS_INLINE const char *find_eol_fallback(const char *data, size_t len)
{

    return (const char *)memchr(data, '\r', len);
}

ALWAYS_INLINE const char *find_eol_avx512(const char *data, size_t len)
{
    if (len < 64)
        return (const char *)memchr(data, '\r', len);

    const __m512i target = _mm512_set1_epi8('\r');
    size_t processed = 0;

    for (; processed + 64 <= len; processed += 64)
    {
        __m512i chunk = _mm512_loadu_si512((const __m512i *)(data + processed));

        uint64_t mask = _mm512_cmpeq_epi8_mask(chunk, target);

        if (mask != 0)
        {

            return data + processed + __builtin_ctzll(mask);
        }
    }

    return (const char *)memchr(data + processed, '\r', len - processed);
}

__attribute__((target("avx512f,avx512bw,avx512vl"))) ALWAYS_INLINE const char *
find_eol_ultra(const char *data, size_t len)
{
    if (UNLIKELY(len == 0))
        return nullptr;

    const __m512i target = _mm512_set1_epi8('\r');
    size_t processed = 0;

    for (; processed + 64 <= len; processed += 64)
    {

        __m512i chunk = _mm512_loadu_si512((const __m512i *)(data + processed));
        uint64_t mask = _mm512_cmpeq_epi8_mask(chunk, target);
        if (UNLIKELY(mask))
            return data + processed + __builtin_ctzll(mask);
    }

    if (processed < len)
    {
        uint64_t tail_mask = _bzhi_u64(-1ULL, len - processed);
        __m512i chunk = _mm512_maskz_loadu_epi8(tail_mask, data + processed);
        uint64_t mask = _mm512_mask_cmpeq_epi8_mask(tail_mask, chunk, target);
        if (mask)
            return data + processed + __builtin_ctzll(mask);
    }
    return nullptr;
}

ALWAYS_INLINE const char *find_eol_avx2(const char *data, size_t len)
{
    if (len < 32)
        return (const char *)memchr(data, '\r', len);

    const __m256i target = _mm256_set1_epi8('\r');
    size_t processed = 0;

    for (; processed + 32 <= len; processed += 32)
    {
        __m256i chunk = _mm256_loadu_si256((const __m256i *)(data + processed));
        uint32_t mask = _mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, target));

        if (mask != 0)
        {
            return data + processed + __builtin_ctz(mask);
        }
    }
    return (const char *)memchr(data + processed, '\r', len - processed);
}

ALWAYS_INLINE const char *find_eol(const char *data, size_t len)
{
    static const bool has_512 = String_lib::CPU_Features::has_512();
    static const bool has_avx2 = String_lib::CPU_Features::has_avx2();

    if (has_512)
        return find_eol_ultra(data, len);
    if (has_avx2)
        return find_eol_avx2(data, len);

    return find_eol_fallback(data, len);
}

} // namespace RESP_Parser

struct alignas(64) ConnectionContext
{
    OVERLAPPED overlapped;
    WSABUF wsaBuf;
    SOCKET socket;
    uint32_t already_received = 0;

    uint32_t responseLen = 0;
    uint32_t batch_size;
    int owner_thread_id;

    bool is_reading;
    bool is_dead = false;
#ifndef FAST_PATH
    bool is_fast_path = false;
#endif

    String_lib::String buffer;

    char responseBuf[8192];

    ConnectionContext(SOCKET s, int tid)
        : socket(s), owner_thread_id(tid), is_reading(true), buffer(8192)
    {
        memset(&overlapped, 0, sizeof(OVERLAPPED));
        wsaBuf.buf = buffer.GetRawData();
        already_received = 0;
        wsaBuf.len = 8192;
    }
};

/*struct alignas(64)
 ConnectionContext {
    OVERLAPPED overlapped;
     WSABUF wsaBuf;
    SOCKET socket;
    uint32_t already_received = 0;
    bool is_reading;
    bool is_dead = false;
    #ifndef FAST_PATH
    bool is_fast_path = false;
    #endif


    alignas(64) uint32_t batch_size;
    int owner_thread_id;
    String_lib::String buffer;


alignas(64)
    char responseBuf[8192];
    uint32_t responseLen = 0;



    ConnectionContext(SOCKET s, int tid) : socket(s), owner_thread_id(tid),
is_reading(true), buffer(8192) { memset(&overlapped, 0, sizeof(OVERLAPPED));
        wsaBuf.buf = buffer.GetRawData();
        already_received = 0;
        wsaBuf.len = 8192;
    }
};*/

struct ShardMessage
{
    ConnectionContext *ctx;
};

constexpr uint32_t GET_SLAB_IDX(size_t sz)
{
    if (sz <= 128)
        return 0;
    if (sz <= 256)
        return 1;
    if (sz <= 512)
        return 2;
    if (sz <= 1024)
        return 3;
    if (sz <= 2048)
        return 4;
    if (sz <= 4096)
        return 5;
    return 6; // 8192
}

struct StringHasher
{
    using is_transparent = void;

    [[clang::always_inline]]
    static uint64_t compute_hash(const char *str, size_t len)
    {

        uint64_t h = 0x9e3779b97f4a7c15ULL;

        size_t blocks = len / 8;
        const uint64_t *p = reinterpret_cast<const uint64_t *>(str);

        for (size_t i = 0; i < blocks; ++i)
        {
            uint64_t block;

            __builtin_memcpy(&block, p + i, 8);
            h = _mm_crc32_u64(h, block);
        }

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

    size_t operator()(const String_lib::String &s) const
    {
        return compute_hash(s.c_str(), s.GetLength());
    }

    size_t operator()(const String_lib::StringView &s) const
    {
        return compute_hash(s.c_str(), s.GetLength());
    }

    size_t operator()(const std::string &s) const
    {
        return compute_hash(s.data(), s.length());
    }

    size_t operator()(const char *s) const
    {
        return compute_hash(s, std::strlen(s));
    }
};

struct StringEq
{
    using is_transparent = void;

    ALWAYS_INLINE bool operator()(const String_lib::StringView &lhs,
                                  const String_lib::StringView &rhs) const
    {
        return lhs == rhs;
    }

    ALWAYS_INLINE bool operator()(const String_lib::String &lhs,
                                  const String_lib::String &rhs) const
    {
        return lhs == rhs;
    }

    ALWAYS_INLINE bool operator()(const String_lib::String &lhs,
                                  const String_lib::StringView &rhs) const
    {
        if (lhs.GetLength() != rhs.GetLength())
            return false;
        return String_lib::c_quick_strcmp(lhs.c_str(), rhs.c_str(),
                                          lhs.GetLength()) == 0;
    }

    ALWAYS_INLINE bool operator()(const String_lib::StringView &lhs,
                                  const String_lib::String &rhs) const
    {
        return (*this)(rhs, lhs);
    }
};

enum class CommandType : uint8_t
{
    NONE = 0,
    SET = 1 << 0,
    GET = 1 << 1,
    DEL = 1 << 2,
    WKS = 1 << 3,
    MSET = 1 << 4,
    MGET = 1 << 5,
};

// TODO: ADD STRINGVIEW AND CHECK PERFStringView key; StringView val;
/*struct ParseResult {

    const char* key_ptr = nullptr;
    size_t key_len = 0;
    const char* val_ptr = nullptr;
    size_t val_len = 0;
    CommandType type = CommandType::NONE;
    bool success = false;
    size_t consumed = 0;
};*/

struct ParseResult
{

    const char *key_ptr = nullptr;
    const char *val_ptr = nullptr;
    size_t key_len = 0;
    size_t val_len = 0;
    size_t consumed = 0;

    CommandType type = CommandType::NONE;
    bool success = false;
};

namespace Radix_Internal
{

struct SortKey
{
    uint64_t head;
    uint32_t entity;
};

template <typename T> struct is_pair : std::false_type
{
};
template <typename T1, typename T2>
struct is_pair<std::pair<T1, T2>> : std::true_type
{
};

ALWAYS_INLINE uint32_t get_length(const String_lib::StringView &v)
{
    return static_cast<uint32_t>(v.GetLength());
}

ALWAYS_INLINE const char *get_c_str(const String_lib::StringView &v)
{
    return v.c_str();
}

template <typename V1, typename V2>
ALWAYS_INLINE uint32_t get_length(const std::pair<V1, V2> &p)
{
    return get_length(p.first);
}

template <typename V1, typename V2>
ALWAYS_INLINE const char *get_c_str(const std::pair<V1, V2> &p)
{
    return get_c_str(p.first);
}

ALWAYS_INLINE uint32_t get_length(const SoftTicket &t)
{
    return static_cast<uint32_t>(t.view.GetLength());
}

ALWAYS_INLINE const char *get_c_str(const SoftTicket &t)
{
    return t.view.c_str();
}

template <typename T>
ALWAYS_INLINE uint8_t get_byte(const T &ticket, size_t offset)
{
    if (offset >= get_length(ticket))
        return 0;

    if constexpr (std::is_same_v<T, String_lib::StringView>)
    {
        return static_cast<uint8_t>(ticket.c_str()[offset]);
    }
    else if constexpr (is_pair<T>::value)
    {
        return static_cast<uint8_t>(ticket.first.c_str()[offset]);
    }
    else
    {

        return static_cast<uint8_t>(ticket.view.c_str()[offset]);
    }
}

ALWAYS_INLINE const String_lib::StringView &
get_view(const String_lib::StringView &v)
{
    return v;
}

template <typename K, typename V>
ALWAYS_INLINE const String_lib::StringView &get_view(const std::pair<K, V> &p)
{
    return p.first;
}

template <typename T>
ALWAYS_INLINE const String_lib::StringView &get_view(const T &t)
{
    return t.view;
}

template <typename T>
void RadSortIdxInternal(uint32_t *idx_begin, uint32_t *idx_end, T *base_data,
                        uint8_t *bytes_pool, size_t offset)
{
restart:
    const size_t count = idx_end - idx_begin;
    if (count < 64)
    {
        for (uint32_t *i = idx_begin + 1; i < idx_end; ++i)
        {
            uint32_t val = *i;
            uint32_t *j = i;
            while (j > idx_begin &&
                   LexisReverseCompare(get_view(base_data[val]),
                                       get_view(base_data[*(j - 1)])))
            {
                *j = *(j - 1);
                --j;
            }
            *j = val;
        }
        return;
    }

    uint8_t *bytes = bytes_pool;

    uint32_t c0[256] = {0}, c1[256] = {0}, c2[256] = {0}, c3[256] = {0};

    size_t i = 0;
    for (; i + 3 < count; i += 4)
    {
        uint8_t b0 = get_byte(base_data[idx_begin[i]], offset);
        uint8_t b1 = get_byte(base_data[idx_begin[i + 1]], offset);
        uint8_t b2 = get_byte(base_data[idx_begin[i + 2]], offset);
        uint8_t b3 = get_byte(base_data[idx_begin[i + 3]], offset);
        bytes[i] = b0;
        c0[b0]++;
        bytes[i + 1] = b1;
        c1[b1]++;
        bytes[i + 2] = b2;
        c2[b2]++;
        bytes[i + 3] = b3;
        c3[b3]++;
    }
    for (; i < count; ++i)
    {
        uint8_t b = get_byte(base_data[idx_begin[i]], offset);
        bytes[i] = b;
        c0[b]++;
    }

    uint32_t counts[256];
    for (int j = 0; j < 256; ++j)
        counts[j] = c0[j] + c1[j] + c2[j] + c3[j];

    for (int j = 0; j < 256; ++j)
    {
        if (counts[j] == count)
        {
            if (offset < 63 && j != 0)
            {
                offset++;
                goto restart;
            }
            return;
        }
    }

    uint32_t offsets[256], pos = 0;
    for (int j = 255; j >= 0; --j)
    {
        offsets[j] = pos;
        pos += counts[j];
    }
    uint32_t active_offsets[256];
    std::copy(std::begin(offsets), std::end(offsets),
              std::begin(active_offsets));

    for (int j = 255; j >= 0; --j)
    {
        if (counts[j] == 0)
            continue;
        uint32_t limit = offsets[j] + counts[j];
        while (active_offsets[j] < limit)
        {
            uint32_t curr_idx = active_offsets[j];
            uint32_t current_val = idx_begin[curr_idx];
            uint8_t b = bytes[curr_idx];
            while (b != j)
            {
                uint32_t dest_idx = active_offsets[b]++;
                std::swap(current_val, idx_begin[dest_idx]);
                std::swap(b, bytes[dest_idx]);
            }
            idx_begin[active_offsets[j]++] = current_val;
        }
    }

    if (offset < 64)
    {
        uint32_t current_pos = 0;
        for (int j = 255; j >= 1; --j)
        {
            if (counts[j] > 1)
            {

                RadSortIdxInternal(idx_begin + current_pos,
                                   idx_begin + current_pos + counts[j],
                                   base_data, bytes_pool + current_pos,
                                   offset + 1);
            }
            current_pos += counts[j];
        }
    }
}

template <typename T>
void RadSortIdxInternal_AVX2(uint32_t *idx_begin, uint32_t *idx_end,
                             T *base_data, uint8_t *bytes_pool, size_t offset)
{
restart:
    const size_t count = idx_end - idx_begin;
    if (count < 64)
    {
        for (uint32_t *i = idx_begin + 1; i < idx_end; ++i)
        {
            uint32_t val = *i;
            uint32_t *j = i;
            while (j > idx_begin &&
                   LexisReverseCompare(get_view(base_data[val]),
                                       get_view(base_data[*(j - 1)])))
            {
                *j = *(j - 1);
                --j;
            }
            *j = val;
        }
        return;
    }

    uint8_t *bytes = bytes_pool;

    alignas(64) uint32_t c0[256] = {0};
    alignas(64) uint32_t c1[256] = {0};
    alignas(64) uint32_t c2[256] = {0};
    alignas(64) uint32_t c3[256] = {0};

    size_t i = 0;

    for (; i + 7 < count; i += 8)
    {

        _mm_prefetch((const char *)&base_data[idx_begin[i + 8]], _MM_HINT_T0);
        _mm_prefetch((const char *)&base_data[idx_begin[i + 12]], _MM_HINT_T0);

        uint8_t b0 = get_byte(base_data[idx_begin[i]], offset);
        uint8_t b1 = get_byte(base_data[idx_begin[i + 1]], offset);
        uint8_t b2 = get_byte(base_data[idx_begin[i + 2]], offset);
        uint8_t b3 = get_byte(base_data[idx_begin[i + 3]], offset);
        uint8_t b4 = get_byte(base_data[idx_begin[i + 4]], offset);
        uint8_t b5 = get_byte(base_data[idx_begin[i + 5]], offset);
        uint8_t b6 = get_byte(base_data[idx_begin[i + 6]], offset);
        uint8_t b7 = get_byte(base_data[idx_begin[i + 7]], offset);

        bytes[i] = b0;
        c0[b0]++;
        bytes[i + 1] = b1;
        c1[b1]++;
        bytes[i + 2] = b2;
        c2[b2]++;
        bytes[i + 3] = b3;
        c3[b3]++;
        bytes[i + 4] = b4;
        c0[b4]++;
        bytes[i + 5] = b5;
        c1[b5]++;
        bytes[i + 6] = b6;
        c2[b6]++;
        bytes[i + 7] = b7;
        c3[b7]++;
    }
    for (; i < count; ++i)
    {
        uint8_t b = get_byte(base_data[idx_begin[i]], offset);
        bytes[i] = b;
        c0[b]++;
    }

    uint32_t counts[256];

#pragma clang loop vectorize(enable) interleave(enable)
    for (int j = 0; j < 256; ++j)
        counts[j] = c0[j] + c1[j] + c2[j] + c3[j];

    for (int j = 0; j < 256; ++j)
    {
        if (UNLIKELY(counts[j] == count))
        {
            if (offset < 63 && j != 0)
            {
                offset++;
                goto restart;
            }
            return;
        }
    }

    uint32_t offsets[256], pos = 0;

    for (int j = 255; j >= 0; --j)
    {
        offsets[j] = pos;
        pos += counts[j];
    }

    uint32_t active_offsets[256];
    __builtin_memcpy(active_offsets, offsets, sizeof(offsets));

    for (int j = 255; j >= 0; --j)
    {
        if (counts[j] == 0)
            continue;
        uint32_t limit = offsets[j] + counts[j];
        while (active_offsets[j] < limit)
        {
            uint32_t curr_idx = active_offsets[j];
            uint32_t current_val = idx_begin[curr_idx];
            uint8_t b = bytes[curr_idx];
            while (b != j)
            {
                uint32_t dest_idx = active_offsets[b]++;

                uint32_t tmp_v = idx_begin[dest_idx];
                idx_begin[dest_idx] = current_val;
                current_val = tmp_v;

                uint8_t tmp_b = bytes[dest_idx];
                bytes[dest_idx] = b;
                b = tmp_b;
            }
            idx_begin[active_offsets[j]++] = current_val;
        }
    }

    if (offset < 64)
    {
        uint32_t current_pos = 0;
        for (int j = 255; j >= 1; --j)
        {
            if (counts[j] > 1)
            {
                RadSortIdxInternal(idx_begin + current_pos,
                                   idx_begin + current_pos + counts[j],
                                   base_data, bytes_pool + current_pos,
                                   offset + 1);
            }
            current_pos += counts[j];
        }
    }
}

static thread_local String_lib::UniversalArena<8> RadixTempArena(1024 * 1024 *
                                                                 256);
static thread_local String_lib::UniversalArena<16>
    RadixGroupTempArena(1024 * 1024 * 256);

template <typename T_Comp, size_t FieldIdx, size_t Align,
          typename... GroupComps>
void RadixMSDStep(uint32_t *idx_begin, uint32_t *idx_end,
                  Group<Align, GroupComps...> &group, size_t offset,
                  uint8_t *chars_buffer)
{
    const size_t count = idx_end - idx_begin;
    if (count < 32)
    {
        auto *stream = group.template get_stream<T_Comp, FieldIdx>();
        for (uint32_t *i = idx_begin + 1; i < idx_end; ++i)
        {
            uint32_t val = *i;
            uint32_t *j = i;
            while (j > idx_begin &&
                   LexisReverseCompare(stream[val], stream[*(j - 1)]))
            {
                *j = *(j - 1);
                --j;
            }
            *j = val;
        }
        return;
    }

    auto *stream = group.template get_stream<T_Comp, FieldIdx>();
    uint32_t counts[256] = {0};

    for (size_t i = 0; i < count; ++i)
    {
        const auto &sv = stream[idx_begin[i]];
        uint8_t b = (offset < sv.GetLength()) ? (uint8_t)sv.c_str()[offset] : 0;
        counts[b]++;
    }

    uint32_t offsets[256], active_offsets[256];
    uint32_t pos = 0;
    for (int j = 255; j >= 0; --j)
    {
        offsets[j] = pos;
        active_offsets[j] = pos;
        pos += counts[j];
    }

    // 3. IN-PLACE Permutation (American Flag Sort logic)
    for (int j = 255; j >= 0; --j)
    {
        while (active_offsets[j] < offsets[j] + counts[j])
        {
            uint32_t cur_idx = idx_begin[active_offsets[j]];
            const auto &sv = stream[cur_idx];
            uint8_t b =
                (offset < sv.GetLength()) ? (uint8_t)sv.c_str()[offset] : 0;

            if (b == j)
            {
                active_offsets[j]++;
            }
            else
            {
                std::swap(idx_begin[active_offsets[j]],
                          idx_begin[active_offsets[b]++]);
            }
        }
    }

    uint32_t current_pos = 0;
    for (int j = 255; j >= 1; --j)
    {
        if (counts[j] > 1 && offset < 128)
        {
            RadixMSDStep<T_Comp, FieldIdx>(idx_begin + current_pos,
                                           idx_begin + current_pos + counts[j],
                                           group, offset + 1, chars_buffer);
        }
        current_pos += counts[j];
    }
}

template <typename T_Comp, size_t FieldIdx, size_t Align,
          typename... GroupComps>
void SortGroup(Group<Align, GroupComps...> &group, Registry<Align> &reg,
               String_lib::UniversalArena<Align> &temp_arena)
{
    const uint32_t count = group.size();
    if (count < 2)
        return;

    const size_t start_offset = temp_arena->current_offset();

    uint32_t *sorted_map =
        (uint32_t *)temp_arena->alloc(count * sizeof(uint32_t));
    for (uint32_t i = 0; i < count; ++i)
        sorted_map[i] = i;

    RadixMSDStep<T_Comp, FieldIdx>(sorted_map, sorted_map + count, group, 0,
                                   temp_arena);

    const uint32_t *entities = group.get_entity_ids();
    uint32_t *final_entities =
        (uint32_t *)temp_arena->alloc(count * sizeof(uint32_t));
    for (uint32_t i = 0; i < count; ++i)
    {
        final_entities[i] = entities[sorted_map[i]];
    }

    (
        [&]()
        {
            auto &set = reg.template get_set<GroupComps>();
            reorder_set_simd(set, final_entities, count, temp_arena);
        }(),
        ...);

    temp_arena->set_offset(start_offset);
}

template <typename T_Comp, size_t FieldIdx, size_t Align,
          typename... GroupComps>
void SortGroup(Group<Align, GroupComps...> &group, Registry<Align> &reg)
{
    const uint32_t count = group.size();
    if (UNLIKELY(count < 2))
        return;
    const size_t start_offset = RadixGroupTempArena.current_offset();

    auto *string_stream = group.template get_stream<T_Comp, FieldIdx>();
    const uint32_t *entities = group.get_entity_ids();

    uint32_t common_skip = 0;
    uint32_t min_len = 0xFFFFFFFF;

    for (uint32_t i = 0; i < count; ++i)
    {
        uint32_t l = string_stream[i].GetLength();
        if (l < min_len)
            min_len = l;
    }

    bool difference_found = false;
    while (common_skip + 64 <= min_len)
    {
        __m512i first_chunk =
            _mm512_loadu_si512(string_stream[0].c_str() + common_skip);
        uint64_t total_diff_mask = 0;

        for (uint32_t i = 1; i < count; ++i)
        {
            __m512i current_chunk =
                _mm512_loadu_si512(string_stream[i].c_str() + common_skip);
            total_diff_mask |=
                _mm512_cmpneq_epi8_mask(first_chunk, current_chunk);
            if (total_diff_mask)
                break;
        }

        if (total_diff_mask)
        {
            common_skip += __builtin_ctzll(total_diff_mask);
            difference_found = true;
            break;
        }
        common_skip += 64;
    }

    if (!difference_found && common_skip < min_len)
    {
        const char *first_ptr = string_stream[0].c_str();
        while (common_skip < min_len)
        {
            char c = first_ptr[common_skip];
            bool match = true;
            for (uint32_t i = 1; i < count; ++i)
            {
                if (string_stream[i].c_str()[common_skip] != c)
                {
                    match = false;
                    break;
                }
            }
            if (!match)
                break;
            common_skip++;
        }
    }

    struct Key
    {
        uint64_t head;
        uint32_t ent;
    };
    Key *src = (Key *)RadixGroupTempArena.alloc(count * sizeof(Key));
    Key *dst = (Key *)RadixGroupTempArena.alloc(count * sizeof(Key));

    for (uint32_t i = 0; i < count; ++i)
    {
        const auto &sv = string_stream[i];
        const char *p = sv.c_str() + common_skip;
        uint32_t rem = sv.GetLength() - common_skip;

        uint32_t load_cnt = (rem < 8) ? rem : 8;
        __mmask8 load_mask = (1U << load_cnt) - 1;

        uint64_t val = __builtin_bswap64(
            _mm_cvtsi128_si64(_mm_maskz_loadu_epi8(load_mask, p)));

        src[i] = {~val, i};
    }

    for (int shift = 0; shift < 64;)
    {
        uint32_t counts[2048] = {0}, offsets[2048];
        uint32_t mask = (shift > 50) ? 511 : 2047;
        for (uint32_t i = 0; i < count; ++i)
            counts[(src[i].head >> shift) & mask]++;
        offsets[0] = 0;
        for (int j = 1; j <= mask; ++j)
            offsets[j] = offsets[j - 1] + counts[j - 1];
        for (uint32_t i = 0; i < count; ++i)
            dst[offsets[(src[i].head >> shift) & mask]++] = src[i];
        std::swap(src, dst);
        shift += 11;
    }

    for (uint32_t i = 0; i < count;)
    {
        uint32_t j = i + 1;

        while (j < count && src[j].head == src[i].head)
            j++;

        uint32_t n = j - i;
        if (n > 1)
        {

            if (n <= 32)
            {
                for (uint32_t k = i + 1; k < j; ++k)
                {
                    Key pivot = src[k];
                    uint32_t m = k;

                    while (m > i &&
                           LexisReverseCompare(string_stream[pivot.ent],
                                               string_stream[src[m - 1].ent]))
                    {
                        src[m] = src[m - 1];
                        --m;
                    }
                    src[m] = pivot;
                }
            }
            else
            {

                std::sort(src + i, src + j,
                          [&](const Key &a, const Key &b)
                          {
                              return LexisReverseCompare(string_stream[a.ent],
                                                         string_stream[b.ent]);
                          });
            }
        }
        i = j;
    }

    uint32_t *final_map =
        (uint32_t *)RadixGroupTempArena.alloc(count * sizeof(uint32_t));

    uint32_t *local_indices =
        (uint32_t *)RadixGroupTempArena.alloc(count * sizeof(uint32_t));

    for (uint32_t i = 0; i < count; ++i)
    {
        uint32_t orig_idx = src[i].ent;
        final_map[i] = entities[orig_idx];
        local_indices[i] = orig_idx;
    }

    (
        [&]()
        {
            auto &set = reg.template get_set<GroupComps>();
            reorder_set_simd(set, final_map, local_indices, count,
                             RadixGroupTempArena);
        }(),
        ...);

    RadixGroupTempArena.set_offset(start_offset);
}

/*template <typename T_Comp, size_t FieldIdx, size_t Align, typename... Fields>
    void SortGroup(SparseSetSoA<Align, Fields...>& set, Registry<Align>& reg) {
    const uint32_t count = set.size();
    if (UNLIKELY(count < 2)) return;
    const size_t start_offset = RadixGroupTempArena.current_offset();

    auto* string_stream = set.template get_stream<FieldIdx>();
    const uint32_t* entities = set.get_dense_ptr();


    uint32_t common_skip = 0;
    uint32_t min_len = 0xFFFFFFFF;


    for (uint32_t i = 0; i < count; ++i) {
        uint32_t l = string_stream[i].GetLength();
        if (l < min_len) min_len = l;
    }


    bool difference_found = false;
    while (common_skip + 64 <= min_len) {
        __m512i first_chunk = _mm512_loadu_si512(string_stream[0].c_str() +
common_skip); uint64_t total_diff_mask = 0;

        for (uint32_t i = 1; i < count; ++i) {
            __m512i current_chunk = _mm512_loadu_si512(string_stream[i].c_str()
+ common_skip); total_diff_mask |= _mm512_cmpneq_epi8_mask(first_chunk,
current_chunk); if (total_diff_mask) break;
        }

        if (total_diff_mask) {
            common_skip += __builtin_ctzll(total_diff_mask);
            difference_found = true;
            break;
        }
        common_skip += 64;
    }


    if (!difference_found && common_skip < min_len) {
        const char* first_ptr = string_stream[0].c_str();
        while (common_skip < min_len) {
            char c = first_ptr[common_skip];
            bool match = true;
            for (uint32_t i = 1; i < count; ++i) {
                if (string_stream[i].c_str()[common_skip] != c) {
                    match = false;
                    break;
                }
            }
            if (!match) break;
            common_skip++;
        }
    }

    struct Key { uint64_t head; uint32_t ent; };
    Key* src = (Key*)RadixGroupTempArena.alloc(count * sizeof(Key));
    Key* dst = (Key*)RadixGroupTempArena.alloc(count * sizeof(Key));

    for (uint32_t i = 0; i < count; ++i) {
    const auto& sv = string_stream[i];
    const char* p = sv.c_str() + common_skip;
    uint32_t rem = sv.GetLength() - common_skip;

    uint32_t load_cnt = (rem < 8) ? rem : 8;
    __mmask8 load_mask = (1U << load_cnt) - 1;

    uint64_t val =
__builtin_bswap64(_mm_cvtsi128_si64(_mm_maskz_loadu_epi8(load_mask, p)));



    src[i] = { ~val, i };
}


    for (int shift = 0; shift < 64; ) {
        uint32_t counts[2048] = {0}, offsets[2048];
        uint32_t mask = (shift > 50) ? 511 : 2047;
        for (uint32_t i = 0; i < count; ++i) counts[(src[i].head >> shift) &
mask]++; offsets[0] = 0; for (int j = 1; j <= mask; ++j) offsets[j] =
offsets[j-1] + counts[j-1]; for (uint32_t i = 0; i < count; ++i)
dst[offsets[(src[i].head >> shift) & mask]++] = src[i]; std::swap(src, dst);
        shift += 11;
    }


for (uint32_t i = 0; i < count; ) {
    uint32_t j = i + 1;

    while (j < count && src[j].head == src[i].head) j++;

    uint32_t n = j - i;
    if (n > 1) {

        if (n <= 32) {
            for (uint32_t k = i + 1; k < j; ++k) {
                Key pivot = src[k];
                uint32_t m = k;



                while (m > i && LexisReverseCompare(string_stream[pivot.ent],
string_stream[src[m - 1].ent])) { src[m] = src[m - 1];
                    --m;
                }
                src[m] = pivot;
            }
        } else {


            std::sort(src + i, src + j, [&](const Key& a, const Key& b) {
                return LexisReverseCompare(string_stream[a.ent],
string_stream[b.ent]);
            });
        }
    }
    i = j;
}


uint32_t* final_map = (uint32_t*)RadixGroupTempArena.alloc(count *
sizeof(uint32_t));

uint32_t* local_indices = (uint32_t*)RadixGroupTempArena.alloc(count *
sizeof(uint32_t));

for (uint32_t i = 0; i < count; ++i) {
    uint32_t orig_idx = src[i].ent;
    final_map[i] = entities[orig_idx];
    local_indices[i] = orig_idx;
}

reorder_set_simd(set, final_map, local_indices, count, RadixGroupTempArena);

    RadixGroupTempArena.set_offset(start_offset);
}*/
template <typename T_Comp, size_t FieldIdx, size_t Align, typename Set>
void SortGroupNumeric(Set &set, Registry<Align> &reg,
                      String_lib::UniversalArena<Align> &temp_arena)
{
    const uint32_t count = set.size();
    if (count < 2)
        return;

    auto *sort_stream = set.template get_stream<FieldIdx>();

    struct Key
    {
        uint32_t head;
        uint32_t ent;
    };
    Key *src = (Key *)temp_arena.alloc(count * sizeof(Key));
    Key *dst = (Key *)temp_arena.alloc(count * sizeof(Key));

    for (uint32_t i = 0; i < count; ++i)
    {
        T_Comp val = sort_stream[i];
        uint32_t u_val;
        static_assert(sizeof(T_Comp) == 4,
                      "Only 4-byte types supported for now");
        memcpy(&u_val, &val, 4);

        src[i] = {~u_val, i};
    }

    for (int shift = 0; shift < 32; shift += 11)
    {
        uint32_t counts[2048] = {0}, offsets[2048];
        uint32_t mask = (shift > 22) ? 1023 : 2047;
        for (uint32_t i = 0; i < count; ++i)
            counts[(src[i].head >> shift) & mask]++;
        offsets[0] = 0;
        for (int j = 1; j <= mask; ++j)
            offsets[j] = offsets[j - 1] + counts[j - 1];
        for (uint32_t i = 0; i < count; ++i)
            dst[offsets[(src[i].head >> shift) & mask]++] = src[i];
        std::swap(src, dst);
    }

    uint32_t *final_map =
        (uint32_t *)temp_arena.alloc(count * sizeof(uint32_t));
    uint32_t *local_indices =
        (uint32_t *)temp_arena.alloc(count * sizeof(uint32_t));
    uint32_t *original_entities = set.get_dense_ptr();

    for (uint32_t i = 0; i < count; ++i)
    {
        uint32_t orig_idx = src[i].ent;
        final_map[i] = original_entities[orig_idx];
        local_indices[i] = orig_idx;
    }

    reorder_set_simd(set, final_map, local_indices, count, temp_arena);
}

// not use!
template <typename Set, size_t Align>
void reorder_sparse_set(Set &set, uint32_t *sorted_entities, uint32_t count,
                        String_lib::UniversalArena<Align> &arena)
{

    for (uint32_t i = 0; i < count; ++i)
    {
        set.swap_entities(sorted_entities[i], set.get_entity_at(i));
    }
}

template <typename T> void RadSortRecursive(T *begin, T *end, size_t offset)
{
    const size_t count = end - begin;

    size_t start_offset = RadixTempArena.current_offset();

    uint32_t *indices =
        static_cast<uint32_t *>(RadixTempArena.alloc(count * sizeof(uint32_t)));
#pragma clang loop vectorize(enable) interleave(enable)
    for (uint32_t i = 0; i < count; ++i)
        indices[i] = i;

    uint8_t *bytes_pool =
        static_cast<uint8_t *>(RadixTempArena.alloc(count * sizeof(uint8_t)));

    RadSortIdxInternal(indices, indices + count, begin, bytes_pool, offset);

    T *temp = static_cast<T *>(RadixTempArena.alloc(count * sizeof(T)));

    for (size_t i = 0; i < count; ++i)
    {
        if (LIKELY(i + 32 < count))
        {
            __builtin_prefetch(&begin[indices[i + 32]], 0, 3);

            __builtin_prefetch(&indices[i + 64], 0, 0);
        }

        if constexpr (std::is_trivially_copyable_v<T>)
        {
            memcpy(&temp[i], &begin[indices[i]], sizeof(T));
        }
        else
        {
            new (&temp[i]) T(std::move(begin[indices[i]]));
        }
    }

    if constexpr (std::is_trivially_copyable_v<T>)
    {
        memcpy(begin, temp, count * sizeof(T));
    }
    else
    {
        for (size_t i = 0; i < count; ++i)
        {
            begin[i] = std::move(temp[i]);
            temp[i].~T();
        }
    }

    RadixTempArena.set_offset(start_offset);
}
/*

ALWAYS_INLINE uint32_t get_length(const SoftTicket& t) {
    return static_cast<uint32_t>(t.view.GetLength());
}

ALWAYS_INLINE const char* get_c_str(const SoftTicket& t) {
    return t.view.c_str();
}*/

template <typename T>
void RadSortRecursiveSkipPrefix(T *begin, T *end, size_t offset)
{
    const size_t count = end - begin;

    size_t start_offset = RadixTempArena.current_offset();

    size_t common_skip = offset;
    uint32_t min_len = 0xFFFFFFFF;

    for (size_t i = 0; i < count; ++i)
    {
        uint32_t l = get_length(begin[i]);
        if (l < min_len)
            min_len = l;
    }

    if (min_len > offset)
    {
        bool diff_found = false;
        const char *first_ptr = get_c_str(begin[0]);

        while (common_skip + 64 <= min_len)
        {
            __m512i base = _mm512_loadu_si512(first_ptr + common_skip);
            uint64_t total_mask = 0;
            for (size_t i = 1; i < count && i < 128; ++i)
            {
                __m512i curr =
                    _mm512_loadu_si512(get_c_str(begin[i]) + common_skip);
                total_mask |= _mm512_cmpneq_epi8_mask(base, curr);
                if (UNLIKELY(total_mask))
                    break;
            }
            if (UNLIKELY(total_mask))
            {
                common_skip += __builtin_ctzll(total_mask);
                diff_found = true;
                break;
            }
            common_skip += 64;
        }

        if (!diff_found)
        {
            while (common_skip < min_len)
            {
                char c = first_ptr[common_skip];
                bool match = true;
                for (size_t i = 1; i < count; ++i)
                {
                    if (get_c_str(begin[i])[common_skip] != c)
                    {
                        match = false;
                        break;
                    }
                }
                if (!match)
                    break;
                common_skip++;
            }
        }
    }

    uint32_t *indices =
        static_cast<uint32_t *>(RadixTempArena.alloc(count * sizeof(uint32_t)));
#pragma clang loop vectorize(enable) interleave(enable)
    for (uint32_t i = 0; i < count; ++i)
        indices[i] = i;

    uint8_t *bytes_pool =
        static_cast<uint8_t *>(RadixTempArena.alloc(count * sizeof(uint8_t)));

    RadSortIdxInternal(indices, indices + count, begin, bytes_pool,
                       common_skip);

    T *temp = static_cast<T *>(RadixTempArena.alloc(count * sizeof(T)));

    for (size_t i = 0; i < count; ++i)
    {
        if (LIKELY(i + 64 < count))
        {
            __builtin_prefetch(&begin[indices[i + 64]], 0, 3);

            __builtin_prefetch(&indices[i + 128], 0, 0);
        }

        if constexpr (std::is_trivially_copyable_v<T>)
        {
            memcpy(&temp[i], &begin[indices[i]], sizeof(T));
        }
        else
        {
            new (&temp[i]) T(std::move(begin[indices[i]]));
        }
    }

    if constexpr (std::is_trivially_copyable_v<T>)
    {
        memcpy(begin, temp, count * sizeof(T));
    }
    else
    {
        for (size_t i = 0; i < count; ++i)
        {
            begin[i] = std::move(temp[i]);
            temp[i].~T();
        }
    }

    RadixTempArena.set_offset(start_offset);
}

ALWAYS_INLINE uint32_t get_diff_mask_avx2(const char *p1, const char *p2)
{
    __m256i v1 = _mm256_loadu_si256((const __m256i *)p1);
    __m256i v2 = _mm256_loadu_si256((const __m256i *)p2);
    __m256i cmp = _mm256_cmpeq_epi8(v1, v2);

    return (uint32_t)~_mm256_movemask_epi8(cmp);
}

template <typename T>
void RadSortRecursiveSkipPrefix_AVX2(T *begin, T *end, size_t offset)
{
    const size_t count = end - begin;
    size_t start_offset = RadixTempArena.current_offset();
    size_t common_skip = offset;
    uint32_t min_len = 0xFFFFFFFF;

    for (size_t i = 0; i < count; ++i)
    {
        uint32_t l = get_length(begin[i]);
        if (l < min_len)
            min_len = l;
    }

    if (min_len > offset)
    {
        const char *first_ptr = get_c_str(begin[0]);
        bool diff_found = false;

        // --- AVX2 Optimized Skip ---
        while (common_skip + 32 <= min_len)
        {
            uint32_t total_mask = 0;
            const char *base_addr = first_ptr + common_skip;

            for (size_t i = 1; i < count && i < 128; ++i)
            {
                total_mask |= get_diff_mask_avx2(
                    base_addr, get_c_str(begin[i]) + common_skip);
                if (UNLIKELY(total_mask))
                    break;
            }

            if (UNLIKELY(total_mask))
            {

                common_skip += __builtin_ctz(total_mask);
                diff_found = true;
                break;
            }
            common_skip += 32;
        }

        if (!diff_found)
        {
            while (common_skip < min_len)
            {
                char c = first_ptr[common_skip];
                bool match = true;
                for (size_t i = 1; i < count; ++i)
                {
                    if (get_c_str(begin[i])[common_skip] != c)
                    {
                        match = false;
                        break;
                    }
                }
                if (!match)
                    break;
                common_skip++;
            }
        }
    }

    uint32_t *indices =
        static_cast<uint32_t *>(RadixTempArena.alloc(count * sizeof(uint32_t)));
#pragma clang loop vectorize(enable) interleave(enable)
    for (uint32_t i = 0; i < count; ++i)
        indices[i] = i;

    uint8_t *bytes_pool =
        static_cast<uint8_t *>(RadixTempArena.alloc(count * sizeof(uint8_t)));

    RadSortIdxInternal_AVX2(indices, indices + count, begin, bytes_pool,
                            common_skip);

    T *temp = static_cast<T *>(RadixTempArena.alloc(count * sizeof(T)));

    for (size_t i = 0; i < count; ++i)
    {
        if (LIKELY(i + 64 < count))
        {
            __builtin_prefetch(&begin[indices[i + 64]], 0, 3);

            __builtin_prefetch(&indices[i + 128], 0, 0);
        }

        if constexpr (std::is_trivially_copyable_v<T>)
        {
            memcpy(&temp[i], &begin[indices[i]], sizeof(T));
        }
        else
        {
            new (&temp[i]) T(std::move(begin[indices[i]]));
        }
    }

    if constexpr (std::is_trivially_copyable_v<T>)
    {
        memcpy(begin, temp, count * sizeof(T));
    }
    else
    {
        for (size_t i = 0; i < count; ++i)
        {
            begin[i] = std::move(temp[i]);
            temp[i].~T();
        }
    }

    RadixTempArena.set_offset(start_offset);
}

// interface
template <typename T> void RadSort(T *begin, T *end, size_t offset)
{
    if (UNLIKELY(end - begin < 2))
        return;
    RadSortRecursiveSkipPrefix_AVX2(begin, end, offset);
}

template <typename T_Comp, size_t FieldIdx, typename... GroupComps,
          size_t Align>
void RadSortGroup(Group<Align, GroupComps...> &group, Registry<Align> &reg)
{

    const size_t start_offset = RadixTempArena.current_offset();

    SortGroup<T_Comp, FieldIdx>(group, reg, RadixTempArena);

    RadixTempArena.set_offset(start_offset);
}
} // namespace Radix_Internal

ALWAYS_INLINE inline int CompareKeys(const String_lib::StringView &lhs,
                                     const String_lib::StringView &rhs)
{
    size_t len1 = lhs.GetLength();
    size_t len2 = rhs.GetLength();
    size_t min_len = (len1 < len2) ? len1 : len2;

    if (LIKELY(min_len > 0))
    {

        int res = String_lib::c_strcmp_unaligned_avx512(lhs.c_str(),
                                                        rhs.c_str(), min_len);
        if (res != 0)
            return res;
    }

    return (len1 < len2) ? -1 : (len1 > len2 ? 1 : 0);
}

struct MergeNode
{
    String_lib::StringView key;
    String_lib::StringView value;
    MmapReader *reader;
    int chunk_id;

    MergeNode(String_lib::StringView k, String_lib::StringView v, MmapReader *r,
              int id)
        : key(k), value(v), reader(r), chunk_id(id)
    {
    }

    MergeNode(MergeNode &&other) noexcept = default;
    MergeNode &operator=(MergeNode &&other) noexcept = default;
    MergeNode(const MergeNode &) = default;
    MergeNode &operator=(const MergeNode &) = default;

    bool operator<(const MergeNode &other) const
    {
        int res = CompareKeys(this->key, other.key);

        if (res == 0)
        {

            return this->chunk_id < other.chunk_id;
        }

        return res < 0;
    }
};

struct IndexEntry
{
    String_lib::StringView key;
    size_t offset;
};

struct DiskState
{
    std::shared_ptr<MmapReader> reader;
    std::vector<IndexEntry> index;
};

/*struct alignas(64)ShardStorage {

    std::unique_ptr<MmapArena<8>> arenas[2];


    ankerl::unordered_dense::map<String_lib::StringView, String_lib::StringView,
StringHasher, StringEq> dbs[2];

    int active_idx = 0;
    int chunk_id = 0;
    int thread_id;
    std::vector<String_lib::String> temp_files;
    std::shared_ptr<DiskState> disk_data;

    ShardStorage(int tid) : thread_id(tid) {
        char name0[64], name1[64];
        sprintf(name0, "thread_%d_arena_0.mmap", tid);
        sprintf(name1, "thread_%d_arena_1.mmap", tid);


        arenas[0] = std::make_unique<MmapArena<8>>(128 * 1024 * 1024, name0);
        arenas[1] = std::make_unique<MmapArena<8>>(128 * 1024 * 1024, name1);
    }


    void Close() {
        for (int i = 0; i < 2; ++i) {

            dbs[i].clear();

            if (arenas[i]) {
                arenas[i].reset();
            }
        }

        disk_data.reset();
        temp_files.clear();
    }


    ~ShardStorage() {
        Close();
    }
};*/

struct alignas(64) ShardStorage
{

    int active_idx = 0;
    int chunk_id = 0;
    int thread_id;

    char _pad0[64 - (sizeof(int) * 3)];

    // --- DATA PATH ---
    alignas(64) std::unique_ptr<MmapArena<8>> arenas[2];

    alignas(64) ankerl::unordered_dense::map<String_lib::StringView,
                                             String_lib::StringView,
                                             StringHasher, StringEq> dbs[2];

    alignas(64) std::vector<String_lib::String> temp_files;
    std::shared_ptr<DiskState> disk_data;

    ShardStorage(int tid) : thread_id(tid)
    {
        char name0[64], name1[64];
        sprintf(name0, "thread_%d_arena_0.mmap", tid);
        sprintf(name1, "thread_%d_arena_1.mmap", tid);

        arenas[0] = std::make_unique<MmapArena<8>>(128 * 1024 * 1024, name0);
        arenas[1] = std::make_unique<MmapArena<8>>(128 * 1024 * 1024, name1);
    }

    void Close()
    {
        for (int i = 0; i < 2; ++i)
        {

            dbs[i].clear();

            if (arenas[i])
            {
                arenas[i].reset();
            }
        }

        disk_data.reset();
        temp_files.clear();
    }

    ~ShardStorage()
    {
        Close();
    }
};

inline void LoadLocalIndexToState(const char *idx_path,
                                  std::shared_ptr<DiskState> &state)
{
    MmapReader idx_reader(idx_path);
    if (!idx_reader.m_data)
        return;

    const char *base = idx_reader.m_data;
    size_t &off = const_cast<size_t &>(idx_reader.m_offset);

    while (off + 12 <= idx_reader.m_size)
    { // 4 (k_len) + 8 (offset)
        uint32_t k_len;
        __builtin_memcpy(&k_len, base + off, 4);
        off += 4;

        size_t main_off;
        __builtin_memcpy(&main_off, base + off, 8);
        off += 8;

        String_lib::StringView key_view(state->reader->m_data + main_off + 8,
                                        k_len);
        state->index.push_back({key_view, main_off});

        off += k_len;

        off = (off + 7) & ~7ULL;
    }
}
// TODO: CHANGE IN OTHER TOO!!!!!!!!!!!!!!!!!!!!!!
inline void FastExternalMerge(std::vector<String_lib::String> &temp_files,
                              int shard_id, ShardStorage &storage)
{
    if (UNLIKELY(temp_files.empty()))
        return;
    // printf("[MERGE_START] Shard %d | Files to merge: %zu + Disk\n", shard_id,
    // temp_files.size());

    char rdb_name[64], idx_name[64];
    sprintf(rdb_name, "shard_%d.rdb", shard_id);
    sprintf(idx_name, "shard_%d.idx", shard_id);
    char temp_rdb_name[64], temp_idx_name[64];
    sprintf(temp_rdb_name, "temp_shard_%d.rdb", shard_id);
    sprintf(temp_idx_name, "temp_shard_%d.idx", shard_id);

    /*if (storage.disk_data) {
       printf("  -> Base Disk: %s\n", rdb_name);
   }
   for (size_t i = 0; i < temp_files.size(); ++i) {
       printf("  -> Chunk [%zu]: %s\n", i, temp_files[i].c_str());
   }*/

    size_t total_max_size = 0;
    for (auto &tf : temp_files)
        total_max_size += GetFileSize(tf.c_str());
    if (storage.disk_data)
        total_max_size += GetFileSize(rdb_name);

    {
        std::vector<std::unique_ptr<MmapReader>> readers;
        std::priority_queue<MergeNode> pq;

        auto current_disk = storage.disk_data;
        if (current_disk && current_disk->reader)
        {
            String_lib::StringView k, v;
            const_cast<size_t &>(current_disk->reader->m_offset) = 0;
            if (current_disk->reader->next_pair(k, v))
            {
                pq.push(MergeNode(k, v, current_disk->reader.get(), -1));
            }
        }

        for (int i = 0; i < (int)temp_files.size(); ++i)
        {
            auto reader = std::make_unique<MmapReader>(temp_files[i].c_str());
            String_lib::StringView k, v;
            if (reader->next_pair(k, v))
            {

                pq.push(MergeNode(k, v, reader.get(), i));
            }
            readers.push_back(std::move(reader));
        }

        MmapArena<8> outRdb(total_max_size, temp_rdb_name);
        uint8_t *write_ptr = outRdb.data();
        uint8_t *const base_ptr = write_ptr;

        if (UNLIKELY(!write_ptr))
        {
            printf("[FATAL] FastExternalMerge: mmap failed! Size: %zu, File: "
                   "%s, Error: %lu\n",
                   total_max_size, temp_rdb_name, GetLastError());
            return;
        }

        MmapArena<8> outIdx(total_max_size / 10, temp_idx_name);
        uint8_t *idx_ptr = outIdx.data();
        uint8_t *const idx_base = idx_ptr;
        std::vector<char> idx_buf(128 * 1024);

        String_lib::StringView last_written_key;
        bool first = true;
        size_t record_count = 0;

        while (!pq.empty())
        {
            MergeNode current = pq.top();
            pq.pop();

            while (!pq.empty() && pq.top().key == current.key)
            {
                MergeNode dup = pq.top();
                pq.pop();

                String_lib::StringView next_k, next_v;
                if (dup.reader->next_pair(next_k, next_v))
                {
                    pq.push(
                        MergeNode(next_k, next_v, dup.reader, dup.chunk_id));
                }
            }

            if (current.value.c_str() != nullptr)
            {
                uint32_t k_len = current.key.GetLength();
                uint32_t v_len = current.value.GetLength();
                size_t current_offset = (size_t)(write_ptr - base_ptr);

                if (UNLIKELY(record_count % 1024 == 0))
                {

                    *(uint32_t *)idx_ptr = k_len;
                    idx_ptr += 4;
                    *(size_t *)idx_ptr = current_offset;
                    idx_ptr += 8;
                    __builtin_memcpy(idx_ptr, current.key.c_str(), k_len);
                    idx_ptr += k_len;
                    // TODO: TEST
                    idx_ptr = (uint8_t *)(((uintptr_t)idx_ptr + 7) & ~7);
                }

                uint64_t combined_lens = ((uint64_t)v_len << 32) | k_len;

                __builtin_memcpy(write_ptr, &combined_lens, 8);
                __builtin_memcpy(write_ptr + 8, current.key.c_str(), k_len);

                uint32_t actual_v_len = 0;
                if (v_len > 0 && v_len != 0xFFFFFFFF)
                {
                    __builtin_memcpy(write_ptr + 8 + k_len,
                                     current.value.c_str(), v_len);
                    actual_v_len = v_len;
                }

                write_ptr += (8 + k_len + actual_v_len);

                write_ptr = (uint8_t *)(((uintptr_t)write_ptr + 7) & ~7);

                ++record_count;
            }

            String_lib::StringView next_k, next_v;
            if (current.reader->next_pair(next_k, next_v))
            {
                pq.push(MergeNode(next_k, next_v, current.reader,
                                  current.chunk_id));
            }
        }
        printf("[MERGE_FFINISH] Shard %d | Total Records Written: %zu\n",
               shard_id, record_count);
        outIdx.truncate_to((size_t)(idx_ptr - idx_base));

        size_t final_size = (size_t)(write_ptr - base_ptr);
        // printf("[MERGE_PRE_TRUNCATE] TID: %lu | Shard: %d | FinalSize: %zu |
        // BasePtr: %p | WritePtr: %p\n", GetCurrentThreadId(), shard_id,
        // final_size, (void*)base_ptr, (void*)write_ptr);
        outRdb.truncate_to(final_size);
        // printf("[MERGE_POST_TRUNCATE] TID: %lu | Shard: %d\n",
        // GetCurrentThreadId(), shard_id);
    }

    auto next_state = std::make_shared<DiskState>();
    /// printf("[DEBUG_MERGE] About to reset disk_data. PQ size: %zu\n",
    /// pq.size());
    // storage.disk_data.reset();

    if (GetFileAttributesA(rdb_name) != INVALID_FILE_ATTRIBUTES)
    {
        std::remove(rdb_name);
        std::remove(idx_name);
    }
    std::rename(temp_rdb_name, rdb_name);
    std::rename(temp_idx_name, idx_name);

    next_state->reader = std::make_shared<MmapReader>(rdb_name);
    LoadLocalIndexToState(idx_name, next_state);
    std::atomic_store(&storage.disk_data, next_state);

    for (auto &tf : temp_files)
        std::remove(tf.c_str());
}

inline void SnapshotShard(
    ankerl::unordered_dense::map<String_lib::StringView, String_lib::StringView,
                                 StringHasher, StringEq> &db,
    int shard_id, int chunk_id, std::vector<String_lib::String> &temp_files)
{

    char name_buff[64];
    sprintf(name_buff, "shard_%d_chunk_%d.bin", shard_id, chunk_id);

    static thread_local std::vector<
        std::pair<String_lib::StringView, String_lib::StringView>>
        kv_pairs;
    kv_pairs.clear();
    kv_pairs.reserve(db.size());

    size_t total_required_size = 0;

    for (auto &it : db)
    {
        kv_pairs.push_back({it.first, it.second});
        uint32_t k_l = it.first.GetLength();
        uint32_t v_l =
            (it.second.c_str() == nullptr) ? 0 : it.second.GetLength();

        size_t record_size = 8 + k_l + v_l;

        total_required_size += (record_size + 7) & ~7ULL;
    }
    // printf("DEBUG: Shard %d, Size %zu, Total Required: %zu\n", shard_id,
    // db.size(), total_required_size);

    // std::sort(kv_pairs.begin(), kv_pairs.end(), [](const auto& a, const auto&
    // b) {
    //     return LexisReverseCompare(a.first, b.first);
    // });
    if (kv_pairs.empty())
        return;

    Radix_Internal::RadSort(kv_pairs.data(), kv_pairs.data() + kv_pairs.size(),
                            0);

    if (UNLIKELY(total_required_size == 0))
    {
        printf("[CRITICAL] SnapshotShard: total_required_size is 0! db.size: "
               "%zu\n",
               db.size());
    }
    MmapArena<8> output_chunk(total_required_size, name_buff);
    uint8_t *write_ptr = output_chunk.data();
    uint8_t *const base_ptr = write_ptr;
    // printf("START WRITE: Shard %d, write_ptr initial: %p\n", shard_id,
    // write_ptr);
    if (UNLIKELY(!write_ptr))
    {
        printf("[FATAL] Mmap failed in Snapshot %d ! Size: %zu | File: %s | "
               "GetLastError: %lu\n",
               shard_id, total_required_size, name_buff, GetLastError());
        return;
    }

    for (const auto &kv : kv_pairs)
    {
        uint32_t k_len = (uint32_t)kv.first.GetLength();
        uint32_t v_len = (kv.second.c_str() == nullptr)
                             ? 0xFFFFFFFF
                             : (uint32_t)kv.second.GetLength();

        uint64_t combined_lens = ((uint64_t)v_len << 32) | k_len;
        __builtin_memcpy(write_ptr, &combined_lens, 8);

        __builtin_memcpy(write_ptr + 8, kv.first.c_str(), k_len);

        uint32_t actual_v_len = 0;
        if (v_len != 0xFFFFFFFF && v_len > 0)
        {
            __builtin_memcpy(write_ptr + 8 + k_len, kv.second.c_str(), v_len);
            actual_v_len = v_len;
        }

        write_ptr += (8 + k_len + actual_v_len);
        write_ptr = (uint8_t *)(((uintptr_t)write_ptr + 7) & ~7ULL);
    }

    size_t actual_written = (size_t)(write_ptr - base_ptr);
    if (UNLIKELY(actual_written != total_required_size))
    {
        printf("[CRITICAL] Size mismatch! Calculated: %zu, Actual: %zu\n",
               total_required_size, actual_written);

        output_chunk.truncate_to(actual_written);
    }
    // TODO: matbe not
    output_chunk.truncate_to(actual_written);
    // TODO: Uncomment if need more stable
    // if (output_chunk.data()) {
    //    FlushViewOfFile(output_chunk.data(), total_required_size);
    //}
    // printf("[WRITE_FINISHED] TID: %lu | Shard: %d | Range: %p - %p\n",
    // GetCurrentThreadId(), shard_id, (void*)output_chunk.data(),
    // (void*)write_ptr);
    temp_files.push_back(std::move(name_buff));
}

struct DumpIndex
{
    struct Entry
    {
        String_lib::StringView key;
        size_t offset;
    };
    std::vector<Entry> sparse_offsets;

    void build(MmapReader &reader)
    {
        reader.m_offset = 0;
        String_lib::StringView k, v;
        size_t count = 0;
        while (reader.next_pair(k, v))
        {
            if (count % 1024 == 0)
            {
                sparse_offsets.push_back(
                    {k, reader.m_offset - (k.GetLength() + v.GetLength() + 8)});
            }
            count++;
        }
    }
};

inline String_lib::StringView FindInDumpIndexed(std::shared_ptr<DiskState> snap,
                                                String_lib::StringView key)
{

    if (!snap || snap->index.empty())
        return {};

    auto it = std::lower_bound(snap->index.begin(), snap->index.end(), key,
                               [](const IndexEntry &e, String_lib::StringView k)
                               { return LexisReverseCompare(e.key, k); });

    if (it != snap->index.begin())
    {
        --it;
    }

    const char *base_ptr = snap->reader->m_data;
    size_t current_off = it->offset;
    size_t total_size = snap->reader->m_size;

    while (current_off + 8 <= total_size)
    {

        uint64_t header;
        __builtin_memcpy(&header, base_ptr + current_off, 8);

        uint32_t k_len = (uint32_t)(header & 0xFFFFFFFF);
        uint32_t v_len = (uint32_t)(header >> 32);

        String_lib::StringView k(base_ptr + current_off + 8, k_len);

        if (k == key)
        {
            if (UNLIKELY(v_len == 0xFFFFFFFF))
                return {}; // Tombstone
            return String_lib::StringView(base_ptr + current_off + 8 + k_len,
                                          v_len);
        }

        if (!LexisReverseCompare(k, key))
            break;

        uint32_t actual_v_len = (v_len == 0xFFFFFFFF) ? 0 : v_len;
        current_off += (8 + k_len + actual_v_len);

        current_off = (current_off + 7) & ~7ULL;
    }
    return {};
}

void EnsureArenaSpace(ShardStorage &storage, size_t needed, int thread_id)
{
    auto &arena = storage.arenas[storage.active_idx];
    if (UNLIKELY(arena->current_offset() + needed + 128 > 128 * 1024 * 1024))
    {
        int old_idx = storage.active_idx;
        storage.active_idx = 1 - storage.active_idx;

        SnapshotShard(storage.dbs[old_idx], thread_id, storage.chunk_id++,
                      storage.temp_files);

        if (storage.temp_files.size() >= 2)
        {
            FastExternalMerge(storage.temp_files, thread_id, storage);
            storage.temp_files.clear();
        }
        storage.dbs[old_idx].clear();
        storage.arenas[old_idx]->reset();
    }
}

inline void ProcessRequest(ConnectionContext *ctx, const ParseResult &pres,
                           ShardStorage &storage, int thread_id,
                           int totalworkers)
{

    static thread_local uint64_t enter_count = 0;
    enter_count++;

    if (pres.type == CommandType::SET)
    {

        auto &arena = storage.arenas[storage.active_idx];

        auto &db = storage.dbs[storage.active_idx];

        auto *current_arena_ptr = &storage.arenas[storage.active_idx];

        // TODO: ARENA SIZE SYNC!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
        if (UNLIKELY(arena->current_offset() + pres.key_len + pres.val_len +
                         128 >
                     128 * 1024 * 1024))
        {
            int old_idx = storage.active_idx;
            storage.active_idx = 1 - storage.active_idx;

            SnapshotShard(storage.dbs[old_idx], thread_id, storage.chunk_id++,
                          storage.temp_files);
            // TODO: MAKE THIS VARIABLE AND ASSIGN TO SHARD ARENA COUNT
            if (storage.temp_files.size() >= 2)
            {

                FastExternalMerge(storage.temp_files, thread_id, storage);
                storage.temp_files.clear();
            }

            storage.dbs[old_idx].clear();
            storage.arenas[old_idx]->reset();
        }

        char *k_mem = (char *)arena->alloc(pres.key_len);
        char *v_mem = (char *)arena->alloc(pres.val_len);

        __builtin_memcpy(k_mem, pres.key_ptr, pres.key_len);
        __builtin_memcpy(v_mem, pres.val_ptr, pres.val_len);

        db[String_lib::StringView(k_mem, pres.key_len)] =
            String_lib::StringView(v_mem, pres.val_len);

        /*local_db[std::move(k)] = std::move(v);*/
        g_stats.total_ops.fetch_add(1, std::memory_order_release);

        char *dest = ctx->responseBuf + ctx->responseLen;

        __builtin_memcpy(dest, "+OK\r\n", 5);

        ctx->responseLen += 5;
    }
    else if (pres.type == CommandType::GET)
    {
        String_lib::StringView key_view(pres.key_ptr, pres.key_len);
        String_lib::StringView val_view;
        bool found = false;
        bool tombstone = false;

        auto &active_map = storage.dbs[storage.active_idx];
        auto it = active_map.find(key_view);
        if (it != active_map.end())
        {
            if (it->second.c_str() != nullptr)
            {
                val_view = it->second;
                found = true;
            }
            else
            {
                tombstone = true;
            }
        }

        if (!found && !tombstone)
        {
            auto &secondary_map = storage.dbs[1 - storage.active_idx];
            auto it2 = secondary_map.find(key_view);
            if (it2 != secondary_map.end())
            {
                if (it2->second.c_str() != nullptr)
                {
                    val_view = it2->second;
                    found = true;
                }
                else
                {
                    tombstone = true;
                }
            }
        }

        if (!found)
        {

            auto snap = std::atomic_load(&storage.disk_data);

            if (LIKELY(snap && !snap->index.empty()))
            {

                val_view = FindInDumpIndexed(snap, key_view);

                if (val_view.c_str() != nullptr)
                {
                    found = true;
                }
            }
        }

        char *current_response_ptr = ctx->responseBuf + ctx->responseLen;

        if (found)
        {
            // const auto& val = val_view;
            size_t v_len = val_view.GetLength();

            int head_len =
                RESP_Utils::write_len_bulk(current_response_ptr, v_len);

            // if (ctx->responseLen + head_len + v_len + 2 > 4096) return res;

            if (v_len <= 8 && v_len > 0)
            {

                __builtin_memcpy(current_response_ptr + head_len,
                                 val_view.c_str(), 8);
            }
            else if (v_len > 0)
            {
                __builtin_memcpy(current_response_ptr + head_len,
                                 val_view.c_str(), v_len);
            }
            char *tail = current_response_ptr + head_len + v_len;
            tail[0] = '\r';
            tail[1] = '\n';

            ctx->responseLen += (uint32_t)(head_len + v_len + 2);
        }
        else
        {
            // if (c->responseLen + 5 > 4096) return res;
            __builtin_memcpy(current_response_ptr, "$-1\r\n", 5);
            ctx->responseLen += 5;
        }

        g_stats.total_ops.fetch_add(1, std::memory_order_release);
    }
    else if (pres.type == CommandType::WKS)
    {
        char *dest = ctx->responseBuf + ctx->responseLen;
        int val = totalworkers;

        *dest = ':';

        if (UNLIKELY(val < 10))
        {
            dest[1] = (char)('0' + val);
            dest[2] = '\r';
            dest[3] = '\n';
            ctx->responseLen += 4; // ":8\r\n"
        }
        else if (LIKELY(val < 100))
        {
            const char *pair = &RESP_Utils::digits100[val * 2];
            dest[1] = pair[0];
            dest[2] = pair[1];
            dest[3] = '\r';
            dest[4] = '\n';
            ctx->responseLen += 5; // ":16\r\n"
        }
        else
        {

            int written = sprintf(dest + 1, "%d\r\n", val);
            ctx->responseLen += (uint32_t)(1 + written);
        }

        g_stats.total_ops.fetch_add(1, std::memory_order_release);
    }
    else if (pres.type == CommandType::DEL)
    {
        EnsureArenaSpace(storage, pres.key_len, thread_id);

        auto &arena = storage.arenas[storage.active_idx];
        auto &db = storage.dbs[storage.active_idx];

        char *k_mem = (char *)arena->alloc(pres.key_len);
        if (!k_mem)
        {
            printf("ARENA OUT OF MEMORY! Shard: %d\n", storage.active_idx);
            return;
        }
        __builtin_memcpy(k_mem, pres.key_ptr, pres.key_len);

        db[String_lib::StringView(k_mem, pres.key_len)] =
            String_lib::StringView(nullptr, 0);

        char *current_response_ptr = ctx->responseBuf + ctx->responseLen;
        __builtin_memcpy(current_response_ptr, ":1\r\n", 4);
        ctx->responseLen += 4;

        g_stats.total_ops.fetch_add(1, std::memory_order_release);
    }
}

ALWAYS_INLINE long fast_resp_atoi(const char *p, const char *eol)
{
    long val = 0;

    while (p < eol)
    {

        val = (val << 3) + (val << 1) + (*p++ - '0');
    }
    return val;
}
// DO NOT USE!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
ALWAYS_INLINE uint32_t fast_atoi_avx512(const char *p, size_t len)
{

    uint64_t mask = _bzhi_u64(-1ULL, len);
    __m128i v = _mm_maskz_loadu_epi8(mask, p);

    // 2. ASCII -> Numbers
    v = _mm_sub_epi8(v, _mm_set1_epi8('0'));

    const __m128i mult =
        _mm_setr_epi8(0, 0, 0, 0, 0, 0, 10, 1, 10, 1, 10, 1, 10, 1, 10, 1);

    __m128i res16 = _mm_maddubs_epi16(v, mult);

    __m128i res32 =
        _mm_madd_epi16(res16, _mm_setr_epi16(10000, 100, 1, 0, 0, 0, 0, 0));

    uint32_t parts[4];
    _mm_storeu_si128((__m128i *)parts, res32);

    if (len == 1)
        return (uint32_t)((uint8_t *)parts)[0];
    if (len == 2)
        return (uint32_t)((uint8_t *)parts)[0] * 10 + ((uint8_t *)parts)[1];

    return parts[0] + parts[1] + parts[2] + parts[3];
}

void worker_loop(const std::vector<HANDLE> &hIocpList, HANDLE hIocp,
                 int thread_id, int total_workers)
{
    thread_local bool is_shutting_down = false;
    thread_local ShardStorage storage(thread_id);
    SetThreadAffinityMask(GetCurrentThread(), 1 << thread_id);
    auto process_resp =
        [&](ConnectionContext *c, uint32_t bytes, uint32_t offset)
    {
        ParseResult res;

        if (bytes < 4)
            return res;
        const char *data = (const char *)c->buffer.GetRawData() + offset;
        if (!data || data[0] != '*')
            return res;

        /*printf("\n[INCOMING] Core: %d | Bytes: %u | Hex: ", thread_id, bytes);
        for (uint32_t i = 0; i < (bytes > 32 ? 32 : bytes); ++i) {
            printf("%02X ", (unsigned char)data[i]);
        }
        printf("| Str: ");
        for (uint32_t i = 0; i < (bytes > 32 ? 32 : bytes); ++i) {
            char c = data[i];
            printf("%c", (c >= 32 && c <= 126) ? c : '.');
        }
        printf("\n");
        fflush(stdout);
    */
        const char *p = data;
        const char *end = data + bytes;

        auto safe_find = [&](const char *start) -> const char *
        {
            if (!start || start >= end)
                return nullptr;
            return RESP_Parser::find_eol(start, end - start);
        };

        p = safe_find(p);
        if (!p)
            return res;
        p += 2;
        p = safe_find(p);
        if (!p)
            return res;
        p += 2;

        const char *cmd_ptr = p;
        p = safe_find(p);
        if (!p)
            return res;
        size_t cmd_l = p - cmd_ptr;
        p += 2;

        if (RESP_Parser::is_wks(cmd_ptr, cmd_l))
        {

            res.type = CommandType::WKS;
            res.consumed = (uint32_t)(p - data);
            res.success = true;
        }

        if (cmd_l == 0 || cmd_l > 16 || cmd_ptr < data || cmd_ptr >= end)
            return res;

        if (UNLIKELY(!cmd_ptr || cmd_ptr < data || cmd_ptr >= end))
        {
            printf("\n[!!!] CRITICAL: cmd_ptr is OUT OF BOUNDS! Addr: %p, "
                   "Data: %p, End: %p\n",
                   (void *)cmd_ptr, (void *)data, (void *)end);
            return res;
        }

        p = safe_find(p);
        if (!p)
            return res;
        p += 2;
        const char *k_ptr = p;
        p = safe_find(p);
        if (!p)
            return res;
        size_t k_len = p - k_ptr;
        p += 2;

        res.key_ptr = k_ptr;
        res.key_len = k_len;

        if (RESP_Parser::is_set(cmd_ptr, cmd_l))
        {
            res.type = CommandType::SET;
            if (p >= end || *p != '$')
                return res;

            const char *len_ptr = p + 1;
            const char *len_eol = safe_find(len_ptr);
            if (!len_eol)
                return res;

            long val_len = fast_resp_atoi(len_ptr, len_eol);
            p = len_eol + 2;

            if (end - p < val_len + 2)
                return res;

            res.val_ptr = p;
            res.val_len = (size_t)val_len;

            p += val_len;
            if (p[0] != '\r' || p[1] != '\n')
                return res;
            p += 2;

            res.consumed = (uint32_t)(p - data);
            res.success = true;
        }
        else if (RESP_Parser::is_get(cmd_ptr, cmd_l))
        {
            res.type = CommandType::GET;
            res.consumed = (uint32_t)(p - data);
            res.success = true;
        }
        else if (RESP_Parser::is_del(cmd_ptr, cmd_l))
        {
            res.type = CommandType::DEL;
            res.consumed = (uint32_t)(p - data);
            res.success = true;
        }

        return res;
    };

    OVERLAPPED_ENTRY entries[128];
    ULONG removed = 0;
    ThreadMetrics &m = global_metrics[thread_id];
    std::vector<ConnectionContext *> to_delete_now;
    to_delete_now.reserve(128);
    while (true)
    {

        BOOL ok = GetQueuedCompletionStatusEx(hIocp, entries, 128, &removed,
                                              INFINITE, FALSE);

        to_delete_now.clear();

        for (ULONG i = 0; i < removed; ++i)
        {
            DWORD bytesTransferred = entries[i].dwNumberOfBytesTransferred;
            ULONG_PTR completionKey = entries[i].lpCompletionKey;
            LPOVERLAPPED overlapped = entries[i].lpOverlapped;

            if (UNLIKELY(completionKey == 0))
            {
                is_shutting_down = true;
                continue;
            }

            /*if (LIKELY(i + 1 < removed)) {
            __builtin_prefetch((void*)entries[i+1].lpCompletionKey, 1, 3);
        }*/
            // TODO: maybe manua;

            if (!ok)
            {

                ConnectionContext *error_ctx =
                    (ConnectionContext *)completionKey;
                if (error_ctx)
                {

                    closesocket(error_ctx->socket);
                    delete error_ctx;
                }
                continue;
            }

            ConnectionContext *ctx = (ConnectionContext *)completionKey;
            if (ctx->is_dead)
                continue;
            if (LIKELY(i + 1 < removed))
            {
                __builtin_prefetch((void *)entries[i + 1].lpCompletionKey, 1,
                                   3);
            }
            __builtin_prefetch(ctx->buffer.GetRawData(), 0, 3);
            bool is_manual_signal = (overlapped == nullptr);
            /*if (!is_manual_signal) {
        //printf("[IOCP] Event: bytes=%lu, is_reading=%d, socket=%llu\n",
                //bytesTransferred, ctx->is_reading, (unsigned long
    long)ctx->socket); } else {
        //printf("[IOCP] Manual Signal (Redirect/Wakeup) for socket=%llu\n",
    (unsigned long long)ctx->socket);
    }*/

            if (!ctx->is_reading && !is_manual_signal)
            {
                // printf("[STATE] Send Completed. already_received: %u\n",
                // ctx->already_received);
                ctx->is_reading = true;
                ctx->responseLen = 0;

                if (ctx->already_received > 0)
                {
                    // printf("[WAKEUP] Leftover data found (%u bytes).
                    // Bypassing WSARecv.\n", ctx->already_received);

                    PostQueuedCompletionStatus(hIocp, 0, (ULONG_PTR)ctx,
                                               nullptr);
                    continue;
                }

                DWORD flags = 0;
                char *buf_ptr = (char *)ctx->buffer.GetRawData();
                uint32_t buf_len = 4096;

                // printf("[RECV_START] Sock:%llu | DestPtr:%p\n", (unsigned
                // long long)ctx->socket, (void*)buf_ptr);
                //  memset(&ctx->overlapped, 0, sizeof(OVERLAPPED));
                ctx->overlapped.Internal = 0;
                ctx->overlapped.InternalHigh = 0;
                ctx->overlapped.Offset = 0;
                ctx->overlapped.OffsetHigh = 0;
                ctx->overlapped.hEvent = nullptr;
                ctx->wsaBuf.buf = buf_ptr;
                ctx->wsaBuf.len = buf_len;

                int rc = WSARecv(ctx->socket, &ctx->wsaBuf, 1, NULL, &flags,
                                 &ctx->overlapped, NULL);
                if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
                {
                    // printf("[RECV_ERR] Code:%d\n", WSAGetLastError());
                    goto cleanup;
                }
                continue;
            }

            if (is_manual_signal && ctx->owner_thread_id == thread_id)
            {

                if (ctx->responseLen > 0)
                {
                    // printf("[SEND_MANUAL] Sent %u bytes, waiting for
                    // completion\n", ctx->responseLen);
                    ctx->is_reading = false;

                    WSABUF sendBuf = {(ULONG)ctx->responseLen,
                                      ctx->responseBuf};
                    ctx->overlapped.Internal = 0;
                    ctx->overlapped.InternalHigh = 0;
                    ctx->overlapped.Offset = 0;
                    ctx->overlapped.OffsetHigh = 0;
                    ctx->overlapped.hEvent = nullptr;
                    uint64_t t5 = __rdtsc();
                    if (WSASend(ctx->socket, &sendBuf, 1, NULL, 0,
                                &ctx->overlapped, NULL) == SOCKET_ERROR)
                    {
                        if (WSAGetLastError() != WSA_IO_PENDING)
                            goto cleanup;
                    }
                    uint64_t t6 = __rdtsc();
                    m.send_cycles += (t6 - t5);
                    ctx->responseLen = 0;
                    continue;
                }

                if (ctx->already_received > 0)
                {
                    // printf("[WAKEUP_EXEC] Starting parser for leftover %u
                    // bytes\n", ctx->already_received);
                }
                else
                {

                    if (!ctx->is_reading)
                    {
                        ctx->is_reading = true;
                        DWORD flags = 0;
                        ctx->wsaBuf.buf = (char *)ctx->buffer.GetRawData();
                        ctx->wsaBuf.len = 4096;
                        ctx->overlapped.Internal = 0;
                        ctx->overlapped.InternalHigh = 0;
                        ctx->overlapped.Offset = 0;
                        ctx->overlapped.OffsetHigh = 0;
                        ctx->overlapped.hEvent = nullptr;
                        WSARecv(ctx->socket, &ctx->wsaBuf, 1, NULL, &flags,
                                &ctx->overlapped, NULL);
                    }
                    continue;
                }
            }

            if (!is_manual_signal)
            {
                ctx->already_received += bytesTransferred;
            }

            if (bytesTransferred == 0 && ctx->is_reading && !is_manual_signal)
            {
                goto cleanup;
            }

#ifndef FAST_PATH

            if (ctx->is_fast_path)
            {

                if (ctx->is_reading)
                {
                    uint32_t offset = 0;

                    // printf("DEBUG PRE-WHILE: recv:%u\n",
                    // ctx->already_received);
                    while (offset < ctx->already_received)
                    {

                        __builtin_prefetch(
                            (const char *)ctx->buffer.GetRawData() + offset +
                                64,
                            0, 3);

                        m.total_ops++;
                        uint64_t t1 = __rdtsc();
                        ParseResult pres = process_resp(
                            ctx, ctx->already_received - offset, offset);
                        uint64_t t2 = __rdtsc();
                        m.parse_cycles += (t2 - t1);
                        if (!pres.success)
                        {

                            m.parse_fails++;
                            uint32_t rem = ctx->already_received - offset;

                            if (offset > 0)
                            {
                                uint32_t rem = ctx->already_received - offset;
                                memmove(ctx->buffer.GetRawData(),
                                        (char *)ctx->buffer.GetRawData() +
                                            offset,
                                        rem);
                                ctx->already_received = rem;
                            }

                            DWORD flags = 0;
                            ctx->wsaBuf.buf = (char *)ctx->buffer.GetRawData() +
                                              ctx->already_received;
                            ctx->wsaBuf.len = 4096 - ctx->already_received;
                            ctx->overlapped.Internal = 0;
                            ctx->overlapped.InternalHigh = 0;
                            ctx->overlapped.Offset = 0;
                            ctx->overlapped.OffsetHigh = 0;
                            ctx->overlapped.hEvent = nullptr;
                            if (WSARecv(ctx->socket, &ctx->wsaBuf, 1, NULL,
                                        &flags, &ctx->overlapped,
                                        NULL) == SOCKET_ERROR)
                            {
                                if (WSAGetLastError() != WSA_IO_PENDING)
                                    goto cleanup;
                            }
                            goto next_item;
                        }

                        uint64_t t3 = __rdtsc();
                        ProcessRequest(ctx, pres, storage, thread_id,
                                       total_workers);
                        uint64_t t4 = __rdtsc();
                        m.exec_cycles += (t4 - t3);

                        offset += pres.consumed;

                        if (UNLIKELY(ctx->responseLen > 7000))
                        {
                            printf("CRITICAL A");
                            break;
                        }
                    }

                    if (offset > 0)
                    {
                        uint32_t rem = ctx->already_received - offset;
                        if (rem > 0)
                        {
                            memmove(ctx->buffer.GetRawData(),
                                    (char *)ctx->buffer.GetRawData() + offset,
                                    rem);
                        }
                        ctx->already_received = rem;
                    }

                    if (ctx->responseLen > 0)
                    {

                        // printf("[STATE] Owner sending %u bytes (Final
                        // Response)\n", ctx->responseLen);

                        ctx->is_reading = false;
                        WSABUF sendBuf = {(ULONG)ctx->responseLen,
                                          ctx->responseBuf};
                        ctx->overlapped.Internal = 0;
                        ctx->overlapped.InternalHigh = 0;
                        ctx->overlapped.Offset = 0;
                        ctx->overlapped.OffsetHigh = 0;
                        ctx->overlapped.hEvent = nullptr;

                        if (WSASend(ctx->socket, &sendBuf, 1, NULL, 0,
                                    &ctx->overlapped, NULL) == SOCKET_ERROR)
                        {
                            if (WSAGetLastError() != WSA_IO_PENDING)
                                goto cleanup;
                        }
                    }
                    else
                    {

                        if (ctx->already_received < 4096)
                        {
                            DWORD flags = 0;
                            ctx->wsaBuf.buf = (char *)ctx->buffer.GetRawData() +
                                              ctx->already_received;
                            ctx->wsaBuf.len = 4096 - ctx->already_received;
                            ctx->overlapped.Internal = 0;
                            ctx->overlapped.InternalHigh = 0;
                            ctx->overlapped.Offset = 0;
                            ctx->overlapped.OffsetHigh = 0;
                            ctx->overlapped.hEvent = nullptr;
                            WSARecv(ctx->socket, &ctx->wsaBuf, 1, NULL, &flags,
                                    &ctx->overlapped, NULL);
                        }
                    }
                    goto next_item;
                }
                else
                {

                    ctx->is_reading = true;
                    ctx->responseLen = 0;

                    goto next_item;
                }
            }

            else
            {
                if (ctx->owner_thread_id == thread_id)
                {
                    if (ctx->is_reading)
                    {
                        uint32_t offset = 0;

                        // printf("DEBUG PRE-WHILE: recv:%u\n",
                        // ctx->already_received);
                        while (offset < ctx->already_received)
                        {

                            __builtin_prefetch(
                                (const char *)ctx->buffer.GetRawData() +
                                    offset + 64,
                                0, 3);

                            m.total_ops++;
                            uint64_t t1 = __rdtsc();
                            ParseResult pres = process_resp(
                                ctx, ctx->already_received - offset, offset);
                            uint64_t t2 = __rdtsc();
                            m.parse_cycles += (t2 - t1);
                            if (!pres.success)
                            {

                                m.parse_fails++;
                                uint32_t rem = ctx->already_received - offset;

                                /*printf("\n[PARSE_FAIL] Socket: %llu | Rem: %u
                                | Total: %u | Offset: %u\n",
                                       (uint64_t)ctx->socket, rem,
                                ctx->already_received, offset);

                                printf("HEX: ");
                                const unsigned char* d = (const unsigned
                                char*)ctx->buffer.GetRawData() + offset; for
                                (uint32_t i = 0; i < (rem > 32 ? 32 : rem); ++i)
                                { printf("%02X ", d[i]);
                                }

                                printf("\nSTR: ");
                                for (uint32_t i = 0; i < (rem > 32 ? 32 : rem);
                                ++i) { char c = (char)d[i]; printf("%c", (c >=
                                32 && c <= 126) ? c : '.');
                                }
                                printf("\n-----------------------------------\n");
                                fflush(stdout);*/

                                // printf("[DEBUG] Parse Fail: Socket %llu |
                                // RemBytes: %u | AlreadyRecv: %u\n",
                                //(uint64_t)ctx->socket, ctx->already_received -
                                //offset, ctx->already_received);

                                if (offset > 0)
                                {
                                    uint32_t rem =
                                        ctx->already_received - offset;
                                    memmove(ctx->buffer.GetRawData(),
                                            (char *)ctx->buffer.GetRawData() +
                                                offset,
                                            rem);
                                    ctx->already_received = rem;
                                }

                                DWORD flags = 0;
                                ctx->wsaBuf.buf =
                                    (char *)ctx->buffer.GetRawData() +
                                    ctx->already_received;
                                ctx->wsaBuf.len = 4096 - ctx->already_received;
                                ctx->overlapped.Internal = 0;
                                ctx->overlapped.InternalHigh = 0;
                                ctx->overlapped.Offset = 0;
                                ctx->overlapped.OffsetHigh = 0;
                                ctx->overlapped.hEvent = nullptr;
                                if (WSARecv(ctx->socket, &ctx->wsaBuf, 1, NULL,
                                            &flags, &ctx->overlapped,
                                            NULL) == SOCKET_ERROR)
                                {
                                    if (WSAGetLastError() != WSA_IO_PENDING)
                                        goto cleanup;
                                }
                                goto next_item;
                            }

                            uint64_t h =
                                compute_hash_resp(pres.key_ptr, pres.key_len);

                            int target_shard = h % total_workers;

                            if (target_shard != thread_id)
                            {
                                m.redirects++;

                                uint32_t rem = ctx->already_received - offset;
                                if (offset > 0 && rem > 0)
                                {
                                    memmove(ctx->buffer.GetRawData(),
                                            (char *)ctx->buffer.GetRawData() +
                                                offset,
                                            rem);
                                }
                                ctx->already_received = rem;

                                PostQueuedCompletionStatus(
                                    hIocpList[target_shard], 0, (ULONG_PTR)ctx,
                                    nullptr);
                                goto next_item;
                            }
                            uint64_t t3 = __rdtsc();
                            ProcessRequest(ctx, pres, storage, thread_id,
                                           total_workers);
                            uint64_t t4 = __rdtsc();
                            m.exec_cycles += (t4 - t3);

                            offset += pres.consumed;

                            if (UNLIKELY(ctx->responseLen > 7000))
                            {
                                printf("CRITICAL A");
                                break;
                            }
                        }

                        if (offset > 0)
                        {
                            uint32_t rem = ctx->already_received - offset;
                            if (rem > 0)
                            {
                                memmove(ctx->buffer.GetRawData(),
                                        (char *)ctx->buffer.GetRawData() +
                                            offset,
                                        rem);
                            }
                            ctx->already_received = rem;
                        }

                        if (ctx->responseLen > 0)
                        {

                            // printf("[STATE] Owner sending %u bytes (Final
                            // Response)\n", ctx->responseLen);

                            ctx->is_reading = false;
                            WSABUF sendBuf = {(ULONG)ctx->responseLen,
                                              ctx->responseBuf};
                            ctx->overlapped.Internal = 0;
                            ctx->overlapped.InternalHigh = 0;
                            ctx->overlapped.Offset = 0;
                            ctx->overlapped.OffsetHigh = 0;
                            ctx->overlapped.hEvent = nullptr;

                            if (WSASend(ctx->socket, &sendBuf, 1, NULL, 0,
                                        &ctx->overlapped, NULL) == SOCKET_ERROR)
                            {
                                if (WSAGetLastError() != WSA_IO_PENDING)
                                    goto cleanup;
                            }
                        }
                        else
                        {

                            if (ctx->already_received < 4096)
                            {
                                DWORD flags = 0;
                                ctx->wsaBuf.buf =
                                    (char *)ctx->buffer.GetRawData() +
                                    ctx->already_received;
                                ctx->wsaBuf.len = 4096 - ctx->already_received;
                                ctx->overlapped.Internal = 0;
                                ctx->overlapped.InternalHigh = 0;
                                ctx->overlapped.Offset = 0;
                                ctx->overlapped.OffsetHigh = 0;
                                ctx->overlapped.hEvent = nullptr;
                                WSARecv(ctx->socket, &ctx->wsaBuf, 1, NULL,
                                        &flags, &ctx->overlapped, NULL);
                            }
                        }
                        goto next_item;
                    }
                    else
                    {

                        ctx->is_reading = true;
                        ctx->responseLen = 0;

                        goto next_item;
                    }
                }

                else
                {
                    uint32_t offset = 0;

                    while (offset < ctx->already_received)
                    {

                        __builtin_prefetch(
                            (const char *)ctx->buffer.GetRawData() + offset +
                                64,
                            0, 3);

                        ParseResult pres = process_resp(
                            ctx, ctx->already_received - offset, offset);

                        if (!pres.success)
                            break;

                        uint64_t h =
                            compute_hash_resp(pres.key_ptr, pres.key_len);

                        int target_shard = h % total_workers;
                        if (pres.key_len == 3 &&
                            memcmp(pres.key_ptr, "WKS", 3) == 0)
                        {
                        }

                        if (target_shard != thread_id)
                        {

                            if (offset > 0)
                            {
                                uint32_t rem = ctx->already_received - offset;
                                memmove(ctx->buffer.GetRawData(),
                                        (char *)ctx->buffer.GetRawData() +
                                            offset,
                                        rem);
                                ctx->already_received = rem;
                            }

                            PostQueuedCompletionStatus(hIocpList[target_shard],
                                                       0, (ULONG_PTR)ctx,
                                                       nullptr);
                            goto next_item;
                        }

                        ProcessRequest(ctx, pres, storage, thread_id,
                                       total_workers);

                        offset += pres.consumed;

                        if (UNLIKELY(ctx->responseLen > 7000))
                        {
                            printf("CRITICAL B");
                            break;
                        }
                    }
                    // printf("[END_WHILE_A] Thr:%d | resLen:%u | off:%u |
                    // recv:%u\n",
                    // thread_id, ctx->responseLen, offset,
                    // ctx->already_received);
                    // fflush(stdout);

                    if (offset > 0)
                    {
                        uint32_t rem = ctx->already_received - offset;
                        if (rem > 0)
                        {
                            memmove(ctx->buffer.GetRawData(),
                                    (char *)ctx->buffer.GetRawData() + offset,
                                    rem);
                        }
                        ctx->already_received = rem;
                    }

                    ctx->is_reading = true;
                    PostQueuedCompletionStatus(hIocpList[ctx->owner_thread_id],
                                               0, (ULONG_PTR)ctx, nullptr);
                    goto next_item;
                }
            }

#else

            if (ctx->owner_thread_id == thread_id)
            {
                if (ctx->is_reading)
                {
                    uint32_t offset = 0;

                    // printf("DEBUG PRE-WHILE: recv:%u\n",
                    // ctx->already_received);
                    while (offset < ctx->already_received)
                    {

                        __builtin_prefetch(
                            (const char *)ctx->buffer.GetRawData() + offset +
                                64,
                            0, 3);

                        m.total_ops++;
                        uint64_t t1 = __rdtsc();
                        ParseResult pres = process_resp(
                            ctx, ctx->already_received - offset, offset);
                        uint64_t t2 = __rdtsc();
                        m.parse_cycles += (t2 - t1);
                        if (!pres.success)
                        {

                            m.parse_fails++;
                            uint32_t rem = ctx->already_received - offset;

                            /*printf("\n[PARSE_FAIL] Socket: %llu | Rem: %u |
                            Total: %u | Offset: %u\n", (uint64_t)ctx->socket,
                            rem, ctx->already_received, offset);

                            printf("HEX: ");
                            const unsigned char* d = (const unsigned
                            char*)ctx->buffer.GetRawData() + offset; for
                            (uint32_t i = 0; i < (rem > 32 ? 32 : rem); ++i) {
                                printf("%02X ", d[i]);
                            }

                            printf("\nSTR: ");
                            for (uint32_t i = 0; i < (rem > 32 ? 32 : rem); ++i)
                            { char c = (char)d[i]; printf("%c", (c >= 32 && c <=
                            126) ? c : '.');
                            }
                            printf("\n-----------------------------------\n");
                            fflush(stdout);*/

                            // printf("[DEBUG] Parse Fail: Socket %llu |
                            // RemBytes: %u | AlreadyRecv: %u\n",
                            //(uint64_t)ctx->socket, ctx->already_received -
                            //offset, ctx->already_received);

                            if (offset > 0)
                            {
                                uint32_t rem = ctx->already_received - offset;
                                memmove(ctx->buffer.GetRawData(),
                                        (char *)ctx->buffer.GetRawData() +
                                            offset,
                                        rem);
                                ctx->already_received = rem;
                            }

                            DWORD flags = 0;
                            ctx->wsaBuf.buf = (char *)ctx->buffer.GetRawData() +
                                              ctx->already_received;
                            ctx->wsaBuf.len = 4096 - ctx->already_received;
                            ctx->overlapped.Internal = 0;
                            ctx->overlapped.InternalHigh = 0;
                            ctx->overlapped.Offset = 0;
                            ctx->overlapped.OffsetHigh = 0;
                            ctx->overlapped.hEvent = nullptr;
                            if (WSARecv(ctx->socket, &ctx->wsaBuf, 1, NULL,
                                        &flags, &ctx->overlapped,
                                        NULL) == SOCKET_ERROR)
                            {
                                if (WSAGetLastError() != WSA_IO_PENDING)
                                    goto cleanup;
                            }
                            goto next_item;
                        }

                        uint64_t h =
                            compute_hash_resp(pres.key_ptr, pres.key_len);

                        int target_shard = h % total_workers;

                        if (target_shard != thread_id)
                        {
                            m.redirects++;

                            uint32_t rem = ctx->already_received - offset;
                            if (offset > 0 && rem > 0)
                            {
                                memmove(ctx->buffer.GetRawData(),
                                        (char *)ctx->buffer.GetRawData() +
                                            offset,
                                        rem);
                            }
                            ctx->already_received = rem;

                            PostQueuedCompletionStatus(hIocpList[target_shard],
                                                       0, (ULONG_PTR)ctx,
                                                       nullptr);
                            goto next_item;
                        }
                        uint64_t t3 = __rdtsc();
                        ProcessRequest(ctx, pres, storage, thread_id,
                                       total_workers);
                        uint64_t t4 = __rdtsc();
                        m.exec_cycles += (t4 - t3);

                        offset += pres.consumed;

                        if (UNLIKELY(ctx->responseLen > 7000))
                        {
                            printf("CRITICAL A");
                            break;
                        }
                    }

                    if (offset > 0)
                    {
                        uint32_t rem = ctx->already_received - offset;
                        if (rem > 0)
                        {
                            memmove(ctx->buffer.GetRawData(),
                                    (char *)ctx->buffer.GetRawData() + offset,
                                    rem);
                        }
                        ctx->already_received = rem;
                    }

                    if (ctx->responseLen > 0)
                    {

                        // printf("[STATE] Owner sending %u bytes (Final
                        // Response)\n", ctx->responseLen);

                        ctx->is_reading = false;
                        WSABUF sendBuf = {(ULONG)ctx->responseLen,
                                          ctx->responseBuf};
                        ctx->overlapped.Internal = 0;
                        ctx->overlapped.InternalHigh = 0;
                        ctx->overlapped.Offset = 0;
                        ctx->overlapped.OffsetHigh = 0;
                        ctx->overlapped.hEvent = nullptr;

                        if (WSASend(ctx->socket, &sendBuf, 1, NULL, 0,
                                    &ctx->overlapped, NULL) == SOCKET_ERROR)
                        {
                            if (WSAGetLastError() != WSA_IO_PENDING)
                                goto cleanup;
                        }
                    }
                    else
                    {

                        if (ctx->already_received < 4096)
                        {
                            DWORD flags = 0;
                            ctx->wsaBuf.buf = (char *)ctx->buffer.GetRawData() +
                                              ctx->already_received;
                            ctx->wsaBuf.len = 4096 - ctx->already_received;
                            ctx->overlapped.Internal = 0;
                            ctx->overlapped.InternalHigh = 0;
                            ctx->overlapped.Offset = 0;
                            ctx->overlapped.OffsetHigh = 0;
                            ctx->overlapped.hEvent = nullptr;
                            WSARecv(ctx->socket, &ctx->wsaBuf, 1, NULL, &flags,
                                    &ctx->overlapped, NULL);
                        }
                    }
                    goto next_item;
                }
                else
                {

                    ctx->is_reading = true;
                    ctx->responseLen = 0;

                    goto next_item;
                }
            }

            else
            {
                uint32_t offset = 0;

                while (offset < ctx->already_received)
                {

                    __builtin_prefetch((const char *)ctx->buffer.GetRawData() +
                                           offset + 64,
                                       0, 3);

                    ParseResult pres = process_resp(
                        ctx, ctx->already_received - offset, offset);

                    if (!pres.success)
                        break;

                    uint64_t h = compute_hash_resp(pres.key_ptr, pres.key_len);

                    int target_shard = h % total_workers;
                    if (pres.key_len == 3 &&
                        memcmp(pres.key_ptr, "WKS", 3) == 0)
                    {
                    }

                    if (target_shard != thread_id)
                    {

                        if (offset > 0)
                        {
                            uint32_t rem = ctx->already_received - offset;
                            memmove(ctx->buffer.GetRawData(),
                                    (char *)ctx->buffer.GetRawData() + offset,
                                    rem);
                            ctx->already_received = rem;
                        }

                        PostQueuedCompletionStatus(hIocpList[target_shard], 0,
                                                   (ULONG_PTR)ctx, nullptr);
                        goto next_item;
                    }

                    ProcessRequest(ctx, pres, storage, thread_id,
                                   total_workers);

                    offset += pres.consumed;

                    if (UNLIKELY(ctx->responseLen > 7000))
                    {
                        printf("CRITICAL B");
                        break;
                    }
                }
                // printf("[END_WHILE_A] Thr:%d | resLen:%u | off:%u |
                // recv:%u\n",
                // thread_id, ctx->responseLen, offset, ctx->already_received);
                // fflush(stdout);

                if (offset > 0)
                {
                    uint32_t rem = ctx->already_received - offset;
                    if (rem > 0)
                    {
                        memmove(ctx->buffer.GetRawData(),
                                (char *)ctx->buffer.GetRawData() + offset, rem);
                    }
                    ctx->already_received = rem;
                }

                ctx->is_reading = true;
                PostQueuedCompletionStatus(hIocpList[ctx->owner_thread_id], 0,
                                           (ULONG_PTR)ctx, nullptr);
                goto next_item;
            }

#endif

            continue;

        cleanup:
            if (ctx && !ctx->is_dead)
            {
                closesocket(ctx->socket);
                ctx->is_dead = true;
                g_stats.active_conns.fetch_sub(1, std::memory_order_relaxed);

                to_delete_now.push_back(ctx);
            }
            continue;

        next_item:
        }

        if (is_shutting_down)
        {

            int active_idx = storage.active_idx;
            if (!storage.dbs[active_idx].empty())
            {
                std::vector<String_lib::String> tmp_list;

                SnapshotShard(storage.dbs[active_idx], thread_id,
                              storage.chunk_id++, storage.temp_files);
            }

            if (!storage.temp_files.empty())
            {
                printf("[Worker %d] Final merge of %zu chunks...\n", thread_id,
                       storage.temp_files.size());
                FastExternalMerge(storage.temp_files, thread_id, storage);
            }
            storage.Close();
            char name0[64], name1[64];
            sprintf(name0, "thread_%d_arena_0.mmap", thread_id);
            sprintf(name1, "thread_%d_arena_1.mmap", thread_id);

            std::error_code ec;
            std::filesystem::remove(name0, ec);
            std::filesystem::remove(name1, ec);

            printf("[Worker %d] Storage cleaned up and arenas deleted.\n",
                   thread_id);

            return;

            PostQueuedCompletionStatus(hIocp, 0, 0, NULL);
        }

        for (auto *dead_ctx : to_delete_now)
        {
            delete dead_ctx;
        }
    }
}

SOCKET connect_to_shard(int shard_id)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    sockaddr_in addr{AF_INET, htons(6380 + shard_id)};
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(s, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR)
    {

        return INVALID_SOCKET;
    }

    int nodelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay,
               sizeof(int));

    return s;
}

int discover_total_workers(const char *ip, int port)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{AF_INET, htons(port)};
    addr.sin_addr.s_addr = inet_addr(ip);

    if (connect(s, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR)
        return 1;

    const char *msg = "*1\r\n$3\r\nWKS\r\n";
    send(s, msg, (int)strlen(msg), 0);

    char buf[64];
    int n = recv(s, buf, 63, 0);
    closesocket(s);
    if (n > 0 && buf[0] == ':')
    {

        return atoi(buf + 1);
    }
    return -1;
}

void smart_bench_worker(int total_workers, int total_ops, int start_i)
{

    std::vector<SOCKET> shard_sockets(total_workers);

    std::vector<std::string> shard_buffers(total_workers);

    std::vector<int> shard_counts(total_workers, 0);

    for (int i = 0; i < total_workers; ++i)
    {
        shard_sockets[i] = connect_to_shard(i);
        // printf("commected to %i ",i);
    }
    // TODO: try 128 32 256
    int batch_limit = 512;

    for (int i = start_i; i < start_i + total_ops; ++i)
    {
        char key_buf[32];
        int k_len = sprintf(key_buf, "key:%08d", i);

        uint64_t h = compute_hash_resp(key_buf, k_len);
        int target = h % total_workers;

        char cmd[128];
        int c_len =
            sprintf(cmd, "*3\r\n$3\r\nSET\r\n$%d\r\n%s\r\n$4\r\nval1\r\n",
                    k_len, key_buf);
        shard_buffers[target].append(cmd, c_len);
        shard_counts[target]++;

        if (shard_counts[target] >= batch_limit)
        {
            send(shard_sockets[target], shard_buffers[target].data(),
                 shard_buffers[target].size(), 0);

            shard_buffers[target].clear();
            shard_counts[target] = 0;

            char recv_buf[4096];
            recv(shard_sockets[target], recv_buf, batch_limit * 5,
                 0); // "+OK\r\n" * 128

            g_bench_ops.fetch_add(batch_limit, std::memory_order_relaxed);
        }
    }

    for (int i = 0; i < total_workers; ++i)
    {
        if (!shard_buffers[i].empty())
        {
            send(shard_sockets[i], shard_buffers[i].data(),
                 shard_buffers[i].size(), 0);
        }
        closesocket(shard_sockets[i]);
    }
}

void smart_bench_get_worker(int total_workers, int total_ops)
{
    std::vector<SOCKET> shard_sockets(total_workers);
    std::vector<std::string> shard_buffers(total_workers);
    std::vector<int> shard_counts(total_workers, 0);

    for (int i = 0; i < total_workers; ++i)
    {
        shard_sockets[i] = connect_to_shard(i);
    }

    int batch_limit = 512;

    int expected_bytes = batch_limit * 10;
    char recv_buf[16384];

    for (int i = 0; i < total_ops; ++i)
    {
        char key_buf[32];
        int k_len = sprintf(key_buf, "key:%08d", i);

        uint64_t h = compute_hash_resp(key_buf, k_len);
        int target = h % total_workers;

        // RESP GET: *2\r\n$3\r\nGET\r\n$12\r\nkey:00000001\r\n
        char cmd[128];
        int c_len =
            sprintf(cmd, "*2\r\n$3\r\nGET\r\n$%d\r\n%s\r\n", k_len, key_buf);
        shard_buffers[target].append(cmd, c_len);
        shard_counts[target]++;

        if (shard_counts[target] >= batch_limit)
        {
            send(shard_sockets[target], shard_buffers[target].data(),
                 (int)shard_buffers[target].size(), 0);

            int received = 0;
            while (received < expected_bytes)
            {
                int n = recv(shard_sockets[target], recv_buf + received,
                             expected_bytes - received, 0);
                if (n <= 0)
                    break;
                received += n;
            }

            shard_buffers[target].clear();
            shard_counts[target] = 0;
            g_bench_ops.fetch_add(batch_limit, std::memory_order_relaxed);
        }
    }

    for (int i = 0; i < total_workers; ++i)
    {
        closesocket(shard_sockets[i]);
    }
}

void start_smart_benchmark(int num_threads, int ops_per_thread)
{

    int total_shards = discover_total_workers("127.0.0.1", 6379);
    printf("\nSERVER WORKERS_COUNT: %i\n", total_shards);
    std::vector<std::thread> threads;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_threads; ++i)
    {

        threads.emplace_back(smart_bench_worker, total_shards, ops_per_thread,
                             i * ops_per_thread);
    }

    for (auto &t : threads)
        t.join();

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    std::cout << "SMART Bench Results: " << g_bench_ops << " ops in "
              << diff.count() << "s\n";
    std::cout << "Avg RPS: " << (double)g_bench_ops / diff.count() << "\n";
}

void start_smart_get_benchmark(int num_threads, int ops_per_thread)
{
    int total_shards = discover_total_workers("127.0.0.1", 6379);
    printf("\n[GET TEST] SERVER WORKERS_COUNT: %i\n", total_shards);

    g_bench_ops.store(0);
    std::vector<std::thread> threads;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_threads; ++i)
    {
        threads.emplace_back(smart_bench_get_worker, total_shards,
                             ops_per_thread);
    }

    for (auto &t : threads)
        t.join();

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    std::cout << "GET SMART Bench Results: " << g_bench_ops << " ops in "
              << diff.count() << "s\n";
    std::cout << "Avg RPS: " << (double)g_bench_ops / diff.count() << "\n";
}

void smart_bench_del_worker(int total_workers, int total_ops, int start_i)
{
    std::vector<SOCKET> shard_sockets(total_workers);
    std::vector<std::string> shard_buffers(total_workers);
    std::vector<int> shard_counts(total_workers, 0);

    for (int i = 0; i < total_workers; ++i)
    {
        shard_sockets[i] = connect_to_shard(i);
    }

    int batch_limit = 512;

    int expected_bytes = batch_limit * 4;
    char recv_buf[16384];

    for (int i = start_i; i < start_i + total_ops; ++i)
    {
        char key_buf[32];
        int k_len = sprintf(key_buf, "key:%08d", i);

        uint64_t h = compute_hash_resp(key_buf, k_len);
        int target = h % total_workers;

        // RESP GET: *2\r\n$3\r\nGET\r\n$12\r\nkey:00000001\r\n
        char cmd[128];
        int c_len =
            sprintf(cmd, "*2\r\n$3\r\nDEL\r\n$%d\r\n%s\r\n", k_len, key_buf);
        shard_buffers[target].append(cmd, c_len);
        shard_counts[target]++;

        if (shard_counts[target] >= batch_limit)
        {
            send(shard_sockets[target], shard_buffers[target].data(),
                 (int)shard_buffers[target].size(), 0);

            int received = 0;
            while (received < expected_bytes)
            {
                int n = recv(shard_sockets[target], recv_buf + received,
                             expected_bytes - received, 0);
                if (n <= 0)
                    break;
                received += n;
            }

            shard_buffers[target].clear();
            shard_counts[target] = 0;
            g_bench_ops.fetch_add(batch_limit, std::memory_order_relaxed);
        }
    }

    for (int i = 0; i < total_workers; ++i)
    {
        if (!shard_buffers[i].empty())
        {

            send(shard_sockets[i], shard_buffers[i].data(),
                 (int)shard_buffers[i].size(), 0);

            int tail_count = shard_counts[i];
            int tail_expected_bytes = tail_count * 4;

            int tail_received = 0;
            while (tail_received < tail_expected_bytes)
            {
                int n = recv(shard_sockets[i], recv_buf + tail_received,
                             tail_expected_bytes - tail_received, 0);
                if (n <= 0)
                    break;
                tail_received += n;
            }

            g_bench_ops.fetch_add(tail_count, std::memory_order_relaxed);
            shard_buffers[i].clear();
            shard_counts[i] = 0;
        }

        closesocket(shard_sockets[i]);
    }
}

void start_smart_del_benchmark(int num_threads, int ops_per_thread)
{
    int total_shards = discover_total_workers("127.0.0.1", 6379);
    printf("\n[DEL TEST] SERVER WORKERS_COUNT: %i\n", total_shards);

    g_bench_ops.store(0);
    std::vector<std::thread> threads;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_threads; ++i)
    {
        threads.emplace_back(smart_bench_del_worker, total_shards,
                             ops_per_thread, i * ops_per_thread);
    }

    for (auto &t : threads)
        t.join();

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    std::cout << "DEL SMART Bench Results: " << g_bench_ops << " ops in "
              << diff.count() << "s\n";
    std::cout << "Avg RPS: " << (double)g_bench_ops / diff.count() << "\n";
}

void smart_latency_worker(int total_workers, int ops_per_thread)
{

    std::vector<SOCKET> shard_sockets(total_workers);
    for (int i = 0; i < total_workers; ++i)
    {
        shard_sockets[i] = connect_to_shard(i);
    }

    std::vector<double> latencies;
    latencies.reserve(ops_per_thread);

    char recv_buf[1024];

    for (int i = 0; i < ops_per_thread; ++i)
    {
        char key_buf[32];
        int k_len = sprintf(key_buf, "lat:%08d", i);

        uint64_t h = compute_hash_resp(key_buf, k_len);
        int target = h % total_workers;

        char cmd[128];
        int c_len =
            sprintf(cmd, "*3\r\n$3\r\nSET\r\n$%d\r\n%s\r\n$4\r\nval1\r\n",
                    k_len, key_buf);

        auto t1 = std::chrono::high_resolution_clock::now();

        send(shard_sockets[target], cmd, c_len, 0);

        int n = recv(shard_sockets[target], recv_buf, sizeof(recv_buf), 0);

        auto t2 = std::chrono::high_resolution_clock::now();

        if (n > 0)
        {
            std::chrono::duration<double, std::micro> elapsed = t2 - t1;
            latencies.push_back(elapsed.count());
            g_bench_ops.fetch_add(1, std::memory_order_relaxed);
        }
    }

    std::sort(latencies.begin(), latencies.end());

    double avg = 0;
    for (double l : latencies)
        avg += l;
    avg /= latencies.size();

    printf("\n[Latency Thread] Ops: %d | Avg: %.1fus | p50: %.1fus | p95: "
           "%.1fus | p99: %.1fus | Max: %.1fus\n",
           ops_per_thread, avg, latencies[latencies.size() * 0.50],
           latencies[latencies.size() * 0.95],
           latencies[latencies.size() * 0.99], latencies.back());

    for (auto s : shard_sockets)
        closesocket(s);
}

void start_latency_benchmark(int num_threads, int ops_per_thread)
{
    int total_shards = discover_total_workers("127.0.0.1", 6379);
    printf("\n[LATENCY TEST] Shards: %i | Threads: %i\n", total_shards,
           num_threads);

    g_bench_ops.store(0);
    std::vector<std::thread> threads;

    for (int i = 0; i < num_threads; ++i)
    {
        threads.emplace_back(smart_latency_worker, total_shards,
                             ops_per_thread);
    }

    for (auto &t : threads)
        t.join();
}

struct StatsHistory
{
    static constexpr int MAX_SAMPLES = 100;
    float rps_samples[MAX_SAMPLES] = {0};
    float conn_samples[MAX_SAMPLES] = {0};
    int current_idx = 0;
    uint64_t last_ops = 0;

    void update(uint64_t total_ops, uint64_t active_conns)
    {
        rps_samples[current_idx] = (float)(total_ops - last_ops);
        conn_samples[current_idx] = (float)active_conns;

        last_ops = total_ops;
        current_idx = (current_idx + 1) % MAX_SAMPLES;
    }
};

StatsHistory g_hist;

void run_server(SDL_Window *window, int w, int h)
{

    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    int num_workers =
        sysInfo.dwNumberOfProcessors > 1 ? sysInfo.dwNumberOfProcessors - 1 : 1;

    std::vector<HANDLE> hIocpList(num_workers);
    for (int i = 0; i < num_workers; ++i)
    {
        hIocpList[i] = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    }

    // 2. RECOVERY PHASE
    printf("[RECOVERY] Scanning for unsaved arenas and chunks...\n");

    std::vector<std::string> mmap_to_recover;
    for (auto &p : std::filesystem::directory_iterator("."))
    {
        std::string fname = p.path().filename().string();
        if (fname.find("thread_") == 0 &&
            fname.find(".mmap") != std::string::npos)
        {
            mmap_to_recover.push_back(p.path().string());
        }
    }

    for (const auto &mmap_path : mmap_to_recover)
    {
        int tid = -1, aid = -1;
        std::string fname =
            std::filesystem::path(mmap_path).filename().string();

        if (sscanf(fname.c_str(), "thread_%d_arena_%d.mmap", &tid, &aid) == 2)
        {
            size_t recovered_size = 0;
            {
                MmapReader raw_reader(mmap_path.c_str());
                ankerl::unordered_dense::map<String_lib::StringView,
                                             String_lib::StringView,
                                             StringHasher, StringEq>
                    recover_db;
                String_lib::StringView k, v;

                while (raw_reader.next_pair(k, v))
                {
                    recover_db[k] = v;
                }

                if (!recover_db.empty())
                {
                    recovered_size = raw_reader.m_offset;
                    std::vector<String_lib::String> tmp_list;

                    SnapshotShard(recover_db, tid, 9000 + aid, tmp_list);
                }
            }
            std::filesystem::remove(mmap_path);
        }
    }

    std::map<int, std::vector<String_lib::String>> shard_chunks;
    for (auto &p : std::filesystem::directory_iterator("."))
    {
        std::string fname = p.path().filename().string();
        if (fname.find("shard_") == 0 &&
            fname.find(".bin") != std::string::npos)
        {
            int sid = -1;
            if (sscanf(fname.c_str(), "shard_%d_chunk_", &sid) == 1)
            {
                shard_chunks[sid].push_back(String_lib::String(fname.c_str()));
            }
        }
    }

    if (!shard_chunks.empty())
    {
        printf("[RECOVERY] Found lost data for %zu shards. Merging to RDB...\n",
               shard_chunks.size());
        for (auto &[sid, files] : shard_chunks)
        {
            if (sid >= num_workers)
                continue;

            ShardStorage temp_storage(sid);

            FastExternalMerge(files, sid, temp_storage);

            printf("[RECOVERY] Shard %d is now READY on disk.\n", sid);
        }
    }

    global_metrics = (ThreadMetrics *)_aligned_malloc(
        sizeof(ThreadMetrics) * num_workers, 64);
    if (!global_metrics)
    {
    }
    memset(global_metrics, 0, sizeof(ThreadMetrics) * num_workers);

    std::vector<std::thread> accept_threads;
    std::vector<std::thread> worker_threads;
    std::vector<SOCKET> listen_sockets;

    for (int i = 0; i < num_workers; ++i)
    {
        int worker_port = 6380 + i;
        SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

        sockaddr_in addr{AF_INET, htons((unsigned short)worker_port)};
        addr.sin_addr.s_addr = INADDR_ANY;

        int reuse = 1;
        setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse,
                   sizeof(reuse));

        if (bind(listenSock, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR)
        {
            printf("[!] FAILED BIND %d. Error: %d\n", worker_port,
                   WSAGetLastError());
            closesocket(listenSock);
            break;
            ;
        }
        listen(listenSock, SOMAXCONN);
        printf("[*] Port %d is READY (Shard %d)\n", worker_port, i);

        HANDLE myIocp = hIocpList[i];

        listen_sockets.push_back(listenSock);

        accept_threads.emplace_back(
            [listenSock, myIocp, i]()
            {
                while (true)
                {
                    SOCKET client = accept(listenSock, NULL, NULL);
                    if (client == INVALID_SOCKET)
                        break;

                    int nodelay = 1;
                    setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
                               (const char *)&nodelay, sizeof(nodelay));

                    int sndbuf = 0;
                    setsockopt(client, SOL_SOCKET, SO_SNDBUF,
                               (const char *)&sndbuf, sizeof(sndbuf));

                    ConnectionContext *ctx = new ConnectionContext(client, i);
                    ctx->is_reading = true;
#ifndef FAST_PATH
                    ctx->is_fast_path = true;
#endif

                    CreateIoCompletionPort((HANDLE)client, myIocp,
                                           (ULONG_PTR)ctx, 0);

                    DWORD flags = 0;
                    DWORD received = 0;
                    ctx->overlapped.Internal = 0;
                    ctx->overlapped.InternalHigh = 0;
                    ctx->overlapped.Offset = 0;
                    ctx->overlapped.OffsetHigh = 0;
                    ctx->overlapped.hEvent = nullptr;

                    if (WSARecv(client, &ctx->wsaBuf, 1, &received, &flags,
                                &ctx->overlapped, NULL) == SOCKET_ERROR)
                    {
                        if (WSAGetLastError() != WSA_IO_PENDING)
                        {
                            printf("[!] Initial WSARecv failed: %d\n",
                                   WSAGetLastError());
                            closesocket(client);
                            delete ctx;
                            continue;
                        }
                    }

                    g_stats.active_conns.fetch_add(1,
                                                   std::memory_order_relaxed);
                }
            });

        worker_threads.emplace_back(
            [myIocp, i, hIocpList, num_workers]()
            {
                SetThreadAffinityMask(GetCurrentThread(),
                                      (DWORD_PTR)1 << (i + 1));

                worker_loop(hIocpList, myIocp, i, num_workers);
            });
    }

    SOCKET mainListenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{AF_INET, htons(6379)};
    bind(mainListenSock, (sockaddr *)&addr, sizeof(addr));
    listen(mainListenSock, SOMAXCONN);

    listen_sockets.push_back(mainListenSock);

    accept_threads.emplace_back(
        [mainListenSock, hIocpList, num_workers]()
        {
            uint64_t conn_counter = 0;
            while (true)
            {
                SOCKET client = accept(mainListenSock, NULL, NULL);
                if (client == INVALID_SOCKET)
                {
                    break;
                }

                int target = (conn_counter++) % num_workers;
                ConnectionContext *ctx = new ConnectionContext(client, target);
#ifndef FAST_PATH
                ctx->is_fast_path = false;
#endif
                ctx->is_reading = true;
                CreateIoCompletionPort((HANDLE)client, hIocpList[target],
                                       (ULONG_PTR)ctx, 0);

                DWORD flags = 0;
                ctx->overlapped.Internal = 0;
                ctx->overlapped.InternalHigh = 0;
                ctx->overlapped.Offset = 0;
                ctx->overlapped.OffsetHigh = 0;
                ctx->overlapped.hEvent = nullptr;
                WSARecv(client, &ctx->wsaBuf, 1, NULL, &flags, &ctx->overlapped,
                        NULL);
                g_stats.active_conns.fetch_add(1, std::memory_order_relaxed);
            }
        });

    bool running = true;
    uint32_t last_stats_update = SDL_GetTicks();

    while (running)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_EVENT_QUIT)
                running = false;
            else if (event.type == SDL_EVENT_KEY_DOWN)
            {
                if (event.key.key == SDLK_ESCAPE)
                {
                    // Release
                    running = false;
                }
            }
        }

        uint32_t now = SDL_GetTicks();
        if (now - last_stats_update >= 1000)
        {
            uint64_t total_ops =
                g_stats.total_ops.load(std::memory_order_acquire);
            uint64_t active_conns =
                g_stats.active_conns.load(std::memory_order_relaxed);

            g_hist.update(total_ops, active_conns);

            uint64_t rps =
                (uint64_t)g_hist.rps_samples[(g_hist.current_idx +
                                              StatsHistory::MAX_SAMPLES - 1) %
                                             StatsHistory::MAX_SAMPLES];

            std::cout << "[Neko Server] RPS: " << rps
                      << " | Active Conns: " << active_conns
                      << " | Total Ops: " << total_ops << std::endl;

            last_stats_update = now;

            // PrintProfilerReport(num_workers);
        }

        glClearColor(0.05f, 0.05f, 0.07f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glBegin(GL_LINES);
        for (int i = 0; i < StatsHistory::MAX_SAMPLES; ++i)
        {
            int idx = (g_hist.current_idx + i) % StatsHistory::MAX_SAMPLES;
            float x = -1.0f + (2.0f * i / StatsHistory::MAX_SAMPLES);

            float rps_val = g_hist.rps_samples[idx] / 1000000.0f;
            glBegin(GL_LINES);
            glColor3f(0.0f, 1.0f, 0.4f);
            glVertex2f(x, -1.0f);
            glVertex2f(x, -1.0f + rps_val);
            glEnd();

            float conn_val = g_hist.conn_samples[idx] / 2000.0f;
            glBegin(GL_POINTS);
            glColor3f(0.0f, 0.7f, 1.0f);
            glVertex2f(x, -1.0f + conn_val);
            glEnd();
        }

        SDL_GL_SwapWindow(window);
        SDL_Delay(16);
    }
    printf("[FINISH] Server started stop\n");

    for (int i = 0; i < (int)worker_threads.size(); ++i)
    {
        uint32_t start_join = SDL_GetTicks();
        printf("  [Worker %d] Sending Poison Pill and waiting for join...", i);

        for (int p = 0; p < 128; ++p)
        {
            PostQueuedCompletionStatus(hIocpList[i], 0, (ULONG_PTR)0, NULL);
        }

        if (worker_threads[i].joinable())
        {
            worker_threads[i].join();
            uint32_t duration = SDL_GetTicks() - start_join;
            printf(" DONE (%u ms)\n", duration);
        }
        else
        {
            printf(" SKIP (not joinable)\n");
        }
    }

    for (SOCKET s : listen_sockets)
        closesocket(s);

    for (auto &t : accept_threads)
    {
        if (t.joinable())
            t.join();
    }

    for (auto h : hIocpList)
        CloseHandle(h);
    _aligned_free(global_metrics);
    WSACleanup();

    printf("[FINISH] Server stopped gracefully. All threads joined.\n");
}