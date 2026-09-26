/*  gngeo a neogeo emulator
 *  Copyright (C) 2001 Peponas Mathieu
 * 
 *  This program is free software; you can redistribute it and/or modify  
 *  it under the terms of the GNU General Public License as published by   
 *  the Free Software Foundation; either version 2 of the License, or    
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA. 
 */  
    
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include "SDL.h"
#include "sound.h"
#include "control.h"
#include "conf.h"
#include "emu.h"
#include "memory.h"
#include "profiler.h"
#include "gnutil.h"
#include "ym2610/ym2610.h"
#ifdef GP2X
#include "ym2610-940/940shared.h"
#endif

#ifdef USE_OSS
#include <sys/soundcard.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <pthread.h>

#endif

SDL_AudioSpec * desired, *obtain;
/* non-zero when playback was opened on a device named by the "audiodevice"
   config item; zero means the legacy default-device API is in use */
static SDL_AudioDeviceID audio_device = 0;

#define MIXER_MAX_CHANNELS 16
//#define CPU_FPS 60
#define BUFFER_LEN 16384
//#define BUFFER_LEN 
//extern int throttle;
static int audio_sample_rate;
Uint16 play_buffer[BUFFER_LEN];

#ifndef GP2X
#define NB_SAMPLES 512 /* better resolution */
#else
//#define NB_SAMPLES 128
#define NB_SAMPLES 64
//#define NB_SAMPLES 512
#endif

/* Set while the control socket's frame-locked WAV capture owns the mixer.
   The YM2610 is rendered on demand, and normally SDL's audio thread is what
   demands it, in real time -- which under frame stepping means the chip runs
   on the wall clock rather than the emulated one. So the capture takes it
   over: the emulation thread renders exactly one frame's worth of samples at
   the end of every frame, and the callback plays silence meanwhile rather
   than advance the same chip a second time. */
static volatile int mixer_owned = 0;

/* Render `frames` stereo sample frames into out. Only the owner of the mixer
   may call it: the emulation thread, while sound_own_mixer(1) holds. */
void sound_render(Sint16 *out, int frames)
{
	while (frames > 0) {
		int n = frames > 4096 ? 4096 : frames;
		YM2610Update_stream(n);
		memcpy(out, play_buffer, n * 4);
		out += n * 2;
		frames -= n;
	}
}

#ifndef USE_OSS

void update_sdl_stream(void *userdata, Uint8 * stream, int len)
{
#ifdef ENABLE_940T
	static Uint32 play_buffer_pos;
#endif

	PROFILER_START(PROF_SOUND);
	//streamupdate(len);
	if (mixer_owned) {
		memset(stream, 0, len);
		PROFILER_STOP(PROF_SOUND);
		return;
	}
	
#ifdef ENABLE_940T
	if (shared_ctl->buf_pos >= play_buffer_pos && 
	    shared_ctl->buf_pos <= play_buffer_pos + len) {
		//printf("SOUND WARN 1 %d %d\n",shared_ctl->buf_pos,play_buffer_pos);
		
		return;
	}
	if (shared_ctl->buf_pos + shared_ctl->sample_len >= play_buffer_pos && 
	    shared_ctl->buf_pos + shared_ctl->sample_len <= play_buffer_pos + len) {
		//printf("SOUND WARN 2 %d %d\n",shared_ctl->buf_pos,play_buffer_pos);
		
		return;
	}
	if ( play_buffer_pos+len>SAMPLE_BUFLEN) {
		unsigned int last=(SAMPLE_BUFLEN-play_buffer_pos);
		memcpy(stream, (Uint8 *) shared_ctl->play_buffer+ play_buffer_pos, last);
		memcpy(stream+last, (Uint8 *) shared_ctl->play_buffer, len-last);
		//printf("Case 1\n");
		play_buffer_pos=len-last;
	} else {
		memcpy(stream, (Uint8 *) shared_ctl->play_buffer+play_buffer_pos, len);
		play_buffer_pos+=len;
		//printf("Case 2\n");
	}


#else

	YM2610Update_stream(len/4);
	memcpy(stream, (Uint8 *) play_buffer, len);

#endif
	control_audio_tap(stream, len);

#ifdef GNGEO_AUDIO_DEBUG
	{
		/* report the loudest sample seen, once per ~1s of audio */
		static int dbg_calls = 0;
		static int dbg_peak = 0;
		static double dbg_sumsq = 0.0;
		static long dbg_total = 0, dbg_nonzero = 0;
		Sint16 *dbg_s = (Sint16 *)stream;
		int dbg_n = len / 2;
		for (int dbg_i = 0; dbg_i < dbg_n; dbg_i++) {
			int v = dbg_s[dbg_i] < 0 ? -dbg_s[dbg_i] : dbg_s[dbg_i];
			if (v > dbg_peak) dbg_peak = v;
			dbg_sumsq += (double)v * v;
			if (v) dbg_nonzero++;
			dbg_total++;
		}
		if (++dbg_calls >= 40) {
			double rms = dbg_total ? sqrt(dbg_sumsq / dbg_total) : 0.0;
			fprintf(stderr, "[audio] peak=%5d rms=%8.1f nonzero=%.1f%% (%ld samples)\n",
			        dbg_peak, rms, dbg_total ? 100.0 * dbg_nonzero / dbg_total : 0.0,
			        dbg_total);
			fflush(stderr);
			dbg_calls = 0;
			dbg_peak = 0;
			dbg_sumsq = 0.0;
			dbg_total = dbg_nonzero = 0;
		}
	}
#endif
	PROFILER_STOP(PROF_SOUND);

}
void dummy_stream(void *userdata, Uint8 * stream, int len) {
}
static int sound_initialize=GN_FALSE;

int init_sdl_audio(void)
{
	if (sound_initialize==GN_TRUE)
		close_sdl_audio();
	sound_initialize=GN_TRUE;
    SDL_InitSubSystem(SDL_INIT_AUDIO);

    desired = (SDL_AudioSpec *) malloc(sizeof(SDL_AudioSpec));
    obtain = (SDL_AudioSpec *) malloc(sizeof(SDL_AudioSpec));
    audio_sample_rate = conf.sample_rate;
    desired->freq = conf.sample_rate;
    desired->samples = NB_SAMPLES;
    
#ifdef WORDS_BIGENDIAN
    desired->format = AUDIO_S16MSB;
#else	/* */
    desired->format = AUDIO_S16;
#endif	/* */
    desired->channels = 2;
    desired->callback = update_sdl_stream;
	 
	//desired->callback = NULL;
    desired->userdata = NULL;
    //SDL_OpenAudio(desired, NULL);

    /* "audiodevice" lets the user pin playback to a named endpoint instead of
       following the Windows default, which per-app routing can send elsewhere.
       Empty (the default) keeps the legacy behaviour. */
    {
        char *devname = CF_STR(cf_get_item_by_name("audiodevice"));
        int ndev = SDL_GetNumAudioDevices(0);
        printf("audio devices available: %d\n", ndev);
        for (int i = 0; i < ndev; i++)
            printf("  [%d] %s\n", i, SDL_GetAudioDeviceName(i, 0));

        if (devname && devname[0]) {
            /* allowed_changes=0 => SDL converts to our S16 spec rather than
               changing it under us */
            audio_device = SDL_OpenAudioDevice(devname, 0, desired, obtain, 0);
            if (audio_device == 0)
                printf("could not open audio device \"%s\": %s\n", devname, SDL_GetError());
            else
                printf("using audio device \"%s\"\n", devname);
        }
        if (audio_device == 0) {
            /* obtained MUST be NULL here: with a non-NULL obtained spec, SDL is
               free to hand back a different format (F32 on WASAPI), and the
               mixer's S16 samples then get read as denormal floats == silence.
               NULL makes SDL convert to the requested S16 instead. */
            SDL_OpenAudio(desired, NULL);
            *obtain = *desired;
            printf("using the default audio device\n");
        }
    }
#ifdef GNGEO_AUDIO_DEBUG
    fprintf(stderr, "[audio] desired: freq=%d fmt=0x%04x ch=%d samples=%d\n",
            desired->freq, desired->format, desired->channels, desired->samples);
    fprintf(stderr, "[audio] obtained: freq=%d fmt=0x%04x ch=%d samples=%d size=%u\n",
            obtain->freq, obtain->format, obtain->channels, obtain->samples, obtain->size);
    fflush(stderr);
#endif
    printf("Obtained sample rate: %d\n",obtain->freq);
    conf.sample_rate=obtain->freq;
    return GN_TRUE;
}

void close_sdl_audio(void) {
	sound_initialize=GN_FALSE;
    if (audio_device) {
        SDL_PauseAudioDevice(audio_device, 1);
        SDL_CloseAudioDevice(audio_device);
        audio_device = 0;
    } else {
        SDL_PauseAudio(1);
        SDL_CloseAudio();
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
	if (desired) free(desired);
	desired = NULL;
	if (obtain) free(obtain);
	obtain = NULL;
}

/* Hand the mixer to the emulation thread (on) or back to SDL (off). Taken
   under the device lock, so the callback is never part way through a render
   when it changes hands. */
void sound_own_mixer(int on)
{
    if (audio_device) SDL_LockAudioDevice(audio_device);
    else SDL_LockAudio();
    mixer_owned = on;
    if (audio_device) SDL_UnlockAudioDevice(audio_device);
    else SDL_UnlockAudio();
}

void pause_audio(int on) {
    // printf("PAUSE audio %d\n",on);
    if (audio_device)
        SDL_PauseAudioDevice(audio_device, on);
    else
        SDL_PauseAudio(on);
}

#else
int dev_dsp=0;
pthread_t audio_thread;
volatile int paused=0;
int buflen=0;


void *fill_audio_data(void *ptr) {
    printf("Update audio\n");

    while(1) {

        if (!paused && !mixer_owned) {
            YM2610Update_stream(buflen/4);
            write(dev_dsp,play_buffer,buflen);
        }
            
    }
}
void sound_own_mixer(int on) {
    mixer_owned = on;
}

void pause_audio(int on) {
    static init=0;
    if (init==0) {
        pthread_create(&audio_thread,NULL,fill_audio_data,NULL);
        init=1;
    }
    	paused=on;
}


int init_sdl_audio(void) {
    int format;
    int channels=2;
    int speed=conf.sample_rate;
    int arg = 0x9;

    dev_dsp=open("/dev/dsp",O_WRONLY);
    if (dev_dsp==0) {
        printf("Couldn't open /dev/dsp\n");
        return GN_FALSE;
    }

    if (ioctl(dev_dsp, SNDCTL_DSP_SETFRAGMENT, &arg)) {
		printf(" SNDCTL_DSP_SETFRAGMENT Error\n");
        return GN_FALSE;
	}

#ifdef WORDS_BIGENDIAN
    format = AFMT_S16_BE;
#else	/* */
    format = AFMT_S16_LE;
#endif	/* */

    if (ioctl(dev_dsp, SNDCTL_DSP_SETFMT, &format) == -1) {
        perror("SNDCTL_DSP_SETFMT");
        return GN_FALSE;
    }
    if (ioctl(dev_dsp, SNDCTL_DSP_CHANNELS, &channels) == -1) {
        perror("SNDCTL_DSP_CHANNELS");
        return GN_FALSE;
    }
    if (ioctl(dev_dsp, SNDCTL_DSP_SPEED, &speed)==-1) {
        perror("SNDCTL_DSP_SPEED");
        return GN_FALSE;
    }
    
    

    if (ioctl(dev_dsp, SNDCTL_DSP_GETBLKSIZE, &buflen) == -1) {
        return GN_FALSE;
    }
    printf("Buf Len=%d\n",buflen);
    buflen*=2;

    return GN_TRUE;
}

void close_sdl_audio(void) {
    close(dev_dsp);
}

#endif
