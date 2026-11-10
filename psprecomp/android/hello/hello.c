/* Android milestone 1: SDL2 + GLES 3 + touch + gamepad, nothing of the game. Shows the background colour moving with touch,
 * flashes on any gamepad button, and writes the GL strings, display size, touch and pad events to logcat (tag SDL/APP). */
#include <SDL.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdbool.h>

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) { SDL_Log("SDL_Init: %s", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_Window* w = SDL_CreateWindow("psprecomp hello", 0, 0, 0, 0, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!w) { SDL_Log("CreateWindow: %s", SDL_GetError()); return 1; }
    SDL_GLContext gl = SDL_GL_CreateContext(w);
    if (!gl) { SDL_Log("GL context: %s", SDL_GetError()); return 1; }
    SDL_GL_SetSwapInterval(1);
    SDL_Log("GL_VERSION  %s", (const char*)glGetString(GL_VERSION));
    SDL_Log("GL_RENDERER %s", (const char*)glGetString(GL_RENDERER));
    SDL_Log("GLSL        %s", (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION));
    GLint maxtex = 0, maxdb = 0; glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxtex); glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maxdb);
    SDL_Log("max texture %d, draw buffers %d", maxtex, maxdb);
    int pw = 0, ph = 0; SDL_GL_GetDrawableSize(w, &pw, &ph);
    SDL_Log("drawable %dx%d, %d joystick(s)", pw, ph, SDL_NumJoysticks());
    SDL_GameController* pads[8] = {0};
    for (int i = 0; i < SDL_NumJoysticks() && i < 8; i++) if (SDL_IsGameController(i)) { pads[i] = SDL_GameControllerOpen(i); SDL_Log("pad %d: %s", i, SDL_GameControllerName(pads[i])); }
    float r = 0.1f, g = 0.1f, b = 0.3f, flash = 0;
    bool run = true;
    while (run) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: run = false; break;
            case SDL_FINGERDOWN: case SDL_FINGERMOTION:
                r = e.tfinger.x; g = e.tfinger.y; b = 1.0f - e.tfinger.x;
                SDL_Log("touch %.2f %.2f", e.tfinger.x, e.tfinger.y); break;
            case SDL_CONTROLLERBUTTONDOWN: flash = 1; SDL_Log("pad button %d", e.cbutton.button); break;
            case SDL_CONTROLLERAXISMOTION: if (e.caxis.value > 20000 || e.caxis.value < -20000) SDL_Log("pad axis %d = %d", e.caxis.axis, e.caxis.value); break;
            case SDL_CONTROLLERDEVICEADDED: {
                int i = e.cdevice.which;
                if (i < 8 && !pads[i]) { pads[i] = SDL_GameControllerOpen(i); SDL_Log("pad added: %s", SDL_GameControllerName(pads[i])); }
                break; }
            case SDL_WINDOWEVENT: SDL_Log("window event %d", e.window.event); break;
            default: break;
            }
        }
        glClearColor(r + flash, g + flash, b + flash, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        flash *= 0.9f;
        SDL_GL_SwapWindow(w);
    }
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(w);
    SDL_Quit();
    return 0;
}
