extern "C"
{
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
#include "cpp_init_array.h"
#ifndef GNW_DISABLE_COMPRESSION
#include "lzma.h"
#endif
#include "heap.hpp"
#include "DefPropsBin.h"

extern void __libc_init_array(void);
}

#include "Console.hxx"
#include "Cart.hxx"
#include "MD5.hxx"
#include "Paddles.hxx"
#include "Sound.hxx"
#include "Switches.hxx"
#include "StateManager.hxx"
#include "TIA.hxx"
#include "M6532.hxx"
#include "Version.hxx"

#include "Stubs.hxx"

static Console *console = 0;
static Cartridge *cartridge = 0;
static Settings *settings = 0;
static OSystem osystem;
static StateManager stateManager(&osystem);

static int videoWidth, videoHeight;

static const uint32_t *currentPalette32 = NULL;
static uint16_t currentPalette16[256] = {0};

static int16_t sampleBuffer[2048];

static int paddle_digital_sensitivity = 50;

#define AUDIO_A2600_SAMPLE_RATE 31400

static void blend_frames_16(uInt8 *stella_fb, int width, int height);
static void blit();

static bool LoadState(const char *savePathName)
{
    Serializer in(savePathName, true);
    if(!in.isValid()) {
        return false;  // Failed to open file for reading
    }
    return stateManager.loadState(in);
}

static bool SaveState(const char *savePathName)
{
    Serializer out(savePathName, false);
    if(!out.isValid()) {
        return false;  // Failed to open file for writing
    }
    return stateManager.saveState(out);
}

static void *Screenshot()
{
    lcd_wait_for_vblank();

    lcd_clear_active_buffer();

    blit();

    return lcd_get_active_buffer();
}

uint8_t a2600_y_offset = 0x24;
uint16_t a2600_height = 250;
char a2600_control[15];
bool a2600_control_swap = false;
bool a2600_swap_paddle = false;
char a2600_display_mode[10];
uint8_t a2600_difficulty;
bool a2600_fastscbios = false;

static string cartType = "AUTO";

#ifndef GNW_DISABLE_COMPRESSION
// Memory to handle compressed roms
#define ROM_BUFF_LENGTH 131072 // 128kB
static uint8_t rom_memory[ROM_BUFF_LENGTH];
#endif

static size_t getromdata(unsigned char **data) {
#ifndef GNW_DISABLE_COMPRESSION
    /* src pointer to the ROM data in the external flash (raw or LZ4) */
    const unsigned char *src = ROM_DATA;
    unsigned char *dest = (unsigned char *)rom_memory;

    if(strcmp(ROM_EXT, "lzma") == 0){
        size_t n_decomp_bytes;
        n_decomp_bytes = lzma_inflate(dest, ROM_BUFF_LENGTH, src, ROM_DATA_LENGTH);
        *data = dest;
        return n_decomp_bytes;
    } else {
        *data = (unsigned char *)ROM_DATA;
        return ROM_DATA_LENGTH;
    }
#else
    uint32_t size = ACTIVE_FILE->size;
//    if (size > (heap_free_mem())) {
        *data = (uint8_t *)odroid_overlay_cache_file_in_flash(ACTIVE_FILE->path, &size, false);
//    } else {
//        *data = (uint8_t *)heap_alloc_mem(size);
//        if (data != NULL) {
//            odroid_overlay_cache_file_in_ram(ACTIVE_FILE->path, (uint8_t *)data);
//        }
//    }
    return size;
#endif
}

void fill_stella_config(string md5string)
{
    rom_properties_t props;
    char md5[33];

    strncpy(md5, md5string.c_str(), sizeof(md5) - 1);
    md5[sizeof(md5) - 1] = '\0';
    
    // Initialize properties database
    if (!defprops_init("/cores/a2600_defprops.bin")) {
        printf("Stella: Failed to load database.\n");
        // Default values if database not found
        strcpy(a2600_control, ""); // Joystick
        strcpy(a2600_display_mode, "NTSC");
        a2600_difficulty = 0;
        cartType = "AUTO";
        return;
    }
    
    // Get properties for this ROM
    if (defprops_get_properties(md5, &props)) {
        printf("Stella: Found properties in database\n");
        // Set properties from database
        if (props.yoffset[0] != '\0') {
            a2600_y_offset = atoi(props.yoffset);
            printf("Stella: yoffset: %d\n", a2600_y_offset);
        }
        if (props.height[0] != '\0') {
            a2600_height = atoi(props.height);
            printf("Stella: height: %d\n", a2600_height);
        }
        if (props.control[0] != '\0') {
            strncpy(a2600_control, props.control, sizeof(a2600_control) - 1);
            a2600_control[sizeof(a2600_control) - 1] = '\0';
        }
        if (props.region[0] != '\0') {
            strncpy(a2600_display_mode, props.region, sizeof(a2600_display_mode) - 1);
            a2600_display_mode[sizeof(a2600_display_mode) - 1] = '\0';
        }
        if (props.difficulty[0] != '\0') {
            a2600_difficulty = atoi(props.difficulty);
        }
        if (props.control_swap[0] != '\0') {
            a2600_control_swap = (strcmp(props.control_swap, "YES") == 0);
        }
        if (props.paddle_swap[0] != '\0') {
            a2600_swap_paddle = (strcmp(props.paddle_swap, "YES") == 0);
        }
        if (props.mapper[0] != '\0') {
            cartType = props.mapper;
        } else {
            cartType = "AUTO";
        }
    } else {
        printf("Stella: No properties found for %s\n", md5);
        // Default values if ROM not found in database
        strcpy(a2600_control, ""); // Joystick
        strcpy(a2600_display_mode, "NTSC");
        a2600_difficulty = 0;
        cartType = "AUTO";
    }
    printf("Stella: cartType: %s\n", cartType.c_str());
    defprops_cleanup();
}

void update_joystick(odroid_gamepad_state_t *joystick)
{
    Event &ev = osystem.eventHandler().event();
    ev.set(Event::Type(Event::JoystickZeroUp), joystick->values[ODROID_INPUT_UP] ? 1 : 0);
    ev.set(Event::Type(Event::JoystickZeroDown), joystick->values[ODROID_INPUT_DOWN] ? 1 : 0);
    ev.set(Event::Type(Event::JoystickZeroLeft), joystick->values[ODROID_INPUT_LEFT] ? 1 : 0);
    ev.set(Event::Type(Event::JoystickZeroRight), joystick->values[ODROID_INPUT_RIGHT] ? 1 : 0);
    ev.set(Event::Type(Event::JoystickZeroFire), joystick->values[ODROID_INPUT_A] ? 1 : 0);
    /*    ev.set(Event::Type(Event::ConsoleLeftDiffA),  joystick_state & (1 << RETRO_DEVICE_ID_JOYPAD_L));
        ev.set(Event::Type(Event::ConsoleLeftDiffB),  joystick_state & (1 << RETRO_DEVICE_ID_JOYPAD_L2));
        ev.set(Event::Type(Event::ConsoleColor),      joystick_state & (1 << RETRO_DEVICE_ID_JOYPAD_L3));
        ev.set(Event::Type(Event::ConsoleRightDiffA), joystick_state & (1 << RETRO_DEVICE_ID_JOYPAD_R));
        ev.set(Event::Type(Event::ConsoleRightDiffB), joystick_state & (1 << RETRO_DEVICE_ID_JOYPAD_R2));
        ev.set(Event::Type(Event::ConsoleBlackWhite), joystick_state & (1 << RETRO_DEVICE_ID_JOYPAD_R3));*/
    ev.set(Event::Type(Event::ConsoleSelect), joystick->values[ODROID_INPUT_SELECT] || joystick->values[ODROID_INPUT_Y]);
    ev.set(Event::Type(Event::ConsoleReset), joystick->values[ODROID_INPUT_START] || joystick->values[ODROID_INPUT_X]);

    console->controller(Controller::Left).update();
    console->switches().update();
}

static void convert_palette(const uint32_t *palette32, uint16_t *palette16)
{
    size_t i;
    for (i = 0; i < 256; i++)
    {
        uint32_t color32 = *(palette32 + i);
        *(palette16 + i) = ((color32 & 0xF80000) >> 8) |
                           ((color32 & 0x00F800) >> 5) |
                           ((color32 & 0x0000F8) >> 3);
    }
}

static void blend_frames_16(uInt8 *stella_fb, int width, int height)
{
    if (width > 320)
        width = 320;
    if (height > 240)
        height = 240;

    const uint32_t *palette32 = console->getPalette(0);
    uint16_t *palette16 = currentPalette16;
    uInt8 *in = stella_fb;
    uint16_t *out = (uint16_t *)lcd_get_active_buffer();
    int x, y;
//    int yoffset = (240 - height) / 2;

    /* If palette has changed, re-cache converted
     * RGB565 values */
    if (palette32 != currentPalette32)
    {
        currentPalette32 = palette32;
        convert_palette(palette32, palette16);
    }

    /*   for (y=0; y < height; y++) {
        for (x = 0; x < width; x++) {
          *(out+y*320+x) = *(palette16 + *(in++));
        }
       }*/

    for (y = 0; y < height; y++)
    {
        for (x = 0; x < width; x++)
        {
            *(out + y * 320 + 2 * x) = *(palette16 + *(in++));
            *(out + y * 320 + 2 * x + 1) = *(out + y * 320 + 2 * x);
        }
    }
    /*   for (y=yoffset; y < height+yoffset; y++) {
        for (x = 0; x < width; x++) {
          *(out+y*320+2*x) = *(palette16 + *(in++));
          *(out+y*320+2*x+1) = *(out+y*320+2*x);
        }
       }*/
}

static void sound_store(int16_t *audio_out_buf, uint16_t length)
{
    // MUST shift with at least 1 place, or it will brownout.
    uint8_t volume = odroid_audio_volume_get();
    int32_t factor = volume_tbl[volume];
    int16_t *audio_in_buf = sampleBuffer;

    if (audio_mute || (volume == ODROID_AUDIO_VOLUME_MIN))
    {
        // mute
        for (int i = 0; i < length; i++)
        {
            audio_out_buf[i] = 0;
        }
        return;
    }

    // Write to DMA buffer and lower the volume accordingly
    for (int i = 0; i < length; i++)
    {
        int32_t sample = *audio_in_buf++;
        audio_out_buf[i] = (sample * factor) >> 8;
    }
}

static void blit()
{
    blend_frames_16(console->tia().currentFrameBuffer(), console->tia().width(), console->tia().height());
}

static void app_main_a2600_cpp(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    size_t offset;
    odroid_gamepad_state_t joystick;
    odroid_dialog_choice_t options[] = {
        ODROID_DIALOG_CHOICE_LAST};
    uint32_t rom_length = 0;
    uint8_t *rom_ptr = NULL;

    if (start_paused)
    {
        common_emu_state.pause_after_frames = 2;
    }
    else
    {
        common_emu_state.pause_after_frames = 0;
    }

    printf("cartType = %s\n", cartType.c_str());
    string cartId;
    settings = new Settings(&osystem);
    //   settings->setValue("romloadcount", false);
    rom_length = getromdata(&rom_ptr);
    string cartMD5 = MD5((const uInt8*)rom_ptr, (uInt32)rom_length);

    fill_stella_config(cartMD5);

    string cartMD52 = "";
    cartridge = Cartridge::create((const uInt8 *)rom_ptr, (uInt32)rom_length, cartMD52, cartType, cartId, osystem, *settings);

    if (cartridge == 0)
    {
        printf("Stella: Failed to load cartridge.\n");
        return;
    }

    // Create the console
    console = new Console(&osystem, cartridge);
    osystem.myConsole = console;

    // Init sound and video
    console->initializeVideo();
    console->initializeAudio();

    // Get the ROM's width and height
    TIA &tia = console->tia();

    videoWidth = tia.width();
    videoHeight = tia.height();
    static uint32_t tiaSamplesPerFrame = (uint32_t)(AUDIO_A2600_SAMPLE_RATE / console->getFramerate());

    printf("videoWidth %d videoHeight %d\n", videoWidth, videoHeight);

    // G&W init
    common_emu_state.frame_time_10us = (uint16_t)(100000 / console->getFramerate() + 0.5f);

    // Black background
    memset(framebuffer1, 0, sizeof(framebuffer1));
    memset(framebuffer2, 0, sizeof(framebuffer2));

    odroid_system_init(APPID_A2600, AUDIO_A2600_SAMPLE_RATE);
    odroid_system_emu_init(&LoadState, &SaveState, &Screenshot, NULL);

    /* Set initial digital sensitivity */
    Paddles::setDigitalSensitivity(paddle_digital_sensitivity);

    if (load_state) {
        odroid_system_emu_load_state(save_slot);
    } else {
        lcd_clear_buffers();
    }

    // Init Sound
    audio_start_playing(tiaSamplesPerFrame);

    while (1)
    {
        wdog_refresh();
        common_emu_frame_loop();
        odroid_input_read_gamepad(&joystick);
        common_emu_input_loop(&joystick, options, &blit);

        uint8_t turbo_buttons = odroid_settings_turbo_buttons_get();
        bool turbo_a = (joystick.values[ODROID_INPUT_A] && (turbo_buttons & 1));
        bool turbo_b = (joystick.values[ODROID_INPUT_B] && (turbo_buttons & 2));
        bool turbo_button = odroid_button_turbos();
        if (turbo_a)
            joystick.values[ODROID_INPUT_A] = turbo_button;
        if (turbo_b)
            joystick.values[ODROID_INPUT_B] = !turbo_button;

        update_joystick(&joystick);

        tia.update();

        blit();
        common_ingame_overlay();
        osystem.sound().processFragment(sampleBuffer, tiaSamplesPerFrame);

        offset = (dma_state == DMA_TRANSFER_STATE_HF) ? 0 : tiaSamplesPerFrame;

        lcd_swap();
        sound_store(&audiobuffer_dma[offset], tiaSamplesPerFrame);

        common_emu_sound_sync(false);
    }
}

extern "C" int app_main_a2600(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    // Call static c++ constructors now, *after* OSPI and other memory is copied
    // Do not use __libc_init_array() as it will not work with the overlay
    cpp_init_array(__init_array_a2600_start__, __init_array_a2600_end__);

    app_main_a2600_cpp(load_state, start_paused, save_slot);

    return 0;
}
