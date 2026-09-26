
#ifndef _BLITTER_H_
#define _BLITTER_H_

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif


#include "SDL.h"

typedef struct {
    const char *name;
    const char *desc;
    int(*init) ();
    int(*resize) (int w, int h);
    void (*update) ();
    void (*fullscreen) ();
    void (*close) ();
} blitter_func;

/* Software blitter */
int blitter_soft_init();
void blitter_soft_update();
void blitter_soft_fullscreen();
void blitter_soft_close();

/* OpenGL blitter */
int blitter_opengl_init();
int blitter_opengl_resize(int w, int h);
void blitter_opengl_update();
void blitter_opengl_fullscreen();
void blitter_opengl_close();

/* GLSL blitter */
int blitter_glsl_init();
int blitter_glsl_resize(int w, int h);
void blitter_glsl_update();
void blitter_glsl_fullscreen();
void blitter_glsl_close();
int blitter_glsl_grab(const char *path);

/* Overlay blitter */
int blitter_overlay_init();
int blitter_overlay_resize();
void blitter_overlay_update();
void blitter_overlay_fullscreen();
void blitter_overlay_close();

extern blitter_func blitter[];


#endif
