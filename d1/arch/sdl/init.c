// Holds the main init and de-init functions for arch-related program parts

#include <SDL.h>
#include "songs.h"
#include "key.h"
#include "digi.h"
#include "mouse.h"
#include "joy.h"
#include "gr.h"
#include "dxxerror.h"
#include "text.h"
#include "args.h"
#include "config.h"

void arch_close(void)
{
	songs_uninit();

	gr_close();

	if (!GameArg.CtlNoJoystick)
		joy_close();

	if (!GameArg.CtlNoMouse)
		mouse_close();

	if (!GameArg.SndNoSound)
	{
		digi_close();
	}

	key_close();

	SDL_Quit();
}

void arch_init(void)
{
	int t;

	extern int Dedicated_server;
	if (Dedicated_server)
	{
		if (SDL_Init(SDL_INIT_TIMER) < 0)
			Error("SDL library initialisation failed: %s.", SDL_GetError());
		/* Same as interactive -nosound: wire up the digi_* function
		 * pointers (cheap, no device opened) so level load's unconditional
		 * digi_init_sounds()/set_sound_sources() calls have valid fptrs to
		 * call into. Never call digi_init() itself -- that's what would
		 * open a real audio device. */
		digi_select_system( GameArg.SndDisableSdlMixer ? SDLAUDIO_SYSTEM : SDLMIXER_SYSTEM );
		return; /* headless: no video, no input devices, no sound device */
	}

	if (SDL_Init(SDL_INIT_VIDEO) < 0)
		Error("SDL library initialisation failed: %s.",SDL_GetError());

	key_init();

	digi_select_system( GameArg.SndDisableSdlMixer ? SDLAUDIO_SYSTEM : SDLMIXER_SYSTEM );

	if (!GameArg.SndNoSound)
		digi_init();

	if (!GameArg.CtlNoMouse)
		mouse_init();

	if (!GameArg.CtlNoJoystick)
		joy_init();

	if ((t = gr_init(0)) != 0)
		Error(TXT_CANT_INIT_GFX,t);

	atexit(arch_close);
}

