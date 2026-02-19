#pragma once

#include <SDL3/SDL.h>
#include <memory>
#include <sstream>

namespace neko::SDL
{
    template <typename Creator, typename Detructor, typename... Arguments>
    inline auto MakeResource(Creator c, Detructor d, Arguments&&... args)
    {
        auto r = c(std::forward<Arguments>(args)...);
        return std::unique_ptr<std::decay_t<decltype(*r)>, decltype(d)>(r, d);
    }

    using SDL_System = int;

    inline SDL_System* SDL_CreateSDL(uint32_t flags)
    {
        auto init_status = new SDL_System;
        *init_status = SDL_Init(flags);
        return init_status;
    }

    inline void SDL_DestroySDL(SDL_System* init_status)
    {
        delete init_status;
        SDL_Quit();
    }

    using Sdlsystem_ptr = std::unique_ptr<SDL_System, decltype(&SDL_DestroySDL)>;
    using Window_ptr = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>;
    using Renderer_ptr = std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)>;
    using Surface_ptr = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>;
    using Texture_ptr = std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)>;

    inline Sdlsystem_ptr MakeSdlsystem(uint32_t flags)
    {
        return MakeResource(SDL_CreateSDL, SDL_DestroySDL, flags);
    }

    inline Window_ptr MakeWindow(const char* title, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t flags)
    {
        return MakeResource(SDL_CreateWindow, SDL_DestroyWindow, title, w, h, flags);
    }

    inline Renderer_ptr MakeRenderer(SDL_Window* window, const char* driver)
    {
        return MakeResource(SDL_CreateRenderer, SDL_DestroyRenderer, window, driver);
    }

    inline Surface_ptr MakeBMP(SDL_IOStream* sdlfile, bool closeio)
    {
        return MakeResource(SDL_LoadBMP_IO, SDL_DestroySurface, sdlfile, closeio);
    }

    inline Texture_ptr MakeTexture(SDL_Renderer* renderer, SDL_Surface* surface)
    {
        return MakeResource(SDL_CreateTextureFromSurface, SDL_DestroyTexture, renderer, surface);
    }

    class SDL_Error : public std::exception
    {
        private:

        std::string m_errorMessage;

        public:

        SDL_Error(const char* errorMessage)
        {
            std::stringstream msg;
            msg << errorMessage << ": " << SDL_GetError();
            this->m_errorMessage = msg.str();
        }

        SDL_Error(const std::string& errorMessage)
            : SDL_Error(errorMessage.c_str())
        {}

        const char* what() const noexcept override
        {
            return m_errorMessage.c_str();
        }

    };

}