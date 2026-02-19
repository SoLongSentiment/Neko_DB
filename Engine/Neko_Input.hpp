#ifndef NEKOINPUT_HPP
#define NEKOINPUT_HPP

namespace neko::input 
{
    void init();

    void Update();

    bool isQuit();
    
    bool isEscapePressed();

    bool isMouseLeftPressed();

    bool isAPressed();
    bool isDPressed();

    void GetMouseLocation(int&, int&);

}

#endif