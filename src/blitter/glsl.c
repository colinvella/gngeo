#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#if (defined(HAVE_GL_GL_H) || defined(HAVE_OPENGL_GL_H)) && defined (USE_GLSL)

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "SDL.h"
#ifdef USE_GL2
#include "GL/glew.h"
#else
#include "SDL_opengles2.h"
#include "SDL_opengles2_gl2.h"
#endif
#include "../emu.h"
#include "../screen.h"
#include "../video.h"
#include "../effect.h"
#include "../conf.h"
#include "../gnutil.h"

#if !defined(I386_ASM) && !defined(PROCESSOR_ARM)
#define RGB24_PIXELS 1
#endif

/*
 * Libretro's GLSL preset
 * https://github.com/libretro/docs/blob/master/docs/specs/shader.md
 */
#define SOURCE 0
#define ABSOLUTE 1
#define VIEWPORT 2

/// A pass configuration in a GLSL preset
typedef struct _pass_ {
	int    scale_type_x;
	int    scale_type_y;
	int    filter_linear;
	float  scale_x;
	float  scale_y;
	GLuint program;
	int    src_width;
	int    src_height;
	GLuint src_texture;
	int    dst_width;
	int    dst_height;
	GLuint dst_framebuffer;
} *pass_t;

/// The entire GLSL multi-pass rendering
static pass_t pipeline;

/// Number of passes in the GLSL rendering
static int nb_passes;

/*
 * Rendering settings
 */

/// configured viewport in the total visible area (320x224)
static SDL_Rect screen_rect = { 8,  0, 304, 224};
/// configured screen ratio, maps classic CRT ratio
static float screen_ratio = 4.0/3.0;
static Uint32 current_window_width;
static Uint32 current_window_height;
/* size to restore when leaving fullscreen; conf.res_x/res_y get overwritten
   with the fullscreen dimensions by the resize event, so they cannot serve */
static int windowed_width;
static int windowed_height;

/// The pixel data to upload into the source GL texture
static SDL_Surface *input_pixels;
/// The input texture that is feeded to the GLSL rendering
static GLuint input_tex;

/// Number of frames to clear after resize, for double buffer
int clear_after_resize;

#ifdef USE_GL2
/// The vertex array object required for rendering with OpenGL
static GLuint vao;
#endif

/*
 * vertex and texel coordinate buffers for rendering
 */

static GLuint vertex_buffer;
static GLuint tex_buffer;
static GLuint screen_buffer;

GLfloat MVPMatrix[] = {
	1.0, 0.0, 0.0, 0.0,
	0.0, 1.0, 0.0, 0.0,
	0.0, 0.0, 1.0, 0.0,
	0.0, 0.0, 0.0, 1.0
};

GLfloat VertexData[] = {
	-1.0, -1.0,
	 1.0, -1.0,
	-1.0,  1.0,
	-1.0,  1.0,
	 1.0, -1.0,
	 1.0,  1.0
};

GLfloat TexData[] = {
	0.0,  1.0,
	1.0,  1.0,
	0.0,  0.0,
	0.0,  0.0,
	1.0,  1.0,
	1.0,  0.0
};

GLfloat ScreenData[] = {
	0.0,  0.0,
	1.0,  0.0,
	0.0,  1.0,
	0.0,  1.0,
	1.0,  0.0,
	1.0,  1.0
};


/*
 * GLSL shader parsing and compilation
 */

#define SHADER_TAG_LEN 200

/// Read the content of a file into an allocated buffer.
/// Wrap the content into a CPP define statement.
static char* load_file_and_wrap(char *file, char *shadertag)
{
	FILE *fptr;
	long length;
	char *buf;
	char *shader_buf;
	char def_buf[SHADER_TAG_LEN];
	snprintf(def_buf, SHADER_TAG_LEN, "#version 100\n#define %s 1\n", shadertag);

	fptr = fopen(file, "rb");
	if (!fptr) {
		printf("Could not open %s for reading: %s\n", file, strerror(errno));
		return NULL;
	}
	fseek(fptr, 0, SEEK_END);
	length = ftell(fptr);
	buf = (char*)malloc(SHADER_TAG_LEN+length+1);

	strncpy(buf, def_buf, SHADER_TAG_LEN);
	shader_buf = buf+strlen(buf);

	fseek(fptr, 0, SEEK_SET);
	fread(shader_buf, length, 1, fptr);
	fclose(fptr);
	shader_buf[length] = 0;

	return buf;
}

GLuint compile_shader(char* shader_name, GLenum shader_type)
{
	char *shader_path;
	char *full_name;
	char *src;
	GLuint shader;
	char *type_name;
	int buflen;
	int status;

	shader_path = CF_STR(cf_get_item_by_name("shaderpath"));
	buflen = strlen(shader_path)+strlen(shader_name)+2;
	full_name = malloc(buflen);
	snprintf(full_name, buflen, "%s/%s",shader_path, shader_name);

	type_name = shader_type==GL_VERTEX_SHADER?"VERTEX":"FRAGMENT";
	shader = glCreateShader(shader_type);
	if (shader == 0) {
		printf("Could not create a %s shader object\n", type_name);
		return GL_FALSE;
	}
	src = load_file_and_wrap(full_name, type_name);
	if (src == NULL) { return GL_FALSE; }
	glShaderSource(shader, 1, (const GLchar**)&src, 0);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if(status == GL_FALSE) {
		int length;
		char *log;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
		log = malloc(length);
		glGetShaderInfoLog(shader, length, &length, log);
		printf("Error when compiling shader:\n%s\n", log);
		free(log);
		return GL_FALSE;
	}
	return shader;
}

GLuint compile_shader_program(char* shader_name)
{
	GLuint vertex, fragment, program;
	int linked;

	vertex = compile_shader(shader_name, GL_VERTEX_SHADER);
	if (vertex == GL_FALSE) { return GL_FALSE; }

	fragment = compile_shader(shader_name, GL_FRAGMENT_SHADER);
	if (fragment == GL_FALSE) { return GL_FALSE; }

	program = glCreateProgram();
	glAttachShader(program, vertex);
	glAttachShader(program, fragment);
	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, (int *)&linked);
	if (!linked) {
		int length = 0;
		char *log;
		glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
		log = (char *)malloc(length);
		glGetProgramInfoLog(program, length, &length, log);
		printf("Error when linking shader:\n%s\n", log);
		free(log);
		return GN_FALSE;
	}

	// GLuint attr;
	// GLuint uni;
	// glGetProgramiv(program, GL_ACTIVE_ATTRIBUTES, &attr);
	// glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &uni);
	// printf("%d - %d attribs\n",program,attr);
	// printf("%d - %d uniforms\n",program,uni);
	printf("Linked shader program: %s\n",shader_name);

	return program;
}

/*
 * Utility functions to set up the GLSL rendering
 */

static void set_texture_filter_linear(GLuint texture, int linear)
{
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear?GL_LINEAR:GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear?GL_LINEAR:GL_NEAREST);
}

static GLuint create_input_texture()
{
	GLuint texture;
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	set_texture_filter_linear(texture, GL_FALSE);
	return texture;
}

static void create_pass_framebuffer(int width, int height, GLuint *texture, GLuint *framebufferid)
{
	*texture = create_input_texture();
	*framebufferid = 0;
	glGenFramebuffers(1, framebufferid);
	// printf("gen fb: %d (with tex: %d)\n", *framebufferid, *texture);
	glBindFramebuffer(GL_FRAMEBUFFER, *framebufferid);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *texture, 0);
}

static void init_static_gl_coords_buffers()
{
#ifdef USE_GL2
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
#endif
	glGenBuffers(1,&vertex_buffer);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
	glBufferData(GL_ARRAY_BUFFER, 12 * sizeof(GLfloat), VertexData, GL_STATIC_DRAW);

	glGenBuffers(1,&tex_buffer);
	glBindBuffer(GL_ARRAY_BUFFER, tex_buffer);
	glBufferData(GL_ARRAY_BUFFER, 12 * sizeof(GLfloat), TexData, GL_STATIC_DRAW);

	glGenBuffers(1,&screen_buffer);
	glBindBuffer(GL_ARRAY_BUFFER, screen_buffer);
	glBufferData(GL_ARRAY_BUFFER, 12 * sizeof(GLfloat), ScreenData, GL_STATIC_DRAW);
}

static void update_texture_dimensions(int viewport_width, int viewport_height, int verbose)
{
	int i;
	for (i=0; i<nb_passes; i++) {
		pass_t p = &pipeline[i];
		switch (p->scale_type_x) {
		case VIEWPORT:
			p->dst_width = viewport_width;
			break;
		case ABSOLUTE:
			p->dst_width = 1;
			break;
		case SOURCE:
		default:
			p->dst_width = p->src_width;
		}
		switch (p->scale_type_y) {
		case VIEWPORT:
			p->dst_height = viewport_height;
			break;
		case ABSOLUTE:
			p->dst_height = 1;
			break;
		case SOURCE:
		default:
			p->dst_height = p->src_height;
		}
		p->dst_width *= p->scale_x;
		p->dst_height *= p->scale_y;
		if (verbose) {
			printf("Pass %d output texture size: %d x %d\n",
			       i, p->dst_width, p->dst_height);
		}
		if (i<nb_passes-1) {
			pipeline[i+1].src_width = p->dst_width;
			pipeline[i+1].src_height = p->dst_height;
			glBindTexture(GL_TEXTURE_2D, pipeline[i+1].src_texture);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
				     p->dst_width,p->dst_height, 0,
				     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		}
	}
}


/*
 * GLSL preset parser functions
 */

char* trim(char* start, char*end)
{
	for(;end-1>=start && (isspace(*(end-1)) || *(end-1)=='"'); end--);
	*end = '\0';
	for(;start<end && (isspace(*start) || *start=='"'); start++);
	return start;
}

int startsWith(char* val, char* start)
{
	return strncasecmp(start, val, strlen(start)) == 0;
}

#define GLSLP_SHADER        "shader"
#define GLSLP_SCALE_TYPE_X  "scale_type_x"
#define GLSLP_SCALE_TYPE_Y  "scale_type_y"
#define GLSLP_SCALE_TYPE    "scale_type"
#define GLSLP_SCALE_X       "scale_x"
#define GLSLP_SCALE_Y       "scale_y"
#define GLSLP_SCALE         "scale"
#define GLSLP_FILTER_LINEAR "filter_linear"

#define PARSE_INT(expr, var)						\
{									\
	char* fail = NULL;						\
	var = strtol(expr, &fail, 10);					\
	if (*fail != '\0') {						\
		printf("bad parsing, not an integer: '%s'\n", val);	\
		return GN_FALSE;					\
	}								\
}

#define PARSE_FLOAT(expr, var)						\
{									\
	char* fail = NULL;						\
	var = strtof(expr, &fail);					\
	if (*fail != '\0') {						\
		printf("bad parsing, not an integer: '%s'\n", val);	\
		return GN_FALSE;					\
	}								\
}

#define PARSE_SCALE(val, var)						\
{									\
	int stype;							\
	if (!strcmp(val, "source"))        { stype = SOURCE; }		\
	else if (!strcmp(val, "absolute")) { stype = ABSOLUTE; }	\
	else if (!strcmp(val, "viewport")) { stype = VIEWPORT; }	\
	else {								\
		printf("Invalid scale type: '%s'\n", val);		\
		return GN_FALSE;                                        \
	}								\
	var = stype;							\
}

#define PARSE_POS(expr, var)						\
{									\
	char* fail = NULL;						\
	var = strtol(expr, &fail, 10);					\
	if (*fail != '\0') {						\
		printf("bad parsing, not an integer: '%s'\n", val);	\
		return GN_FALSE;					\
	}								\
	if (pipeline == NULL) {						\
		printf("Number of shaders not yet initialized\n");	\
		return GN_FALSE;					\
	}								\
	if (var<0 || var>=nb_passes) {					\
		printf("Shader number is invalid: '%d'\n", var);	\
		return GN_FALSE;					\
	}								\
}

int parse_shader_preset(char *glslpname,
                        int start_width, int start_height,
                        int window_width, int window_height)
{
	char *stmt;
	int pos;
	int buflen;
	int i;
	char *shaderpath, *sbuf;
	char *glslp;

	shaderpath = CF_STR(cf_get_item_by_name("shaderpath"));
	buflen = strlen(shaderpath)+strlen(glslpname)+2;
	sbuf = malloc(buflen);
	snprintf(sbuf, buflen, "%s/%s",shaderpath, glslpname);
	printf("Loading GLSL preset %s\n", sbuf);
	glslp=load_file_and_wrap(sbuf, "");
	if (glslp == NULL) return GN_FALSE;

	for (stmt=strtok(glslp, "\n"); stmt!=NULL; stmt=strtok(NULL, "\n")) {
		//printf("\nSTMT: |%s|\n",stmt);
		char *eqpos = strchr(stmt, '=');

		if (eqpos == NULL) continue;
		char *val = trim(eqpos+1, stmt+strlen(stmt));
		char *key = trim(stmt, eqpos);
		//printf("KEY: |%s|\n", key);
		//printf("VAL: |%s|\n", val);
		if (strcmp(key, "shaders") == 0) {
			PARSE_INT(val, nb_passes);
			pipeline = calloc(nb_passes, sizeof(struct _pass_));
			for (i=0; i<nb_passes; i++) {
				pipeline[i].scale_type_x = SOURCE;
				pipeline[i].scale_type_y = SOURCE;
				pipeline[i].scale_x = 1.0;
				pipeline[i].scale_y = 1.0;
			}
			pipeline[0].src_texture = input_tex;
			pipeline[0].src_width = start_width;
			pipeline[0].src_height = start_height;
			pipeline[0].filter_linear = GN_FALSE;
			pipeline[nb_passes-1].scale_type_x = VIEWPORT;
			pipeline[nb_passes-1].scale_type_y = VIEWPORT;
			pipeline[nb_passes-1].dst_framebuffer = 0;
			printf("Initializing a %d-passes shader pipeline\n", nb_passes);
			continue;
		}
		else if (startsWith(key, GLSLP_SHADER)) {
			PARSE_POS(key+strlen(GLSLP_SHADER), pos);
			GLuint shader = compile_shader_program(val);
			if (shader == GL_FALSE) {
				printf("Unable to compile shader program %s\n", val);
				return GL_FALSE;
			}
			pipeline[pos].program = shader;
		}
		else if (startsWith(key, GLSLP_SCALE_TYPE_X)) {
			PARSE_POS(key+strlen(GLSLP_SCALE_TYPE_X), pos);
			PARSE_SCALE(val, pipeline[pos].scale_type_x);
		}
		else if (startsWith(key, GLSLP_SCALE_TYPE_Y)) {
			PARSE_POS(key+strlen(GLSLP_SCALE_TYPE_Y), pos);
			PARSE_SCALE(val, pipeline[pos].scale_type_y);
		}
		else if (startsWith(key, GLSLP_SCALE_TYPE)) {
			PARSE_POS(key+strlen(GLSLP_SCALE_TYPE), pos);
			PARSE_SCALE(val, pipeline[pos].scale_type_x);
			PARSE_SCALE(val, pipeline[pos].scale_type_y);
		}
		else if (startsWith(key, GLSLP_SCALE_X)) {
			PARSE_POS(key+strlen(GLSLP_SCALE_X), pos);
			PARSE_FLOAT(val, pipeline[pos].scale_x);
		}
		else if (startsWith(key, GLSLP_SCALE_Y)) {
			PARSE_POS(key+strlen(GLSLP_SCALE_Y), pos);
			PARSE_FLOAT(val, pipeline[pos].scale_y);
		}
		else if (startsWith(key, GLSLP_SCALE)) {
			PARSE_POS(key+strlen(GLSLP_SCALE), pos);
			PARSE_FLOAT(val, pipeline[pos].scale_x);
			PARSE_FLOAT(val, pipeline[pos].scale_y);
		}
		else if (startsWith(key, GLSLP_FILTER_LINEAR)) {
			PARSE_POS(key+strlen(GLSLP_FILTER_LINEAR), pos);
			pipeline[pos].filter_linear = (strcasecmp(val, "true") == 0);
		}
	}

	// Initialize FBO and texture for each pass
	for (i=0; i<nb_passes; i++) {
		pass_t p = &pipeline[i];
		set_texture_filter_linear(p->src_texture, p->filter_linear);
		if (i<nb_passes-1) {
			GLuint texture;
			GLuint framebufferid;
			create_pass_framebuffer(p->dst_width, p->dst_height,
						&texture, &framebufferid);
			p->dst_framebuffer = framebufferid;
			pipeline[i+1].src_texture = texture;
		}
	}

	// Set the textures' initial dimensions
	update_texture_dimensions(window_width, window_height, GN_TRUE);

	return GN_TRUE;
}


/*
 * GnGeo Blitter API
 */

int
blitter_glsl_init()
{
	SDL_GLContext context;
        int glsl_ret;

        if (window != NULL) return GN_TRUE;

        if (neffect != 0) {
		printf("ERROR: GLSL blitter does not support effect\n");
		return GN_FALSE;
        }

        // compute real window size and save it for when switching in
        // and out of fullscreen
        current_window_height = screen_rect.h*scale;
        current_window_width = (int)(current_window_height * screen_ratio);
        conf.res_x = current_window_width;
	conf.res_y = current_window_height;

        // the surface that will be uploaded
#ifndef RGB24_PIXELS
        input_pixels = SDL_CreateRGBSurface(SDL_SWSURFACE, screen_rect.w, screen_rect.h,
					    16, 0xF800, 0x7E0, 0x1F, 0);
#else
        input_pixels = SDL_CreateRGBSurface(SDL_SWSURFACE, screen_rect.w, screen_rect.h,
					    32, 0, 0, 0, 0);
#endif

	window = SDL_CreateWindow("GnGeo",
                                  SDL_WINDOWPOS_UNDEFINED,
                                  SDL_WINDOWPOS_UNDEFINED,
                                  (int)current_window_width,
                                  (int)current_window_height,
                                  (fullscreen?SDL_WINDOW_FULLSCREEN_DESKTOP:0) |
                                  SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN |
                                  SDL_WINDOW_OPENGL );
	if ( window == NULL)
		return GN_FALSE;

	// Configure GL or GL ES depending on the lib we depend on
#ifdef USE_GL2
	SDL_GL_SetAttribute( SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE );
	SDL_GL_SetAttribute( SDL_GL_CONTEXT_MAJOR_VERSION, 3 );
	SDL_GL_SetAttribute( SDL_GL_CONTEXT_MINOR_VERSION, 2 );
#else
	SDL_GL_SetAttribute( SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES );
	SDL_GL_SetAttribute( SDL_GL_CONTEXT_MAJOR_VERSION, 2 );
	SDL_GL_SetAttribute( SDL_GL_CONTEXT_MINOR_VERSION, 0 );
#endif
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        context = SDL_GL_CreateContext(window);
        SDL_GL_SetSwapInterval(1);

#ifdef USE_GL2
	// GLEW ensures that we get access to all the GL functions
	// required for proper shader compilation
	glewExperimental = GL_TRUE;
	glewInit();
#endif
	printf("Using GL: %s\n", glGetString(GL_VERSION));

        // Initialize the GLSL rendering pipeline
        input_tex = create_input_texture();
        init_static_gl_coords_buffers();
        glsl_ret = parse_shader_preset(CF_STR(cf_get_item_by_name("shader")),
                                       screen_rect.w, screen_rect.h,
                                       current_window_width, current_window_height);
        if (glsl_ret == GN_FALSE) {
		printf("Could not initialize GLSL shaders\n");
		return GN_FALSE;
        }

	SDL_ShowWindow(window);
	return GN_TRUE;
}

int
blitter_glsl_resize(int w, int h)
{
	int viewport_width=w, viewport_height=h;
	current_window_width = w;
	current_window_height = h;

	if (CF_BOOL(cf_get_item_by_name("resize"))) {
		if (((float)w/h) >= screen_ratio) {
			viewport_width = (int)(h*screen_ratio);
		} else {
			viewport_height = (int)(w/screen_ratio);
		}

		update_texture_dimensions(viewport_width, viewport_height, GN_FALSE);
	}
	// FIXME: Only need to clear buffer during resize because the
	// viewport may be smaller than the window
	clear_after_resize = 2;

	return GN_TRUE;
}

static void render_pass(int orig_width, int orig_height,
                        GLuint src_texture, int src_width, int src_height,
                        GLuint dst_framebuffer, int dst_width, int dst_height,
                        GLuint program)
{
	glBindTexture(GL_TEXTURE_2D, src_texture);
	glBindFramebuffer(GL_FRAMEBUFFER, dst_framebuffer);
	GLint x = 0;
	GLint y = 0;
	if (dst_framebuffer == 0) {
		x = ((int)current_window_width - dst_width)/2;
		y = ((int)current_window_height - dst_height)/2;
	}
	glViewport(x, y, dst_width, dst_height);

	glUseProgram(program);

	// location of uniform input in the linked program
	GLuint l_VertexCoord = glGetAttribLocation(program, "VertexCoord");
	GLuint l_TexCoord = glGetAttribLocation(program, "TexCoord");
	GLuint l_TextureSize = glGetUniformLocation(program, "TextureSize");
	GLuint l_InputSize = glGetUniformLocation(program, "InputSize");
	GLuint l_OutputSize = glGetUniformLocation(program, "OutputSize");
	GLuint l_MVPMatrix = glGetUniformLocation(program, "MVPMatrix");
	// GLuint l_FrameDirection = glGetUniformLocation(program, "FrameDirection");
	// GLuint l_FrameCount = glGetUniformLocation(program, "FrameCount");

#ifdef USE_GL2
	glBindVertexArray(vao);
#endif

	glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
	glVertexAttribPointer(l_VertexCoord, 2, GL_FLOAT, GL_FALSE, 0, 0);
	glEnableVertexAttribArray(l_VertexCoord);

	glBindBuffer(GL_ARRAY_BUFFER, dst_framebuffer!=0?screen_buffer:tex_buffer);
	glVertexAttribPointer(l_TexCoord, 2, GL_FLOAT, GL_FALSE, 0, 0);
	glEnableVertexAttribArray(l_TexCoord);

	if (l_TextureSize != -1) glUniform2f(l_TextureSize, (float)orig_width, (float)orig_height);
	if (l_InputSize != -1) glUniform2f(l_InputSize, (float)src_width, (float)src_height);
	if (l_OutputSize != -1) glUniform2f(l_OutputSize, (float)dst_width, (float)dst_height);
	if (l_MVPMatrix != -1) glUniformMatrix4fv(l_MVPMatrix, 1, GL_FALSE, MVPMatrix);
	// if (l_FrameDirection != -1) glUniform1f(l_FrameDirection, 0);
	// if (l_FrameCount != -1) glUniform1i(l_FrameCount, 0);

	// pass previsouly registered buffers to the fragment shader
	glDrawArrays(GL_TRIANGLES, 0, 6);
}

void
blitter_glsl_update()
{
	int i;
	// GnGeo renders graphics in a big internal 2D buffer. Pixels are
	// written in the "visible area", a 320x224 sub-rect with a
	// (16,16) offset from the start of the internal buffer.
	//
	// A lot of ROMs crop the screen to 304x224, at the center of the
	// visible area (represented by screen_rect).
	//
	// viewport_rect is the final visible area in GnGeo's internal 2D
	// buffer.
	SDL_Rect viewport_rect = screen_rect;
	viewport_rect.x += visible_area.x;
	viewport_rect.y += visible_area.y;
	SDL_BlitSurface(buffer, &viewport_rect, input_pixels, NULL);

	if (clear_after_resize>0) {
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glClearColor(0.0, 0.0, 0.0, 1.0);
		glClear(GL_COLOR_BUFFER_BIT);
		clear_after_resize--;
	}

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, input_tex);
#ifndef RGB24_PIXELS
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, screen_rect.w, screen_rect.h,
		     0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, input_pixels->pixels);
#else
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, screen_rect.w, screen_rect.h,
		     0, GL_BGRA, GL_UNSIGNED_BYTE, input_pixels->pixels);
#endif

	// render: TEX -> (pass1_prog) -> FBO -> ... -> (pass2_prog) -> OUTPUT
	for (i=0; i<nb_passes; i++) {
		pass_t p = &pipeline[i];
		render_pass(screen_rect.w, screen_rect.h,
			    p->src_texture, p->src_width, p->src_height,
			    p->dst_framebuffer, p->dst_width, p->dst_height,
			    p->program);
	}

	SDL_GL_SwapWindow(window);
}

void
blitter_glsl_close()
{
	//if (screen != NULL)
	//	SDL_FreeSurface(screen);
}

void
blitter_glsl_fullscreen()
{
	int w, h;

	/* Remember the windowed size before switching: the resize event that
	   the switch generates overwrites conf.res_x/res_y with the fullscreen
	   dimensions, so conf is no longer a way back. */
	if (fullscreen)
		SDL_GetWindowSize(window, &windowed_width, &windowed_height);

	if (SDL_SetWindowFullscreen(window,
				    fullscreen?SDL_WINDOW_FULLSCREEN_DESKTOP:0) != 0) {
		printf("Could not toggle fullscreen: %s\n", SDL_GetError());
		fullscreen ^= 1;	/* keep the flag in step with reality */
		return;
	}

	if (!fullscreen && windowed_width > 0)
		SDL_SetWindowSize(window, windowed_width, windowed_height);

	/* Ask the window what it actually became. The previous code read
	   SDL_GetWindowDisplayMode() without checking its return value, so a
	   failure fed an uninitialised stack SDL_DisplayMode -- garbage width
	   and height -- straight into the resize. */
	SDL_GetWindowSize(window, &w, &h);
	if (w <= 0 || h <= 0) {
		printf("Fullscreen toggle gave a %dx%d window, ignoring\n", w, h);
		return;
	}

	blitter_glsl_resize(w, h);
}

#endif
