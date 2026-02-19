#ifndef NEKOGRAPHICSDEVICE_OPENGL_HPP
#define NEKOGRAPHICSDEVICE_OPENGL_HPP

#include "Neko_SDL3.hpp"
#include "include/glad/glad.h"
#include <math.h>

namespace neko::graphics 
{
    class GraphicsDeviceOpenGL
    {
    public:
        GraphicsDeviceOpenGL();
        GraphicsDeviceOpenGL(const GraphicsDeviceOpenGL&);
        ~GraphicsDeviceOpenGL();

        bool init(SDL_Window*, int, int, float, float, bool);
        void Shutdown();

        void BeginScene(float, float, float, float);
        void PresentScene();

        void BuildIdentityMatrix(float*);
        void BuildPerspectiveFovMatrix(float*, float, float, float, float);
        void BuildOrthoMatrix(float*, float,  float, float, float);
        
        void GetWorldMatrix(float*);
        void GetProjectionMatrix(float*);
        void GetOrthoMatrix(float*);

        void MatrixRotationX(float*, float);
        void MatrixRotationY(float*, float);
        void MatrixRotationZ(float*, float);
        void MatrixTranslation(float*, float, float, float);
        void MatrixScale(float*, float, float, float);
        void MatrixTranspose(float*, float*);
        void MatrixMultiply(float*, float*, float*);
        void MatrixInverse(float*, float*);

        void TurnZBufferOn();
        void TurnZBufferOff();

        void EnableAlphaBlending();
        void DisableAlphaBlending();

        void SetBackBufferRenderTarget();

        void ResetViewport();

        void EnableClipping();
        void DisableClipping();

    private:
        SDL_Window* m_window;
        float m_worldMatrix[16];
        float m_projectionMatrix[16];
        float m_orthoMatrix[16];
        float m_screenWidth, m_screenHeight;

    };
}

#endif