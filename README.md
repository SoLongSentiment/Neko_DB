# Neko_DB

A high-performance Key-Value store for Windows.

### Performance (Tested on i5-11400H Laptop)
*   **GET:** 7.0M+ RPS 
*   **SET:** 4.8M+ RPS
*   **DEL:** 3.7M+ RPS

### Features
*   **Network:** Native WinSock2 + IOCP.
*   **Storage:** LSM-tree architecture with double-buffering arenas.
*   **SIMD Optimization:** Adaptive RESP parsing and hashing.
*   **Shared-Nothing:** Zero mutexes on the hot path.
*   **Recovery:** Automatic data recovery from `.mmap` arenas after crashes.
*   **Monitoring:** Real-time RPS and connection graphs via **SDL + OpenGL**.

### Tech Stack
*   **Language:** C++23
*   **OS:** Windows 10/11
*   **Dependencies:** SDL (for GUI), `ankerl::unordered_dense`.
