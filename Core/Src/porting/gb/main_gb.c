#include <odroid_system.h>
#include <string.h>
#include <assert.h>

#include "main.h"
#include "bilinear.h"
#include "gw_lcd.h"
#include "gw_linker.h"
#include "rg_i18n.h"
#include "gw_buttons.h"
#include "gnuboy/loader.h"
#include "gnuboy/hw.h"
#include "gnuboy/lcd.h"
#include "gnuboy/cpu.h"
#include "gnuboy/mem.h"
#include "gnuboy/sound.h"
#include "gnuboy/regs.h"
#include "gnuboy/rtc.h"
#include "gnuboy/defs.h"
#include "common.h"
#include "rom_manager.h"
#include "appid.h"
#include "gw_malloc.h"

#define NVS_KEY_SAVE_SRAM "sram"

// Use 60Hz for GB
#define AUDIO_BUFFER_LENGTH_GB (AUDIO_SAMPLE_RATE / 60)
static int16_t *audiobuffer_emulator;

static odroid_video_frame_t update1 = {GB_WIDTH, GB_HEIGHT, GB_WIDTH * 2, 2, 0xFF, -1, NULL, NULL, 0, {}};
static odroid_video_frame_t update2 = {GB_WIDTH, GB_HEIGHT, GB_WIDTH * 2, 2, 0xFF, -1, NULL, NULL, 0, {}};
static odroid_video_frame_t *currentUpdate = &update1;

static bool saveSRAM = false;
static int  saveSRAM_Timer = 0;

static uint8_t gb_framebuffer[GB_WIDTH*GB_HEIGHT*sizeof(uint16_t)];

// --- MAIN

#define WIDTH 320



__attribute__((optimize("unroll-loops")))
static inline void screen_blit_nn(int32_t dest_width, int32_t dest_height)
{
    static uint32_t lastFPSTime = 0;
    static uint32_t frames = 0;
    uint32_t currentTime = HAL_GetTick();
    uint32_t delta = currentTime - lastFPSTime;

    frames++;

    if (delta >= 1000) {
        int fps = (10000 * frames) / delta;
        printf("FPS: %d.%d, frames %ld, delta %ld ms, skipped %d\n", fps / 10, fps % 10, delta, frames, common_emu_state.skipped_frames);
        frames = 0;
        common_emu_state.skipped_frames = 0;
        lastFPSTime = currentTime;
    }

    int w1 = currentUpdate->width;
    int h1 = currentUpdate->height;
    int w2 = dest_width;
    int h2 = dest_height;

    int x_ratio = (int)((w1<<16)/w2) +1;
    int y_ratio = (int)((h1<<16)/h2) +1;
    int hpad = (320 - dest_width) / 2;
    int wpad = (240 - dest_height) / 2;

    int x2;
    int y2;

    uint16_t* screen_buf = (uint16_t*)currentUpdate->buffer;
    uint16_t *dest = lcd_get_active_buffer();

    PROFILING_INIT(t_blit);
    PROFILING_START(t_blit);

    for (int i=0;i<h2;i++) {
        for (int j=0;j<w2;j++) {
            x2 = ((j*x_ratio)>>16) ;
            y2 = ((i*y_ratio)>>16) ;
            uint16_t b2 = screen_buf[(y2*w1)+x2];
            dest[((i+wpad)*WIDTH)+j+hpad] = b2;
        }
    }

    PROFILING_END(t_blit);

#ifdef PROFILING_ENABLED
    printf("Blit: %d us\n", (1000000 * PROFILING_DIFF(t_blit)) / t_blit_t0.SecondFraction);
#endif
}

static void screen_blit_bilinear(int32_t dest_width)
{
    static uint32_t lastFPSTime = 0;
    static uint32_t frames = 0;
    uint32_t currentTime = HAL_GetTick();
    uint32_t delta = currentTime - lastFPSTime;

    frames++;

    if (delta >= 1000) {
        int fps = (10000 * frames) / delta;
        printf("FPS: %d.%d, frames %ld, delta %ld ms, skipped %d\n", fps / 10, fps % 10, delta, frames, common_emu_state.skipped_frames);
        frames = 0;
        common_emu_state.skipped_frames = 0;
        lastFPSTime = currentTime;
    }

    int w1 = currentUpdate->width;
    int h1 = currentUpdate->height;

    int w2 = dest_width;
    int h2 = 240;
    int stride = 320;
    int hpad = (320 - dest_width) / 2;

    uint16_t *dest = lcd_get_active_buffer();

    image_t dst_img;
    dst_img.w = dest_width;
    dst_img.h = 240;
    dst_img.bpp = 2;
    dst_img.pixels = ((uint8_t *) dest) + hpad * 2;

    if (hpad > 0) {
        memset(dest, 0x00, hpad * 2);
    }

    image_t src_img;
    src_img.w = currentUpdate->width;
    src_img.h = currentUpdate->height;
    src_img.bpp = 2;
    src_img.pixels = currentUpdate->buffer;

    float x_scale = ((float) w2) / ((float) w1);
    float y_scale = ((float) h2) / ((float) h1);



    PROFILING_INIT(t_blit);
    PROFILING_START(t_blit);

    imlib_draw_image(&dst_img, &src_img, 0, 0, stride, x_scale, y_scale, NULL, -1, 255, NULL,
                     NULL, IMAGE_HINT_BILINEAR, NULL, NULL);

    PROFILING_END(t_blit);

#ifdef PROFILING_ENABLED
    printf("Blit: %d us\n", (1000000 * PROFILING_DIFF(t_blit)) / t_blit_t0.SecondFraction);
#endif
}

static inline void screen_blit_v3to5(void) {
    static uint32_t lastFPSTime = 0;
    static uint32_t frames = 0;
    uint32_t currentTime = HAL_GetTick();
    uint32_t delta = currentTime - lastFPSTime;

    frames++;

    if (delta >= 1000) {
        int fps = (10000 * frames) / delta;
        printf("FPS: %d.%d, frames %ld, delta %ld ms, skipped %d\n", fps / 10, fps % 10, delta, frames, common_emu_state.skipped_frames);
        frames = 0;
        common_emu_state.skipped_frames = 0;
        lastFPSTime = currentTime;
    }

    uint16_t *dest = lcd_get_active_buffer();

    PROFILING_INIT(t_blit);
    PROFILING_START(t_blit);

#define CONV(_b0)    (((0b11111000000000000000000000&_b0)>>10) | ((0b000001111110000000000&_b0)>>5) | ((0b0000000000011111&_b0)))
#define EXPAND(_b0)  (((0b1111100000000000 & _b0) << 10) | ((0b0000011111100000 & _b0) << 5) | ((0b0000000000011111 & _b0)))

    int y_src = 0;
    int y_dst = 0;
    int w = currentUpdate->width;
    int h = currentUpdate->height;
    for (; y_src < h; y_src += 3, y_dst += 5) {
        int x_src = 0;
        int x_dst = 0;
        for (; x_src < w; x_src += 1, x_dst += 2) {
            uint16_t *src_col = &((uint16_t *)currentUpdate->buffer)[(y_src * w) + x_src];
            uint32_t b0 = EXPAND(src_col[w * 0]);
            uint32_t b1 = EXPAND(src_col[w * 1]);
            uint32_t b2 = EXPAND(src_col[w * 2]);

            dest[((y_dst + 0) * WIDTH) + x_dst] = CONV(b0);
            dest[((y_dst + 1) * WIDTH) + x_dst] = CONV((b0+b1)>>1);
            dest[((y_dst + 2) * WIDTH) + x_dst] = CONV(b1);
            dest[((y_dst + 3) * WIDTH) + x_dst] = CONV((b1+b2)>>1);
            dest[((y_dst + 4) * WIDTH) + x_dst] = CONV(b2);

            dest[((y_dst + 0) * WIDTH) + x_dst + 1] = CONV(b0);
            dest[((y_dst + 1) * WIDTH) + x_dst + 1] = CONV((b0+b1)>>1);
            dest[((y_dst + 2) * WIDTH) + x_dst + 1] = CONV(b1);
            dest[((y_dst + 3) * WIDTH) + x_dst + 1] = CONV((b1+b2)>>1);
            dest[((y_dst + 4) * WIDTH) + x_dst + 1] = CONV(b2);
        }
    }

    PROFILING_END(t_blit);

#ifdef PROFILING_ENABLED
    printf("Blit: %d us\n", (1000000 * PROFILING_DIFF(t_blit)) / t_blit_t0.SecondFraction);
#endif
}

static inline void screen_blit_jth(void) {
    static uint32_t lastFPSTime = 0;
    static uint32_t frames = 0;
    uint32_t currentTime = HAL_GetTick();
    uint32_t delta = currentTime - lastFPSTime;

    frames++;

    if (delta >= 1000) {
        int fps = (10000 * frames) / delta;
        printf("FPS: %d.%d, frames %ld, delta %ld ms, skipped %d\n", fps / 10, fps % 10, delta, frames, common_emu_state.skipped_frames);
        frames = 0;
        common_emu_state.skipped_frames = 0;
        lastFPSTime = currentTime;
    }


    uint16_t* screen_buf = (uint16_t*)currentUpdate->buffer;
    uint16_t *dest = lcd_get_active_buffer();

    PROFILING_INIT(t_blit);
    PROFILING_START(t_blit);


    int w1 = currentUpdate->width;
    int h1 = currentUpdate->height;
    int w2 = 320;
    int h2 = 240;

    const int border = 24;

    // Iterate on dest buf rows
    for(int y = 0; y < border; ++y) {
        uint16_t *src_row  = &screen_buf[y * w1];
        uint16_t *dest_row = &dest[y * w2];
        for (int x = 0, xsrc=0; x < w2; x+=2,xsrc++) {
            dest_row[x]     = src_row[xsrc];
            dest_row[x + 1] = src_row[xsrc];
        }
    }

    for (int y = border, src_y = border; y < h2-border; y+=2, src_y++) {
        uint16_t *src_row  = &screen_buf[src_y * w1];
        uint32_t *dest_row0 = (uint32_t *) &dest[y * w2];
        for (int x = 0, xsrc=0; x < w2; x++,xsrc++) {
            uint32_t col = src_row[xsrc];
            dest_row0[x] = (col | (col << 16));
        }
    }

    for (int y = border, src_y = border; y < h2-border; y+=2, src_y++) {
        uint16_t *src_row  = &screen_buf[src_y * w1];
        uint32_t *dest_row1 = (uint32_t *)&dest[(y + 1) * w2];
        for (int x = 0, xsrc=0; x < w2; x++,xsrc++) {
            uint32_t col = src_row[xsrc];
            dest_row1[x] = (col | (col << 16));
        }
    }

    for(int y = 0; y < border; ++y) {
        uint16_t *src_row  = &screen_buf[(h1-border+y) * w1];
        uint16_t *dest_row = &dest[(h2-border+y) * w2];
        for (int x = 0, xsrc=0; x < w2; x+=2,xsrc++) {
            dest_row[x]     = src_row[xsrc];
            dest_row[x + 1] = src_row[xsrc];
        }
    }


    PROFILING_END(t_blit);

#ifdef PROFILING_ENABLED
    printf("Blit: %d us\n", (1000000 * PROFILING_DIFF(t_blit)) / t_blit_t0.SecondFraction);
#endif
}

static void blit(void)
{
    odroid_display_scaling_t scaling = odroid_display_get_scaling_mode();
    odroid_display_filter_t filtering = odroid_display_get_filter_mode();

    switch (scaling) {
    case ODROID_DISPLAY_SCALING_OFF:
        // Original Resolution
        screen_blit_nn(160, 144);
        break;
    case ODROID_DISPLAY_SCALING_FIT:
        // Full height, borders on the side
        switch (filtering) {
        case ODROID_DISPLAY_FILTER_OFF:
            /* fall-through */
        case ODROID_DISPLAY_FILTER_SHARP:
            // crisp nearest neighbor scaling
            screen_blit_nn(266, 240);
            break;
        case ODROID_DISPLAY_FILTER_SOFT:
            // soft bilinear scaling
            screen_blit_bilinear(266);
            break;
        default:
            printf("Unknown filtering mode %d\n", filtering);
            assert(!"Unknown filtering mode");
        }
        break;
    case ODROID_DISPLAY_SCALING_FULL:
        // full height, full width
        switch (filtering) {
        case ODROID_DISPLAY_FILTER_OFF:
            // crisp nearest neighbor scaling
            screen_blit_nn(320, 240);
            break;
        case ODROID_DISPLAY_FILTER_SHARP:
            // sharp bilinear-ish scaling
            screen_blit_v3to5();
            break;
        case ODROID_DISPLAY_FILTER_SOFT:
            // soft bilinear scaling
            screen_blit_bilinear(320);
            break;
        default:
            printf("Unknown filtering mode %d\n", filtering);
            assert(!"Unknown filtering mode");
        }
        break;
    case ODROID_DISPLAY_SCALING_CUSTOM:
        // compressed top and bottom sections, full width
        screen_blit_jth();
        break;
    default:
        printf("Unknown scaling mode %d\n", scaling);
        assert(!"Unknown scaling mode");
        break;
    }
    common_ingame_overlay();
}

static void blit_and_swap(void)
{
    blit();
    lcd_swap();
}

#define STATE_SAVE_BUFFER_LENGTH 1024 * 192

static bool SaveState(const char *savePathName)
{
    printf("Saving state...\n");

    // Use GB_ROM_SRAM_CACHE (which points to _GB_ROM_UNPACK_BUFFER)
    // as a temporary save buffer.
    size_t size = gb_state_save((uint8_t *)&_OVERLAY_GB_BSS_END, STATE_SAVE_BUFFER_LENGTH);
    FILE *file = fopen(savePathName, "wb");
    if (file == NULL) {
        printf("Error opening file\n");
        return false;
    }

    fwrite((const void *)&_OVERLAY_GB_BSS_END, 1, size, file);

    fclose(file);
    
    return true;
}

static bool LoadState(const char *savePathName)
{
    size_t savestate_size;

    // We store data in the not visible framebuffer
    FILE *file = fopen(savePathName, "rb");
    if (file == NULL) {
        printf("Error opening file\n");
        return false;
    }

    savestate_size = fread((void *)&_OVERLAY_GB_BSS_END, 1, STATE_SAVE_BUFFER_LENGTH, file);

    fclose(file);

    gb_state_load((const uint8_t *)_OVERLAY_GB_BSS_END, savestate_size);

    return true;
}


static bool palette_update_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event, uint32_t repeat)
{
    int pal = pal_get_dmg();
    int max = pal_count_dmg();

    if (event == ODROID_DIALOG_PREV) {
        pal = pal > 0 ? pal - 1 : max;
    }

    if (event == ODROID_DIALOG_NEXT) {
        pal = pal < max ? pal + 1 : 0;
    }

    if (event == ODROID_DIALOG_PREV || event == ODROID_DIALOG_NEXT) {
        odroid_settings_Palette_set(pal);
        pal_set_dmg(pal);
    }

    if (pal == 0) strcpy(option->value, "GBC");
    else sprintf(option->value, "%d/%d", pal, max);

    return event == ODROID_DIALOG_ENTER;
}

/*static bool save_sram_update_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event, uint32_t repeat)
{
    if (event == ODROID_DIALOG_PREV || event == ODROID_DIALOG_NEXT) {
        saveSRAM = !saveSRAM;
        odroid_settings_app_int32_set(NVS_KEY_SAVE_SRAM, saveSRAM);
    }

    strcpy(option->value, saveSRAM ? "Yes" : "No");

    return event == ODROID_DIALOG_ENTER;
    return -
}

static bool rtc_t_update_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event, uint32_t repeat)
{
    if (option->id == 'd') {
        if (event == ODROID_DIALOG_PREV && --rtc.d < 0) rtc.d = 364;
        if (event == ODROID_DIALOG_NEXT && ++rtc.d > 364) rtc.d = 0;
        sprintf(option->value, "%03d", rtc.d);
    }
    if (option->id == 'h') {
        if (event == ODROID_DIALOG_PREV && --rtc.h < 0) rtc.h = 23;
        if (event == ODROID_DIALOG_NEXT && ++rtc.h > 23) rtc.h = 0;
        sprintf(option->value, "%02d", rtc.h);
    }
    if (option->id == 'm') {
        if (event == ODROID_DIALOG_PREV && --rtc.m < 0) rtc.m = 59;
        if (event == ODROID_DIALOG_NEXT && ++rtc.m > 59) rtc.m = 0;
        sprintf(option->value, "%02d", rtc.m);
    }
    if (option->id == 's') {
        if (event == ODROID_DIALOG_PREV && --rtc.s < 0) rtc.s = 59;
        if (event == ODROID_DIALOG_NEXT && ++rtc.s > 59) rtc.s = 0;
        sprintf(option->value, "%02d", rtc.s);
    }
    return event == ODROID_DIALOG_ENTER;
}

static bool rtc_update_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event, uint32_t repeat)
{
    if (event == ODROID_DIALOG_ENTER) {
        static odroid_dialog_choice_t choices[] = {
            {'d', "Day", "000", 1, &rtc_t_update_cb},
            {'h', "Hour", "00", 1, &rtc_t_update_cb},
            {'m', "Min",  "00", 1, &rtc_t_update_cb},
            {'s', "Sec",  "00", 1, &rtc_t_update_cb},
            ODROID_DIALOG_CHOICE_LAST
        };
        odroid_overlay_dialog("Set Clock", choices, 0);
    }
    sprintf(option->value, "%02d:%02d", rtc.h, rtc.m);
    return false;
}

static bool advanced_settings_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event, uint32_t repeat)
{
   if (event == ODROID_DIALOG_ENTER) {
      odroid_dialog_choice_t options[] = {
        {101, "Set clock", "00:00", 1, &rtc_update_cb},
        {102, "Auto save SRAM", "No", 1, &save_sram_update_cb},
        ODROID_DIALOG_CHOICE_LAST
      };
      odroid_overlay_dialog("Advanced", options, 0);
   }
   return false;
}*/

void pcm_submit() {
    if (common_emu_sound_loop_is_muted()) {
        return;
    }

    int32_t factor = common_emu_sound_get_volume();
    int16_t* sound_buffer = audio_get_active_buffer();
    uint16_t sound_buffer_length = audio_get_buffer_length();

    // Write to sound buffer and lower the volume accordingly
    for (int i = 0; i < sound_buffer_length; i++) {
        int32_t sample = pcm.buf[i];
        sound_buffer[i] = (sample * factor) >> 8;
    }
}

rg_app_desc_t * init(uint8_t load_state, int8_t save_slot)
{
    odroid_system_init(APPID_GB, AUDIO_SAMPLE_RATE);
    odroid_system_emu_init(&LoadState, &SaveState, NULL);

    // bzhxx : fix LCD glitch at the start by cleaning up the buffer emulator
    memset(gb_framebuffer, 0x0, sizeof(gb_framebuffer));

    // Hack: Use the same buffer twice
    update1.buffer = gb_framebuffer;
    update2.buffer = gb_framebuffer;

    //saveSRAM = odroid_settings_app_int32_get(NVS_KEY_SAVE_SRAM, 0);
    saveSRAM = false;

    // Load ROM
    loader_init(NULL);

    // RTC
    memset(&rtc, 0, sizeof(rtc));

    // Video
    memset(&fb, 0, sizeof(fb));
    fb.w = GB_WIDTH;
    fb.h = GB_HEIGHT;
    fb.format = GB_PIXEL_565_LE;
    fb.pitch = update1.stride;
    fb.ptr = currentUpdate->buffer;
    fb.enabled = 1;
    fb.blit_func = &blit_and_swap;

    // Audio
    audiobuffer_emulator = ahb_calloc(sizeof(uint16_t),AUDIO_BUFFER_LENGTH_GB);
    memset(&pcm, 0, sizeof(pcm));
    pcm.hz = AUDIO_SAMPLE_RATE;
    pcm.stereo = 0;
    pcm.len = AUDIO_BUFFER_LENGTH_GB;
    pcm.buf = (n16*)audiobuffer_emulator;
    pcm.pos = 0;

    audio_start_playing(AUDIO_BUFFER_LENGTH_GB);

    rg_app_desc_t *app = odroid_system_get_app();

    emu_init();

    pal_set_dmg(odroid_settings_Palette_get());

    if (load_state) {
        odroid_system_emu_load_state(save_slot);
    } else {
        lcd_clear_buffers();
    }

    return app;
}

void app_main_gb(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    init(load_state, save_slot);
    odroid_gamepad_state_t joystick;

    if (start_paused) {
        common_emu_state.pause_after_frames = 2;
        odroid_audio_mute(true);
    } else {
        common_emu_state.pause_after_frames = 0;
    }

    while (true)
    {
        wdog_refresh();

        bool drawFrame = common_emu_frame_loop();

        char palette_values[16];
        snprintf(palette_values, sizeof(palette_values), "%s", "7/7");
        odroid_dialog_choice_t options[] = {
            {300, curr_lang->s_Palette, (char *)palette_values, hw.cgb ? -1 : 1, &palette_update_cb},
            // {301, "More...", "", 1, &advanced_settings_cb},
            ODROID_DIALOG_CHOICE_LAST
        };

        odroid_input_read_gamepad(&joystick);
        common_emu_input_loop(&joystick, options, &blit);
        common_emu_input_loop_handle_turbo(&joystick);

        pad_set(PAD_UP, joystick.values[ODROID_INPUT_UP]);
        pad_set(PAD_RIGHT, joystick.values[ODROID_INPUT_RIGHT]);
        pad_set(PAD_DOWN, joystick.values[ODROID_INPUT_DOWN]);
        pad_set(PAD_LEFT, joystick.values[ODROID_INPUT_LEFT]);
        pad_set(PAD_SELECT, joystick.values[ODROID_INPUT_SELECT] | joystick.values[ODROID_INPUT_Y]);
        pad_set(PAD_START, joystick.values[ODROID_INPUT_START] | joystick.values[ODROID_INPUT_X]);
        pad_set(PAD_A, joystick.values[ODROID_INPUT_A]);
        pad_set(PAD_B, joystick.values[ODROID_INPUT_B]);

        emu_run(drawFrame);

        if (saveSRAM)
        {
            if (ram.sram_dirty)
            {
                saveSRAM_Timer = 120; // wait 2 seconds
                ram.sram_dirty = 0;
            }

            if (saveSRAM_Timer > 0 && --saveSRAM_Timer == 0)
            {
                // TO DO: Try compressing the sram file, it might reduce stuttering
                sram_save();
            }
        }

        common_emu_sound_sync(false);
    }
}
