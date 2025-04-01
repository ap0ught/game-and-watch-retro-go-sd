#include <odroid_system.h>
#include <string.h>
#include <assert.h>

#include "main.h"
#include "bilinear.h"
#include "gw_lcd.h"
#include "gw_linker.h"
#include "rg_i18n.h"
#include "gw_buttons.h"
#include "common.h"
#include "rom_manager.h"
#include "appid.h"
#include "gw_malloc.h"
#include "rg_storage.h"

// PokeMini headers
#include "PokeMini.h"
#include "MinxIO.h"
#include "PMCommon.h"
#include "Hardware.h"
#include "Joystick.h"
#include "MinxAudio.h"
#include "Video.h"
#include "Video_x3.h"

#define PKMINI_FPS 72
#define PKMINI_SAMPLE_RATE 44100
// (PKMINI_SAMPLE_RATE/PKMINI_FPS) = 612.5
#define PKMINI_BUFFER_LENGTH_MIN 612
#define PKMINI_BUFFER_LENGTH_MAX 613
static int16_t pkmini_audio_samples[PKMINI_BUFFER_LENGTH_MAX];

// Sound buffer size
#define SOUNDBUFFER	2048
#define PMSOUNDBUFF	(SOUNDBUFFER*2)
static int32_t low_pass_range = 0;
static int32_t low_pass_prev  = 0; /* Previous sample */

#define PKMINI_SCALE 3
#define PKMINI_WIDTH 96
#define PKMINI_HEIGHT 64

uint16_t pkmini_data[PKMINI_SCALE * PKMINI_WIDTH * PKMINI_SCALE * PKMINI_HEIGHT];

static void blit();

// Called by the Reload in game menu option
static bool LoadState(const char *savePathName) {
    return PokeMini_LoadSSStream(savePathName, 0);
}

static bool SaveState(const char *savePathName) {
    PokeMini_SaveEEPROMFile(CommandLine.eeprom_file);
    return PokeMini_SaveSSStream(savePathName, 0);
}

static void *Screenshot()
{
    lcd_wait_for_vblank();
    blit();
    return lcd_get_active_buffer();
}

static void blit() {
    uint16_t x_offset = (WIDTH - PKMINI_WIDTH * PKMINI_SCALE) / 2;
    uint16_t y_offset = (HEIGHT - PKMINI_HEIGHT * PKMINI_SCALE) / 2;
    uint16_t *src_buffer = pkmini_data;
    uint16_t *dst_buffer = (uint16_t *)lcd_get_active_buffer() + y_offset * WIDTH + x_offset;
    for (int y = 0; y < PKMINI_HEIGHT * PKMINI_SCALE; y++) {
        memcpy(dst_buffer,(void *)src_buffer,PKMINI_WIDTH * PKMINI_SCALE * sizeof(uint16_t));
        src_buffer+=PKMINI_WIDTH * PKMINI_SCALE;
        dst_buffer+=WIDTH;
    }
}

static void ZeroArray(uint16_t array[], int size)
{
	int i;
	for (i = 0; i < size; i++)
		array[i] = 0;
}

static void ReverseArray(uint16_t array[], int size)
{
	int i, j;
	for (i = 0, j = size; i < j; i++, j--)
	{
		uint16_t tmp = array[i];
		array[i] = array[j];
		array[j] = tmp;
	}
}

static void pkmini_pcm_submit() {
    if (common_emu_sound_loop_is_muted()) {
        return;
    }

    int32_t factor = common_emu_sound_get_volume();
    int16_t* sound_buffer = audio_get_active_buffer();
    uint16_t sound_buffer_length = audio_get_buffer_length();

    for (int i = 0; i < sound_buffer_length; i++) {
        int32_t sample = pkmini_audio_samples[i];
        sound_buffer[i] = (sample * factor) >> 8;
    }
}

static void ApplyLowPassFilterUpmix(int16_t *buffer, int32_t length)
{
   /* Restore previous sample */
   int32_t low_pass = low_pass_prev;

   /* Single-pole low-pass filter (6 dB/octave) */
   int32_t factor_a = low_pass_range;
   int32_t factor_b = 0x10000 - factor_a;

   do
   {
      /* Apply low-pass filter */
      low_pass = (low_pass * factor_a) + (*buffer * factor_b);

      /* 16.16 fixed point */
      low_pass >>= 16;

      /* Update sound buffer
       * > Note: Sound is mono, converted to
       *   stereo by duplicating the left/right
       *   channels */
      *buffer++ = (int16_t)low_pass;
   }
   while (--length);

   /* Save last sample for next frame */
   low_pass_prev = low_pass;
}

// Apply screen shake effect
static void SetPixelOffset(void)
{
	if (CommandLine.rumblelvl)
	{
		int row_offset = PokeMini_GenRumbleOffset(PKMINI_WIDTH * PKMINI_SCALE) * PKMINI_SCALE;
		int buffer_size = PKMINI_WIDTH * PKMINI_SCALE * PKMINI_HEIGHT * PKMINI_SCALE;
		
		// Derived from the classic 'rotate array' technique
		// from 'Programming Pearls' by Jon Bentley
		if (row_offset < 0)
		{
			row_offset = buffer_size + row_offset;
			ZeroArray(pkmini_data, buffer_size - row_offset - 1);
			ReverseArray(pkmini_data + buffer_size - row_offset, row_offset - 1);
		}
		else
		{
			ReverseArray(pkmini_data, buffer_size - row_offset - 1);
			ZeroArray(pkmini_data + buffer_size - row_offset, row_offset - 1);
		}

		ReverseArray(pkmini_data, buffer_size - 1);
	}
}

static int load_rom()
{
    uint32_t size;
    uint8_t* buffer;
	// Free existing color information
	PokeMini_FreeColorInfo();

    rg_stat_t stat = rg_storage_stat(ACTIVE_FILE->path);
    size = stat.size;

	// Check if size is valid
	if ((size <= 0x2100) || (size > 0x200000))
		return 0;

	PM_ROM_Mask = GetMultiple2Mask(size);
	PM_ROM_Size = PM_ROM_Mask + 1;

    if (PM_ROM_Size > 512 * 1024) {
        buffer = odroid_overlay_cache_file_in_flash(ACTIVE_FILE->path, &size, false);
    } else {
        buffer = ram_malloc(PM_ROM_Size);
        if (buffer) {
            odroid_overlay_cache_file_in_ram(ACTIVE_FILE->path, (uint8_t *)buffer);
        }
    }

	PM_ROM = buffer;

	NewMulticart();
	
	return 1;
}

// TODO : Implement call to shutdown function when system is powered off
static void shutdown() {
    // Save EEPROM
    if (PokeMini_EEPROMWritten)
    {
        PokeMini_SaveEEPROMFile(CommandLine.eeprom_file);
    }

    // Terminate emulator
	PokeMini_Destroy();
}

_Noreturn void app_main_pkmini(uint8_t load_state, uint8_t start_paused, int8_t save_slot) {
    odroid_dialog_choice_t options[] = {
        ODROID_DIALOG_CHOICE_LAST
    };
    TPokeMini_VideoSpec *video_spec = NULL;
    odroid_gamepad_state_t joystick;

    ram_start = (uint32_t)&_OVERLAY_PKMINI_BSS_END;

    if (start_paused) {
        common_emu_state.pause_after_frames = 2;
    } else {
        common_emu_state.pause_after_frames = 0;
    }
    common_emu_state.frame_time_10us = (uint16_t)(100000 / PKMINI_FPS + 0.5f);

    lcd_set_refresh_rate(PKMINI_FPS);

    odroid_system_init(APPID_PKMINI, PKMINI_SAMPLE_RATE);
    odroid_system_emu_init(&LoadState, &SaveState, &Screenshot);

    // Init Sound
    audio_start_playing_full_length(PKMINI_BUFFER_LENGTH_MAX+PKMINI_BUFFER_LENGTH_MIN);

    CommandLineInit();
    strcpy(CommandLine.bios_file, "/bios/mini/bios.min");

    char *sram_path = odroid_system_get_path(ODROID_PATH_SAVE_SRAM, ACTIVE_FILE->path);
    strcpy(CommandLine.eeprom_file, sram_path);
    free(sram_path);
	CommandLine.forcefreebios = 0; // OFF
	CommandLine.eeprom_share = 0;  // OFF (there is no practical benefit to a shared eeprom save
	                               //      - it just gets full and becomes a nuisance...)
	CommandLine.updatertc = 2;	   // Update RTC (0=Off, 1=State, 2=Host)

    video_spec = (TPokeMini_VideoSpec *)&PokeMini_Video3x3;

    PokeMini_SetVideo(video_spec, 16, 0, 0);//CommandLine.lcdfilter, CommandLine.lcdmode);

    PokeMini_Create(0, PMSOUNDBUFF);

    PokeMini_VideoPalette_Init(PokeMini_BGR16, 0/* disable high colour*/);

    PokeMini_VideoPalette_Index(CommandLine.palette, NULL, CommandLine.lcdcontrast, CommandLine.lcdbright);

    PokeMini_ApplyChanges();

    MinxAudio_ChangeEngine(CommandLine.sound);

    load_rom();

	// Load EEPROM
	MinxIO_FormatEEPROM();
	if (rg_storage_exists(CommandLine.eeprom_file))
	{
        printf("Read EEPROM file: %s\n", CommandLine.eeprom_file);
		PokeMini_LoadEEPROMFile(CommandLine.eeprom_file);
	}

    PokeMini_Reset(0);

    if (load_state) {
        odroid_system_emu_load_state(save_slot);
    } else {
        lcd_clear_buffers();
    }

    while (true) {
        wdog_refresh();

        bool drawFrame = common_emu_frame_loop();

        odroid_input_read_gamepad(&joystick);
        common_emu_input_loop(&joystick, options, &blit);
        common_emu_input_loop_handle_turbo(&joystick);

        if (joystick.values[ODROID_INPUT_A] == 1)
            MinxIO_Keypad(MINX_KEY_A, 1);
        else
            MinxIO_Keypad(MINX_KEY_A, 0);
        if (joystick.values[ODROID_INPUT_B] == 1)
            MinxIO_Keypad(MINX_KEY_B, 1);
        else
            MinxIO_Keypad(MINX_KEY_B, 0);
        if (joystick.values[ODROID_INPUT_START] == 1)
            MinxIO_Keypad(MINX_KEY_C, 1);
        else
            MinxIO_Keypad(MINX_KEY_C, 0);
        if (joystick.values[ODROID_INPUT_X] == 1)
            MinxIO_Keypad(MINX_KEY_C, 1);
        else
            MinxIO_Keypad(MINX_KEY_C, 0);
        if (joystick.values[ODROID_INPUT_SELECT] == 1)
            MinxIO_Keypad(MINX_KEY_SHOCK, 1);
        else
            MinxIO_Keypad(MINX_KEY_SHOCK, 0);
        if (joystick.values[ODROID_INPUT_Y] == 1)
            MinxIO_Keypad(MINX_KEY_SHOCK, 1);
        else
            MinxIO_Keypad(MINX_KEY_SHOCK, 0);
        if (joystick.values[ODROID_INPUT_UP] == 1)
            MinxIO_Keypad(MINX_KEY_UP, 1);
        else
            MinxIO_Keypad(MINX_KEY_UP, 0);
        if (joystick.values[ODROID_INPUT_DOWN] == 1)
            MinxIO_Keypad(MINX_KEY_DOWN, 1);
        else
            MinxIO_Keypad(MINX_KEY_DOWN, 0);
        if (joystick.values[ODROID_INPUT_LEFT] == 1)
            MinxIO_Keypad(MINX_KEY_LEFT, 1);
        else
            MinxIO_Keypad(MINX_KEY_LEFT, 0);
        if (joystick.values[ODROID_INPUT_RIGHT] == 1)
            MinxIO_Keypad(MINX_KEY_RIGHT, 1);
        else
            MinxIO_Keypad(MINX_KEY_RIGHT, 0);

        PokeMini_EmulateFrame();

        MinxAudio_GetSamplesS16Ch(pkmini_audio_samples, audio_get_buffer_length(), 1);
        ApplyLowPassFilterUpmix(pkmini_audio_samples,
                audio_get_buffer_length());
        pkmini_pcm_submit();

        if (drawFrame) {
            PokeMini_VideoBlit((uint16_t *)pkmini_data, PKMINI_WIDTH*PKMINI_SCALE);
            lcd_swap();
            if (PokeMini_Rumbling)
                SetPixelOffset();
            blit();
        }
        common_ingame_overlay();

        common_emu_sound_sync(false);
    }
}
