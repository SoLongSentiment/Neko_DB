#include "Neko_Input.hpp"
#include "Neko_SDL3.hpp"

//TODO: Add events and job system

namespace neko::input
{
    bool m_keyboardState[256];

    float mouseX = 0, mouseY = 0;

    SDL_MouseButtonFlags mouseFlags;

    bool m_mouse_1 = false;

    bool m_keyboard_Escape = false;
    bool m_keyboard_A = false;
    bool m_keyboard_D = false;

    bool m_quit = false;

    void init()
    {
        //init keyboard state
        for(int i = 0;i<256;i++)
        {
            m_keyboardState[i] = false;
        }

        return;
    }

    void Update()
    {
        SDL_Event e;

        mouseFlags = SDL_GetMouseState(&mouseX, &mouseY);

        while(SDL_PollEvent(&e) != 0)
        {
            if(e.type == SDL_EVENT_KEY_DOWN ) 
            {
                if (e.key.key == SDLK_A) 
                {
                    SDL_Log("A PRESSED.");
                    m_keyboard_A = true;
                }
                else if (e.key.key == SDLK_D) 
                {
                    SDL_Log("D PRESSED.");
                    m_keyboard_D = true;
                }
                else if(e.key.key == SDLK_ESCAPE)
                {
                    //Release
                    m_keyboard_Escape = true;
                }
            }
            else if(e.type == SDL_EVENT_KEY_UP)
            {
                if (e.key.key == SDLK_A) 
                {
                    SDL_Log("A RELEASED.");
                    m_keyboard_A = false;
                }
                else if (e.key.key == SDLK_D) 
                {
                    SDL_Log("D RELEASED.");
                    m_keyboard_D = false;
                }
            }
            else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) 
            {
                //TODO: timestamp, clcks
                if(e.button.button == 1)
                {
                    m_mouse_1 = true;
                }
            }
            else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) 
            {
                if(e.button.button == 1)
                {
                    m_mouse_1 = false;
                }
            }
            else if(e.type == SDL_EVENT_QUIT)
            {
                m_quit = true;
            }
        }
    }

    bool isQuit()
    {
        return m_quit;
    }
    
    bool isEscapePressed()
    {
        return m_keyboard_Escape;
    }

    bool isAPressed()
    {
        return m_keyboard_A;
    }

    bool isDPressed()
    {
        return m_keyboard_D;
    }

    bool isMouseLeftPressed()
    {
        return m_mouse_1;
    }

    void GetMouseLocation(int& x, int& y)
    {
        x=mouseX;
        y=mouseY;
    }
}