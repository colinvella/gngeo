/*
 * control.h -- remote control socket for automated testing.
 *
 * Lets an external process drive the emulator: inject pad state, run an exact
 * number of frames, grab the framebuffer and read 68000 work RAM. Enabled only
 * when GNGEO_CONTROL_PORT is set in the environment, so an ordinary run is
 * completely unaffected.
 */
#ifndef GNGEO_CONTROL_H
#define GNGEO_CONTROL_H

/* Start the listener if GNGEO_CONTROL_PORT is set. Safe to call always. */
void control_init(void);

/* Called at the top of every frame. Services pending requests and blocks
   while the emulator is paused, so a client can single-step frames. */
void control_frame_begin(void);

/* Called once the frame has been rendered. */
void control_frame_end(void);

void control_shutdown(void);

/* ------------------------------------------------------------------ audio
 *
 * Three different questions, so three different taps:
 *   - did the 68000 ask for the sound   -> control_note_command
 *   - did the driver actually start it  -> control_note_adpcm*
 *   - did anything audible come out     -> control_audio_tap
 * All are no-ops unless the control socket is enabled.
 */

/* A byte written to the sound command port at 0x320000. */
void control_note_command(unsigned char cmd);

/* An ADPCM-A voice keyed on. start/end are the sample's V-ROM offsets as the
   chip holds them, which is what identifies the sample. */
void control_note_adpcma(int channel, unsigned int start, unsigned int end);

/* The ADPCM-B (music) voice keyed on. */
void control_note_adpcmb(unsigned int start, unsigned int end, int repeat);

/* The mixed stereo output, from the audio thread. */
void control_audio_tap(const void *stream, int bytes);

#endif
