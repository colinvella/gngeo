/*
 * control.c -- remote control socket for automated testing.
 *
 * A listener thread accepts one client at a time and speaks a line protocol.
 * It never touches emulator state itself: a command is parked in a request
 * slot and serviced by the emulation thread from control_frame_begin(), which
 * is also where pausing and frame stepping happen. So every reply is ordered
 * against a frame boundary, and a screenshot or a RAM read always describes a
 * defined moment rather than whatever the emulator happened to be part way
 * through.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define CLOSESOCK closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define CLOSESOCK close
#endif

#include <SDL.h>

#include "control.h"
#include "emu.h"
#include "memory.h"
#include "screen.h"
#include "event.h"
#include "conf.h"
#include "sound.h"
#include "blitter.h"

#define MAX_LINE 1024
#define MAX_REPLY 8192

/* ------------------------------------------------------------------ state */

static int enabled = 0;
static SOCKET listen_sock = INVALID_SOCKET;
/* The one connected client, so shutdown can say goodbye on it. A harness that
   simply sees the connection reset cannot tell "somebody closed the window"
   from "the emulator crashed", and those want opposite reactions -- the first
   is the end of the session, the second is a bug to chase. So the emulator
   says which it is on the way out. */
static volatile SOCKET client_sock = INVALID_SOCKET;
static SDL_Thread *listen_thread = NULL;
static volatile int stopping = 0;

static SDL_mutex *lock = NULL;
static SDL_cond *req_done = NULL;     /* emulator -> client */

static char req_line[MAX_LINE];       /* guarded by lock */
static char rep_line[MAX_REPLY];
static volatile int req_pending = 0;
static volatile int rep_ready = 0;

/* Frame gating. frames_left < 0 means free running. */
static volatile int frames_left = -1;
static unsigned long frame_count = 0;

/* Pad overrides. Held state, plus a per-button countdown for timed taps. */
static Uint8 ctl_held[2][GN_MAX_KEY];
static int ctl_timer[2][GN_MAX_KEY];
/* What we forced last frame. joy_state is only ever assigned from SDL events,
   so a bit we set stays set for good unless we take it back ourselves. */
static Uint8 ctl_applied[2][GN_MAX_KEY];

/* ------------------------------------------------------------- name table */

struct btn_name { const char *name; GNGEO_BUTTON id; };

static const struct btn_name BUTTONS[] = {
	{ "UP", GN_UP }, { "DOWN", GN_DOWN }, { "LEFT", GN_LEFT },
	{ "RIGHT", GN_RIGHT }, { "A", GN_A }, { "B", GN_B }, { "C", GN_C },
	{ "D", GN_D }, { "START", GN_START }, { "COIN", GN_SELECT_COIN },
	{ NULL, GN_NONE }
};

static int button_by_name(const char *s, GNGEO_BUTTON *out) {
	int i;
	for (i = 0; BUTTONS[i].name; i++) {
		if (SDL_strcasecmp(s, BUTTONS[i].name) == 0) {
			*out = BUTTONS[i].id;
			return 1;
		}
	}
	return 0;
}

/* Parse "A" or "A,B,RIGHT" into a list of button ids. */
static int parse_buttons(char *spec, GNGEO_BUTTON *ids, int max, int *count) {
	char *tok, *save = NULL;
	*count = 0;
	for (tok = SDL_strtokr(spec, ",+", &save); tok;
	     tok = SDL_strtokr(NULL, ",+", &save)) {
		if (*count >= max) return 0;
		if (!button_by_name(tok, &ids[*count])) return 0;
		(*count)++;
	}
	return *count > 0;
}

static int parse_player(const char *s, int *out) {
	if (!s) return 0;
	if (!strcmp(s, "1") || !SDL_strcasecmp(s, "p1")) { *out = 0; return 1; }
	if (!strcmp(s, "2") || !SDL_strcasecmp(s, "p2")) { *out = 1; return 1; }
	return 0;
}

/* --------------------------------------------------------- pad injection */

/* Push our held state into the array the input layer builds the hardware
   registers from. Done every frame, because a real key event would otherwise
   clear it. */
static void apply_pad(void) {
	int p, b;
	for (p = 0; p < 2; p++)
		for (b = 0; b < GN_MAX_KEY; b++) {
			/* Release what we forced before setting this frame's
			   state, or a button we let go of would stay down. A
			   key the user is physically holding is cleared too:
			   while the socket drives a pad, it owns it. */
			if (ctl_applied[p][b] && !ctl_held[p][b])
				joy_state[p][b] = 0;
			if (ctl_held[p][b])
				joy_state[p][b] = 1;
			ctl_applied[p][b] = ctl_held[p][b];
		}
}

/* Age the timed taps by one frame, releasing the ones that have run out. */
static void tick_pad(void) {
	int p, b;
	for (p = 0; p < 2; p++)
		for (b = 0; b < GN_MAX_KEY; b++)
			if (ctl_timer[p][b] > 0 && --ctl_timer[p][b] == 0)
				ctl_held[p][b] = 0;
}

/* ------------------------------------------------------------- screenshot */

/* The same blit take_screenshot() uses, to a path the caller names. BMP
   because that is what SDL writes without another library; the client
   converts. */
static int grab_screen(const char *path) {
	SDL_Rect dst = { 0, 0, 304, 224 };
	SDL_Surface *shoot;
	int rc;

	if (!buffer) return 0;
	dst.w = visible_area.w;
	dst.h = visible_area.h;

	shoot = SDL_CreateRGBSurface(SDL_SWSURFACE, visible_area.w,
				     visible_area.h, 16, 0xF800, 0x7E0, 0x1F, 0);
	if (!shoot) return 0;

	SDL_BlitSurface(buffer, &visible_area, shoot, &dst);
	rc = (SDL_SaveBMP(shoot, path) == 0);
	SDL_FreeSurface(shoot);
	return rc;
}

/* The same frame as the window shows it, through the GLSL blitter's shader
   passes -- the CRT scanlines and all. Only that blitter has passes to run:
   1 on success, 0 on a failed grab, -1 when some other blitter is up. */
static int grab_window(const char *path) {
#if (defined(HAVE_GL_GL_H) || defined(HAVE_OPENGL_GL_H)) && defined(USE_GLSL)
	if (blitter[nblitter].update == blitter_glsl_update)
		return blitter_glsl_grab(path);
#endif
	return -1;
}


/* ------------------------------------------------------------------ audio
 *
 * Two producers write here and neither may block: sound commands come from
 * the emulation thread, ADPCM key-ons and the PCM tap from SDL's audio
 * callback thread. So each producer gets a ring of its own, written by it
 * alone and drained by the emulation thread from the gate -- single producer,
 * single consumer, no lock on the audio path.
 */

#define AUD_RING 256

typedef struct {
	unsigned long frame;
	int kind;                 /* 0 command, 1 adpcm-a, 2 adpcm-b */
	unsigned int a, b, c;
} AudEvent;

static AudEvent cmd_ring[AUD_RING];
static volatile unsigned cmd_head = 0, cmd_tail = 0;

static AudEvent snd_ring[AUD_RING];
static volatile unsigned snd_head = 0, snd_tail = 0;

/* Peak and sum-of-squares of everything mixed since the last "level" query,
   which is how a client tells silence from sound without moving the samples. */
static volatile int lvl_peak = 0;
static volatile double lvl_sumsq = 0.0;
static volatile unsigned long lvl_samples = 0;

/* WAV capture. The audio thread fills a preallocated buffer and never touches
   the filesystem; the emulation thread writes the file when capture ends. */
static Sint16 *wav_buf = NULL;
static volatile unsigned long wav_cap = 0;    /* capacity in samples */
static volatile unsigned long wav_used = 0;
static volatile int wav_active = 0;
static char wav_path[512];

/* Frame-locked WAV capture ("wav <path> sync"). The capture above records
   whatever SDL's callback pulled, in real time -- under frame stepping that
   is a chip running on the wall clock, so a recording made while a harness
   steps the emulator has the sound in the wrong places and gaps between. This
   one owns the mixer instead and renders exactly one frame of samples at the
   end of every emulated frame, from the emulation thread, however long the
   frame took in real time. It streams to the file as it goes, so it has no
   length cap, and patches the header when it stops. The fractional sample is
   carried over, or 5000 frames of 745 where 745.1 was due drifts by a third
   of a second. */
static FILE *sync_file = NULL;
static char sync_path[512];
static double sync_per_frame = 0.0;   /* sample frames per emulated frame */
static double sync_owed = 0.0;
static unsigned long sync_samples = 0; /* stereo sample frames written */
static unsigned long sync_frames = 0;  /* emulated frames covered */
static Sint16 sync_buf[2 * 8192];

static void ring_put(AudEvent *ring, volatile unsigned *head,
		     volatile unsigned tail, AudEvent *ev) {
	unsigned h = *head;
	unsigned next = (h + 1) % AUD_RING;
	if (next == tail) return;   /* full: drop, never block a producer */
	ring[h] = *ev;
	*head = next;
}

void control_note_command(unsigned char cmd) {
	AudEvent ev;
	if (!enabled) return;
	ev.frame = frame_count;
	ev.kind = 0;
	ev.a = cmd; ev.b = 0; ev.c = 0;
	ring_put(cmd_ring, &cmd_head, cmd_tail, &ev);
}

void control_note_adpcma(int channel, unsigned int start, unsigned int end) {
	AudEvent ev;
	if (!enabled) return;
	ev.frame = frame_count;
	ev.kind = 1;
	ev.a = (unsigned)channel; ev.b = start; ev.c = end;
	ring_put(snd_ring, &snd_head, snd_tail, &ev);
}

void control_note_adpcmb(unsigned int start, unsigned int end, int repeat) {
	AudEvent ev;
	if (!enabled) return;
	ev.frame = frame_count;
	ev.kind = 2;
	ev.a = (unsigned)repeat; ev.b = start; ev.c = end;
	ring_put(snd_ring, &snd_head, snd_tail, &ev);
}

void control_audio_tap(const void *stream, int bytes) {
	const Sint16 *s = (const Sint16 *)stream;
	int n = bytes / 2, i;
	int peak = lvl_peak;
	double sumsq = lvl_sumsq;

	if (!enabled) return;

	for (i = 0; i < n; i++) {
		int v = s[i];
		if (v < 0) v = -v;
		if (v > peak) peak = v;
		sumsq += (double)s[i] * (double)s[i];
	}
	lvl_peak = peak;
	lvl_sumsq = sumsq;
	lvl_samples += n;

	if (wav_active && wav_buf) {
		unsigned long room = wav_cap - wav_used;
		unsigned long take = (unsigned long)n < room ? (unsigned long)n : room;
		if (take) {
			memcpy(wav_buf + wav_used, s, take * sizeof(Sint16));
			wav_used += take;
		}
		if (wav_used >= wav_cap) wav_active = 0;   /* full: stop, keep it */
	}
}

/* The 44-byte header of a 16-bit stereo PCM WAV holding data_bytes. */
static void wav_header(unsigned char *hdr, unsigned long data_bytes) {
	unsigned int rate = conf.sample_rate ? conf.sample_rate : 44100;
	unsigned int u32;
	unsigned short u16;

#define PUT32(off, val) do { u32 = (unsigned int)(val); \
	hdr[off] = u32 & 0xff; hdr[(off)+1] = (u32 >> 8) & 0xff; \
	hdr[(off)+2] = (u32 >> 16) & 0xff; hdr[(off)+3] = (u32 >> 24) & 0xff; } while (0)
#define PUT16(off, val) do { u16 = (unsigned short)(val); \
	hdr[off] = u16 & 0xff; hdr[(off)+1] = (u16 >> 8) & 0xff; } while (0)

	memcpy(hdr, "RIFF", 4);
	PUT32(4, 36 + data_bytes);
	memcpy(hdr + 8, "WAVEfmt ", 8);
	PUT32(16, 16);          /* fmt chunk size */
	PUT16(20, 1);           /* PCM */
	PUT16(22, 2);           /* stereo */
	PUT32(24, rate);
	PUT32(28, rate * 2 * 2);/* byte rate */
	PUT16(32, 4);           /* block align */
	PUT16(34, 16);          /* bits */
	memcpy(hdr + 36, "data", 4);
	PUT32(40, data_bytes);
#undef PUT32
#undef PUT16
}

/* Write what was captured as a 16-bit stereo WAV. Emulation thread. */
static int wav_flush(void) {
	FILE *f;
	unsigned long data_bytes = wav_used * sizeof(Sint16);
	unsigned char hdr[44];

	if (!wav_buf || wav_used == 0) return 0;
	f = fopen(wav_path, "wb");
	if (!f) return 0;
	wav_header(hdr, data_bytes);
	fwrite(hdr, 1, 44, f);
	fwrite(wav_buf, 1, data_bytes, f);
	fclose(f);
	return 1;
}

/* One emulated frame of the frame-locked capture. Emulation thread, at the
   end of the frame, once the Z80 and the 68000 have both run it. */
static void sync_frame(void) {
	int n;

	sync_owed += sync_per_frame;
	n = (int)sync_owed;
	sync_owed -= n;
	if (n > 8192) n = 8192;
	sound_render(sync_buf, n);
	fwrite(sync_buf, 4, n, sync_file);
	sync_samples += n;
	sync_frames++;
	control_audio_tap(sync_buf, n * 4);   /* keeps "level" honest */
}

/* Start the frame-locked capture. Returns 0 and fills why on failure. */
static int sync_start(const char *path, double fps, char *why, int whylen) {
	unsigned char hdr[44];

	if (!conf.sound) {
		SDL_strlcpy(why, "ERR sound is off in this emulator", whylen);
		return 0;
	}
	if (sync_file) {
		SDL_strlcpy(why, "ERR a sync capture is already running", whylen);
		return 0;
	}
	sync_file = fopen(path, "wb");
	if (!sync_file) {
		SDL_snprintf(why, whylen, "ERR cannot open %s", path);
		return 0;
	}
	wav_header(hdr, 0);        /* sizes patched by sync_stop */
	fwrite(hdr, 1, 44, sync_file);
	SDL_strlcpy(sync_path, path, sizeof(sync_path));
	sync_per_frame = (double)(conf.sample_rate ? conf.sample_rate : 44100) / fps;
	sync_owed = 0.0;
	sync_samples = sync_frames = 0;
	sound_own_mixer(1);
	return 1;
}

/* Stop it: give the mixer back and patch the header. */
static void sync_stop(void) {
	unsigned char hdr[44];

	if (!sync_file) return;
	sound_own_mixer(0);
	wav_header(hdr, sync_samples * 4);
	fseek(sync_file, 0, SEEK_SET);
	fwrite(hdr, 1, 44, sync_file);
	fclose(sync_file);
	sync_file = NULL;
}

/* Drain both rings into the reply. Emulation thread. */
static void audio_report(char *out, int outlen) {
	int used = SDL_snprintf(out, outlen, "OK");
	unsigned t;

	for (t = cmd_tail; t != cmd_head && used < outlen - 64; t = (t + 1) % AUD_RING) {
		AudEvent *e = &cmd_ring[t];
		used += SDL_snprintf(out + used, outlen - used,
				     " cmd:%lu:%02x", e->frame, e->a);
	}
	cmd_tail = cmd_head;

	for (t = snd_tail; t != snd_head && used < outlen - 64; t = (t + 1) % AUD_RING) {
		AudEvent *e = &snd_ring[t];
		if (e->kind == 1)
			used += SDL_snprintf(out + used, outlen - used,
					     " a%u:%lu:%06x-%06x",
					     e->a, e->frame, e->b, e->c);
		else
			used += SDL_snprintf(out + used, outlen - used,
					     " b:%lu:%06x-%06x:rep%u",
					     e->frame, e->b, e->c, e->a);
	}
	snd_tail = snd_head;
}

/* ------------------------------------------------------------- command run */

/* Runs on the emulation thread, with the lock held. Fills rep_line. */
static void execute(char *line) {
	char *argv[8];
	int argc = 0;
	char *tok, *save = NULL;

	for (tok = SDL_strtokr(line, " \t", &save);
	     tok && argc < 8; tok = SDL_strtokr(NULL, " \t", &save))
		argv[argc++] = tok;

	if (argc == 0) {
		SDL_strlcpy(rep_line, "ERR empty", MAX_REPLY);
		return;
	}

	if (!SDL_strcasecmp(argv[0], "ping")) {
		SDL_strlcpy(rep_line, "OK pong", MAX_REPLY);

	} else if (!SDL_strcasecmp(argv[0], "frame")) {
		/* Three states, not two: a client waiting for a step to finish
		   has to be able to tell "still stepping" from "stopped". */
		SDL_snprintf(rep_line, MAX_REPLY, "OK %lu %s %d", frame_count,
			     frames_left < 0 ? "running" :
			     (frames_left > 0 ? "stepping" : "paused"),
			     frames_left);

	} else if (!SDL_strcasecmp(argv[0], "pause")) {
		frames_left = 0;
		SDL_strlcpy(rep_line, "OK paused", MAX_REPLY);

	} else if (!SDL_strcasecmp(argv[0], "run")) {
		frames_left = -1;
		SDL_strlcpy(rep_line, "OK running", MAX_REPLY);

	} else if (!SDL_strcasecmp(argv[0], "step")) {
		int n = (argc > 1) ? atoi(argv[1]) : 1;
		if (n < 1) n = 1;
		frames_left = n;
		SDL_snprintf(rep_line, MAX_REPLY, "OK step %d", n);

	} else if (!SDL_strcasecmp(argv[0], "hold") ||
		   !SDL_strcasecmp(argv[0], "release") ||
		   !SDL_strcasecmp(argv[0], "press")) {
		GNGEO_BUTTON ids[GN_MAX_KEY];
		int n = 0, p = 0, i;
		int frames = (argc > 3) ? atoi(argv[3]) : 2;

		if (argc < 3 || !parse_player(argv[1], &p)) {
			SDL_strlcpy(rep_line,
				    "ERR usage: <cmd> <1|2> <BTN[,BTN]> [frames]",
				    MAX_REPLY);
		} else if (!parse_buttons(argv[2], ids, GN_MAX_KEY, &n)) {
			SDL_strlcpy(rep_line, "ERR unknown button", MAX_REPLY);
		} else {
			int rel = !SDL_strcasecmp(argv[0], "release");
			int timed = !SDL_strcasecmp(argv[0], "press");
			if (frames < 1) frames = 1;
			for (i = 0; i < n; i++) {
				ctl_held[p][ids[i]] = rel ? 0 : 1;
				ctl_timer[p][ids[i]] = timed ? frames : 0;
			}
			SDL_snprintf(rep_line, MAX_REPLY, "OK %s %d", argv[0], n);
		}

	} else if (!SDL_strcasecmp(argv[0], "clear")) {
		int p, b;
		for (p = 0; p < 2; p++)
			for (b = 0; b < GN_MAX_KEY; b++) {
				ctl_held[p][b] = 0;
				ctl_timer[p][b] = 0;
			}
		SDL_strlcpy(rep_line, "OK clear", MAX_REPLY);

	} else if (!SDL_strcasecmp(argv[0], "pad")) {
		/* What the 68000 will actually read, so a client can tell a
		   dropped input from a game that ignored it. */
		SDL_snprintf(rep_line, MAX_REPLY,
			     "OK p1=%02x p2=%02x start=%02x coin=%02x",
			     memory.intern_p1, memory.intern_p2,
			     memory.intern_start, memory.intern_coin);

	} else if (!SDL_strcasecmp(argv[0], "shot")) {
		/* shot <path>         the framebuffer, 320x224, as the game drew it
		   shot <path> shader  the window's picture, through the shader */
		int shader = argc > 2 && !SDL_strcasecmp(argv[2], "shader");
		int rc;
		if (argc < 2 || (argc > 2 && !shader))
			SDL_strlcpy(rep_line, "ERR usage: shot <path> [shader]",
				    MAX_REPLY);
		else if ((rc = shader ? grab_window(argv[1])
				      : grab_screen(argv[1])) > 0)
			SDL_snprintf(rep_line, MAX_REPLY, "OK %s", argv[1]);
		else if (rc < 0)
			SDL_strlcpy(rep_line, "ERR no shader: the emulator is "
				    "not running the glsl blitter", MAX_REPLY);
		else
			SDL_strlcpy(rep_line, "ERR screenshot failed", MAX_REPLY);

	} else if (!SDL_strcasecmp(argv[0], "peek")) {
		/* 68000 work RAM is 0x100000-0x10FFFF; the low 16 bits are
		   taken, so either form of the address works. READ_BYTE_ROM
		   undoes the core's byte swapping, so bytes come back in
		   68000 order. */
		unsigned long addr, len, i;
		char *end;
		int used;

		if (argc < 3) {
			SDL_strlcpy(rep_line, "ERR usage: peek <addr> <len>", MAX_REPLY);
		} else {
			addr = strtoul(argv[1], &end, 0);
			len = strtoul(argv[2], &end, 0);
			if (len == 0 || len > 2048) {
				SDL_strlcpy(rep_line, "ERR len must be 1..2048", MAX_REPLY);
			} else {
				used = SDL_snprintf(rep_line, MAX_REPLY, "OK ");
				for (i = 0; i < len && used < MAX_REPLY - 3; i++) {
					Uint32 a = (Uint32)((addr + i) & 0xffff);
					Uint8 v = READ_BYTE_ROM(memory.ram + a);
					used += SDL_snprintf(rep_line + used,
							     MAX_REPLY - used,
							     "%02x", v);
				}
			}
		}

	} else if (!SDL_strcasecmp(argv[0], "poke")) {
		/* Write work RAM, through the same accessor the 68000 stores
		   with. Handy for forcing a state a bug needs, and it is what
		   makes a peek/poke round trip a proof that both agree with
		   the CPU about where a byte lives. */
		unsigned long addr, i;
		char *hex, *end;
		size_t n;

		if (argc < 3) {
			SDL_strlcpy(rep_line, "ERR usage: poke <addr> <hexbytes>", MAX_REPLY);
		} else {
			addr = strtoul(argv[1], &end, 0);
			hex = argv[2];
			n = strlen(hex);
			if (n == 0 || (n & 1) || n > 2048) {
				SDL_strlcpy(rep_line, "ERR need an even number of hex digits", MAX_REPLY);
			} else {
				for (i = 0; i < n / 2; i++) {
					char pair[3];
					unsigned long v;
					Uint32 a2 = (Uint32)((addr + i) & 0xffff);
					pair[0] = hex[i * 2];
					pair[1] = hex[i * 2 + 1];
					pair[2] = 0;
					v = strtoul(pair, &end, 16);
					WRITE_BYTE_ROM(memory.ram + a2, (Uint8)v);
				}
				SDL_snprintf(rep_line, MAX_REPLY, "OK poke %lu", (unsigned long)(n / 2));
			}
		}

	} else if (!SDL_strcasecmp(argv[0], "audio")) {
		/* Everything asked for and everything started since last drained,
		   each stamped with the frame it happened on. */
		audio_report(rep_line, MAX_REPLY);

	} else if (!SDL_strcasecmp(argv[0], "level")) {
		/* Peak and RMS of the mix since the last call, as 0..32767.
		   Zero peak over a window that should have had sound is the
		   signature of audio that was asked for but never came out. */
		double rms = lvl_samples ? SDL_sqrt(lvl_sumsq / (double)lvl_samples) : 0.0;
		SDL_snprintf(rep_line, MAX_REPLY, "OK peak=%d rms=%d samples=%lu",
			     lvl_peak, (int)rms, lvl_samples);
		lvl_peak = 0;
		lvl_sumsq = 0.0;
		lvl_samples = 0;

	} else if (!SDL_strcasecmp(argv[0], "wav")) {
		if (argc > 1 && !SDL_strcasecmp(argv[1], "stop") && sync_file) {
			unsigned long n = sync_samples, fr = sync_frames;
			sync_stop();
			SDL_snprintf(rep_line, MAX_REPLY, "OK %s %lu samples %lu frames",
				     sync_path, n, fr);
		} else if (argc > 2 && !SDL_strcasecmp(argv[2], "sync")) {
			/* Frame-locked: one emulated frame of sound per emulated
			   frame, whatever the wall clock does. fps is the rate the
			   frames are meant to be played back at -- the Neo Geo's own
			   59.185606 unless a video is being cut to something else. */
			double fps = (argc > 3) ? SDL_atof(argv[3]) : 59.185606;
			if (fps < 1.0 || fps > 1000.0) fps = 59.185606;
			if (sync_start(argv[1], fps, rep_line, MAX_REPLY))
				SDL_snprintf(rep_line, MAX_REPLY,
					     "OK sync capture to %s: %d Hz, %.3f samples a frame",
					     sync_path, conf.sample_rate, sync_per_frame);
		} else if (argc > 1 && !SDL_strcasecmp(argv[1], "stop")) {
			wav_active = 0;
			if (wav_flush())
				SDL_snprintf(rep_line, MAX_REPLY, "OK %s %lu samples",
					     wav_path, wav_used / 2);
			else
				SDL_strlcpy(rep_line, "ERR nothing captured", MAX_REPLY);
		} else if (argc < 2) {
			SDL_strlcpy(rep_line, "ERR usage: wav <path> [seconds] | wav <path> sync [fps] | wav stop", MAX_REPLY);
		} else {
			int secs = (argc > 2) ? atoi(argv[2]) : 5;
			unsigned int rate = conf.sample_rate ? conf.sample_rate : 44100;
			unsigned long want;

			if (secs < 1) secs = 1;
			if (secs > 60) secs = 60;
			want = (unsigned long)rate * 2 * (unsigned long)secs;

			wav_active = 0;              /* stop before reallocating */
			if (wav_buf) { free(wav_buf); wav_buf = NULL; }
			wav_buf = (Sint16 *)malloc(want * sizeof(Sint16));
			if (!wav_buf) {
				wav_cap = 0;
				SDL_strlcpy(rep_line, "ERR out of memory", MAX_REPLY);
			} else {
				SDL_strlcpy(wav_path, argv[1], sizeof(wav_path));
				wav_cap = want;
				wav_used = 0;
				wav_active = 1;
				SDL_snprintf(rep_line, MAX_REPLY, "OK capturing %ds to %s",
					     secs, wav_path);
			}
		}

	} else if (!SDL_strcasecmp(argv[0], "reset")) {
		cpu_68k_reset();
		SDL_strlcpy(rep_line, "OK reset", MAX_REPLY);

	} else {
		SDL_snprintf(rep_line, MAX_REPLY, "ERR unknown command '%s'", argv[0]);
	}
}

/* Service one parked request, if there is one. Emulation thread, lock held. */
static void service_locked(void) {
	if (!req_pending) return;
	execute(req_line);
	req_pending = 0;
	rep_ready = 1;
	SDL_CondSignal(req_done);
}

/* --------------------------------------------------------------- the gate */

void control_frame_begin(void) {
	if (!enabled) return;

	for (;;) {
		SDL_LockMutex(lock);
		service_locked();
		if (frames_left != 0) {
			if (frames_left > 0) frames_left--;
			SDL_UnlockMutex(lock);
			break;
		}
		SDL_UnlockMutex(lock);

		/* Paused. Keep pumping SDL or the window stops responding, and
		   wait a little so a paused emulator does not spin a core. */
		handle_event();
		SDL_Delay(2);

		if (stopping) return;
	}

	SDL_LockMutex(lock);
	apply_pad();
	SDL_UnlockMutex(lock);
}

void control_frame_end(void) {
	if (!enabled) return;
	/* Before the requests: a sync capture asked for at this gate starts
	   with the next frame, the same as one asked for at the top of it. */
	if (sync_file) sync_frame();
	SDL_LockMutex(lock);
	frame_count++;
	/* Aged here rather than before the frame, so a button asked for over
	   N frames is actually down for N of them. */
	tick_pad();
	service_locked();
	SDL_UnlockMutex(lock);
}

/* ------------------------------------------------------------ the listener */

static int send_all(SOCKET s, const char *buf, int len) {
	int sent = 0;
	while (sent < len) {
		int n = send(s, buf + sent, len - sent, 0);
		if (n <= 0) return 0;
		sent += n;
	}
	return 1;
}

/* Post a line to the emulation thread and wait for its reply. */
static void dispatch(const char *line, char *out, int outlen) {
	SDL_LockMutex(lock);
	SDL_strlcpy(req_line, line, MAX_LINE);
	rep_ready = 0;
	req_pending = 1;
	while (!rep_ready && !stopping)
		SDL_CondWaitTimeout(req_done, lock, 100);
	SDL_strlcpy(out, rep_ready ? rep_line : "ERR emulator not responding", outlen);
	rep_ready = 0;
	SDL_UnlockMutex(lock);
}

static void serve_client(SOCKET c) {
	char in[MAX_LINE];
	char reply[MAX_REPLY + 2];
	int used = 0;

	client_sock = c;
	in[0] = 0;
	for (;;) {
		int n = recv(c, in + used, (int)sizeof(in) - used - 1, 0);
		char *nl;
		if (n <= 0) { client_sock = INVALID_SOCKET; return; }
		used += n;
		in[used] = 0;

		while ((nl = strchr(in, '\n')) != NULL) {
			int linelen = (int)(nl - in);
			char line[MAX_LINE];

			if (linelen > 0 && in[linelen - 1] == '\r') linelen--;
			memcpy(line, in, linelen);
			line[linelen] = 0;

			/* shuffle the remainder down */
			used -= (int)(nl - in) + 1;
			memmove(in, nl + 1, used);
			in[used] = 0;

			if (!SDL_strcasecmp(line, "quit")) return;

			dispatch(line, reply, MAX_REPLY);
			SDL_strlcat(reply, "\n", sizeof(reply));
			if (!send_all(c, reply, (int)strlen(reply))) return;
			if (stopping) return;
		}

		if (used >= (int)sizeof(in) - 1) used = 0; /* overlong line: drop */
	}
}

static int listener(void *unused) {
	(void)unused;
	while (!stopping) {
		SOCKET c = accept(listen_sock, NULL, NULL);
		if (c == INVALID_SOCKET) {
			if (stopping) break;
			SDL_Delay(50);
			continue;
		}
		serve_client(c);
		CLOSESOCK(c);
	}
	return 0;
}

/* ------------------------------------------------------------------- setup */

void control_init(void) {
	const char *port_s = SDL_getenv("GNGEO_CONTROL_PORT");
	struct sockaddr_in addr;
	int port, yes = 1;
	int p, b;

	if (!port_s || !*port_s) return;
	port = atoi(port_s);
	if (port <= 0 || port > 65535) {
		printf("control: bad GNGEO_CONTROL_PORT '%s', disabled\n", port_s);
		return;
	}

#ifdef _WIN32
	{
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
			printf("control: WSAStartup failed, disabled\n");
			return;
		}
	}
#endif

	listen_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_sock == INVALID_SOCKET) {
		printf("control: socket() failed, disabled\n");
		return;
	}
	setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR,
		   (const char *)&yes, sizeof(yes));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short)port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* local only */

	if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
	    listen(listen_sock, 1) != 0) {
		printf("control: cannot listen on port %d, disabled\n", port);
		CLOSESOCK(listen_sock);
		listen_sock = INVALID_SOCKET;
		return;
	}

	lock = SDL_CreateMutex();
	req_done = SDL_CreateCond();

	for (p = 0; p < 2; p++)
		for (b = 0; b < GN_MAX_KEY; b++) {
			ctl_held[p][b] = 0;
			ctl_timer[p][b] = 0;
		}

	/* Starting paused lets a client attach and set up before frame 1. */
	frames_left = SDL_getenv("GNGEO_CONTROL_PAUSED") ? 0 : -1;

	enabled = 1;
	listen_thread = SDL_CreateThread(listener, "gngeo_control", NULL);
	printf("control: listening on 127.0.0.1:%d%s\n", port,
	       frames_left == 0 ? " (paused)" : "");
	fflush(stdout);
}

void control_shutdown(void) {
	if (!enabled) return;
	stopping = 1;
	/* A capture still running when the window closes is still a file
	   somebody wanted: finish its header rather than leave it unreadable. */
	sync_stop();
	/* Tell the client the emulator is going, before the socket drops under
	   it. Best effort and unacknowledged: if the window was closed the peer
	   may already be gone, and there is nothing useful to do about that. */
	if (client_sock != INVALID_SOCKET) {
		static const char bye[] = "BYE emulator closing\n";
		send_all(client_sock, bye, (int)sizeof(bye) - 1);
		client_sock = INVALID_SOCKET;
	}
	if (listen_sock != INVALID_SOCKET) {
		CLOSESOCK(listen_sock);
		listen_sock = INVALID_SOCKET;
	}
	enabled = 0;
}
