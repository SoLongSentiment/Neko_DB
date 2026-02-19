#include <atomic>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <thread>
#include <unordered_set>
#include <vector>


#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpragma-pack"
#include <SDL3/SDL.h>
#pragma clang diagnostic push

//#include "Neko_System.hpp"

//#include "../server.hpp"
#include "../server_inmem.hpp"

void optimize_system_for_bench()
{

    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    // SetProcessAffinityMask(GetCurrentProcess(), 0xFFF);
}


int main(int argc, char *argv[]) 
{
    optimize_system_for_bench();
    
            SDL_Window* m_window;
        SDL_GLContext m_glcontext;

        //int m_screenWidth = 640, m_screenHeight = 480;
        int m_screenWidth = 903, m_screenHeight = 903;

        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            std::cout << "SDL not initialized";
            return -1;
        }

         m_window = SDL_CreateWindow("Neko", m_screenWidth, m_screenHeight, SDL_WINDOW_OPENGL);

        if(m_window == nullptr)
        {
            std::cout << "SDL WINDOW not initialized";
            SDL_Quit();
            return -1;
        }

        m_glcontext = SDL_GL_CreateContext(m_window);

        if(m_glcontext == nullptr)
        {
            std::cout << "GLCONTEXT not initialized";
            SDL_DestroyWindow(m_window);
            SDL_Quit();
            return -1;
        }

        // load GL functions
        if(!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress))
        {
            std::cout << "GL functions not loaded";
            SDL_DestroyWindow(m_window);
            SDL_Quit();
            return -1;
        }

        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

        int majorVersion;
        glGetIntegerv(GL_MAJOR_VERSION, &majorVersion);
        if(majorVersion < 4)
        {
            std::cout << majorVersion;
            SDL_Log("GL not 4");
            SDL_DestroyWindow(m_window);
            SDL_Quit();
            return -1;
        }

        std::thread([]() {
    Sleep(2000); 
    //run_heavy_persistence_test(1, 2000);

    //start_smart_benchmark(24, 2000000);
    //start_smart_get_benchmark(24, 2000000);
    // start_smart_mget_benchmark(24, 2000000);
    //start_smart_del_benchmark(24, 2000000);
    
    start_latency_benchmark(1, 20000);
    start_latency_benchmark(11, 20000);
    //start_full_stress_test(1000, 2000000); 
    //start_benchmark(1000, 200000); 
}).detach();

    run_server(m_window, m_screenWidth, m_screenHeight);

    return 0;
}
