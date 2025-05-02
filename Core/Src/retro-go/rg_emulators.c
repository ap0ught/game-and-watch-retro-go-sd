#include <odroid_system.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "gw_linker.h"
#include "gw_malloc.h"
#include "rg_emulators.h"
#include "rg_i18n.h"
#include "bitmaps.h"
#include "gui.h"
#include "rom_manager.h"
#include "gw_lcd.h"
#include "main.h"
#include "main_gb_tgbdual.h"
#include "main_nes.h"
#include "main_nes_fceu.h"
#include "main_smsplusgx.h"
#include "main_pce.h"
#include "main_msx.h"
#include "main_gw.h"
#include "main_wsv.h"
#include "main_gwenesis.h"
#include "main_a7800.h"
#include "main_amstrad.h"
#include "main_zelda3.h"
#include "main_smw.h"
#include "main_videopac.h"
#include "main_celeste.h"
#include "main_tama.h"
#include "main_pkmini.h"
#include "main_a2600.h"
#include "rg_rtc.h"
#include "heap.hpp"
#include "gw_flash.h"
#include "gw_flash_alloc.h"


const unsigned char *ROM_DATA = NULL;
unsigned ROM_DATA_LENGTH;
const char *ROM_EXT = NULL;
retro_emulator_file_t *ACTIVE_FILE = NULL;

static retro_emulator_file_t *shared_files = NULL;

#if !defined(COVERFLOW)
#define COVERFLOW 0
#endif /* COVERFLOW */
// Increase when adding new emulators
#define MAX_EMULATORS 17
static retro_emulator_t emulators[MAX_EMULATORS];
static rom_system_t systems[MAX_EMULATORS];
static int emulators_count = 0;

#if CHEAT_CODES == 1
static retro_emulator_file_t *CHOSEN_FILE = NULL;
#endif

static void event_handler(gui_event_t event, tab_t *tab)
{
    retro_emulator_t *emu = (retro_emulator_t *)tab->arg;
    listbox_item_t *item = gui_get_selected_item(tab);
    retro_emulator_file_t *file = (retro_emulator_file_t *)(item ? item->arg : NULL);

    if (event == TAB_INIT)
    {
        emulator_init(emu);

        if (emu->roms.count > 0)
        {
            sprintf(tab->status, "%s", emu->system_name);
            gui_resize_list(tab, emu->roms.count);

            for (int i = 0; i < emu->roms.count; i++)
            {
                tab->listbox.items[i].text = emu->roms.files[i].name;
                tab->listbox.items[i].arg = (void *)&emu->roms.files[i];
            }

            gui_sort_list(tab, 0);
            tab->is_empty = false;
        }
        else
        {
            sprintf(tab->status, " No games");
            gui_resize_list(tab, 8);
            //size_t len = 0;
            //tab->listbox.items[0].text = asnprintf(NULL, &len, "Place roms in folder: /roms/%s", emu->dirname);
            //len = 0;
            //tab->listbox.items[2].text = asnprintf(NULL, &len, "With file extension: .%s", emu->ext);
            //tab->listbox.items[4].text = "Use SELECT and START to navigate.";
            tab->listbox.cursor = 3;
            tab->is_empty = true;
        }
    }
    else if (event == TAB_REFRESH_LIST)
    {
        emu->roms.count = 0;
        emulator_refresh_list(emu);
        if (emu->roms.count > 0)
        {
            sprintf(tab->status, "%s", emu->system_name);
            gui_resize_list(tab, emu->roms.count);

            for (int i = 0; i < emu->roms.count; i++)
            {
                tab->listbox.items[i].text = emu->roms.files[i].name;
                tab->listbox.items[i].arg = (void *)&emu->roms.files[i];
            }

            gui_sort_list(tab, 0);
            tab->is_empty = false;
        }
        else
        {
            sprintf(tab->status, " No games");
            gui_resize_list(tab, 8);
            //size_t len = 0;
            //tab->listbox.items[0].text = asnprintf(NULL, &len, "Place roms in folder: /roms/%s", emu->dirname);
            //len = 0;
            //tab->listbox.items[2].text = asnprintf(NULL, &len, "With file extension: .%s", emu->ext);
            //tab->listbox.items[4].text = "Use SELECT and START to navigate.";
            tab->listbox.cursor = 3;
            tab->is_empty = true;
        }
    }

    /* The rest of the events require a file to be selected */
    if (file == NULL)
        return;

    if (event == KEY_PRESS_A)
    {
        emulator_show_file_menu(file);
    }
    else if (event == KEY_PRESS_B)
    {
        emulator_show_file_info(file);
    }
    else if (event == TAB_IDLE)
    {
    }
    else if (event == TAB_REDRAW)
    {
    }
}

static void add_emulator(const char *system, const char *dirname, const char* ext,
                         uint16_t logo_idx, uint16_t header_idx, game_data_type_t game_data_type)
{
    assert(emulators_count <= MAX_EMULATORS);
    retro_emulator_t *p = &emulators[emulators_count];
    rom_system_t *s = &systems[emulators_count];
    emulators_count++;

    strcpy(p->system_name, system);
    strcpy(p->dirname, dirname);
    snprintf(p->exts, sizeof(p->exts), " %s ", ext);
    p->roms.count = 0;
    p->roms.maxcount = 1000;
    if (shared_files == NULL)
    {
        shared_files = ram_calloc(p->roms.maxcount, sizeof(retro_emulator_file_t));
    }
    p->roms.files = shared_files;
    p->initialized = false;
    p->system = s;

    s->extension = (char *)ext;
    s->roms = p->roms.files;
    s->roms_count = p->roms.count;
    s->system_name = (char *)system;
    s->game_data_type = game_data_type;

    gui_add_tab(dirname, logo_idx, header_idx, p, event_handler);
}

static void remove_extension(const char *path, char *new_path) {
    // we can assume an extension is always present
    const char *last_dot = strrchr(path, '.');

    size_t new_len = last_dot - path;

    if (!new_path) return;

    memcpy(new_path, path, new_len);
    new_path[new_len] = '\0';
}

static const char *get_extension(const char *filename) {
    const char *extension = strrchr(filename, '.');
    
    if (extension && extension != filename) {
        return extension + 1;
    }

    return NULL;
}

static int scan_folder_cb(const rg_scandir_t *entry, void *arg)
{
    retro_emulator_t *emu = (retro_emulator_t *)arg;
    const char *ext = rg_extension(entry->basename);
    uint8_t is_valid = false;
    char ext_buf[32];

    // Skip hidden files
    if (entry->basename[0] == '.')
        return RG_SCANDIR_SKIP;

    if (entry->is_file && ext[0])
    {
        snprintf(ext_buf, sizeof(ext_buf), " %s ", ext);
        is_valid = strstr(emu->exts, rg_strtolower(ext_buf)) != NULL;
    }
    else if (entry->is_dir)
    {
        is_valid = true;
    }

    if (!is_valid)
        return RG_SCANDIR_CONTINUE;

    if (emu->roms.count + 1 > emu->roms.maxcount)
    {
        return RG_SCANDIR_STOP;
    }

    emu->roms.files[emu->roms.count] = (retro_emulator_file_t) {
        .address = 0,
        .size = entry->size,
        .system = emu->system,
        .region = REGION_NTSC,
#if COVERFLOW != 0
        .img_state = IMG_STATE_UNKNOWN,
#endif
    };
    remove_extension(entry->basename, emu->roms.files[emu->roms.count].name);
    strcpy(emu->roms.files[emu->roms.count].path, entry->path);
    // Make sure ext points to a memory zone that will not be freed.
    emu->roms.files[emu->roms.count].ext = (char *)get_extension(emu->roms.files[emu->roms.count].path);
#if CHEAT_CODES == 1
    emu->roms.files[emu->roms.count].cheat_count = 0;
    emu->roms.files[emu->roms.count].cheat_codes = NULL;
    emu->roms.files[emu->roms.count].cheat_descs = NULL;
#endif

    emu->roms.count++;
    
    emu->system->roms_count = emu->roms.count;

    return RG_SCANDIR_CONTINUE;
}

void emulator_init(retro_emulator_t *emu)
{
    char folder[RG_PATH_MAX];

    if (emu->initialized)
        return;

    emu->initialized = true;
#if COVERFLOW != 0
    emu->cover_height = 0;
    emu->cover_width = 0;
#endif

    printf("Retro-Go: Initializing emulator '%s'\n", emu->system_name);

    sprintf(folder, ODROID_BASE_PATH_SAVES "/%s", emu->dirname);
    rg_storage_mkdir(folder);

    sprintf(folder, ODROID_BASE_PATH_ROMS "/%s", emu->dirname);
    rg_storage_mkdir(folder);

    snprintf(folder, sizeof(folder), "%s/%s", RG_BASE_PATH_ROMS, emu->dirname);
    rg_storage_scandir(folder, scan_folder_cb, emu, RG_SCANDIR_RECURSIVE);
}

void emulator_refresh_list(retro_emulator_t *emu)
{
    char folder[RG_PATH_MAX];

    sprintf(folder, ODROID_BASE_PATH_ROMS "/%s", emu->dirname);
    rg_storage_mkdir(folder);

    snprintf(folder, sizeof(folder), "%s/%s", RG_BASE_PATH_ROMS, emu->dirname);
    rg_storage_scandir(folder, scan_folder_cb, emu, RG_SCANDIR_RECURSIVE);

}

void emulator_show_file_info(retro_emulator_file_t *file)
{
    char filename_value[128];
    char type_value[32];
    char size_value[32];

    odroid_dialog_choice_t choices[] = {
        {-1, curr_lang->s_File, filename_value, 0, NULL},
        {-1, curr_lang->s_Type, type_value, 0, NULL},
        {-1, curr_lang->s_Size, size_value, 0, NULL},
        ODROID_DIALOG_CHOICE_SEPARATOR,
        {1, curr_lang->s_Close, "", 1, NULL},
        ODROID_DIALOG_CHOICE_LAST
    };

    sprintf(choices[0].value, "%.127s", file->name);
    sprintf(choices[1].value, "%s", file->ext);
    sprintf(choices[2].value, "%d KB", (int)(file->size / 1024));

    odroid_overlay_dialog(curr_lang->s_GameProp, choices, -1, &gui_redraw_callback);
}

#if CHEAT_CODES == 1
static bool cheat_update_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event, uint32_t repeat)
{
    bool is_on = odroid_settings_ActiveGameGenieCodes_is_enabled(CHOSEN_FILE->path, option->id);
    if (event == ODROID_DIALOG_PREV || event == ODROID_DIALOG_NEXT) 
    {
        is_on = is_on ? false : true;
        odroid_settings_ActiveGameGenieCodes_set(CHOSEN_FILE->path, option->id, is_on);
    }
    strcpy(option->value, is_on ? curr_lang->s_Cheat_Codes_ON : curr_lang->s_Cheat_Codes_OFF);
    return event == ODROID_DIALOG_ENTER;
}

static bool show_cheat_dialog()
{
    static odroid_dialog_choice_t last = ODROID_DIALOG_CHOICE_LAST;

    // +1 for the terminator sentinel
    odroid_dialog_choice_t *choices = rg_alloc((CHOSEN_FILE->cheat_count + 1) * sizeof(odroid_dialog_choice_t), MEM_ANY);
    char svalues[MAX_CHEAT_CODES][10];
    for(int i=0; i<CHOSEN_FILE->cheat_count; i++) 
    {
        const char *label = CHOSEN_FILE->cheat_descs[i];
        if (label == NULL) {
            label = CHOSEN_FILE->cheat_codes[i];
        }
        choices[i].id = i;
        choices[i].label = label;
        choices[i].value = svalues[i];
        choices[i].enabled = 1;
        choices[i].update_cb = cheat_update_cb;
    }
    choices[CHOSEN_FILE->cheat_count] = last;
    odroid_overlay_dialog(curr_lang->s_Cheat_Codes_Title, choices, 0, NULL);

    rg_free(choices);
    odroid_settings_commit();
    return false;
}

void emulator_update_cheats_info(retro_emulator_file_t *file) {
    if (file->cheat_codes) {
        return;
    }

    // Check for pceplus cheat file (PC Engine)
    char *cheat_path = odroid_system_get_path(ODROID_PATH_CHEAT_PCE, file->path);
    if (odroid_sdcard_get_filesize(cheat_path) > 0) {
        printf("Retro-Go: Found cheat file %s\n", cheat_path);
        file->cheat_codes = calloc(MAX_CHEAT_CODES, sizeof(char *));
        file->cheat_descs = calloc(MAX_CHEAT_CODES, sizeof(char *));
        FILE *cheat_file = fopen(cheat_path, "r");
        if (!cheat_file) {
            printf("Retro-Go: Failed to open cheat file %s\n", cheat_path);
            return;
        }
        char line[256];
        while (fgets(line, sizeof(line), cheat_file)) {
            char *trimmed_line = strtok(line, "\n");
            if (!trimmed_line || trimmed_line[0] == '#' ||
                (trimmed_line[0] == '/' && trimmed_line[1] == '/')) {
                continue;
            }

            char *parts[10];
            uint8_t part_count = 0;
            char *token = strtok(trimmed_line, ",");
            while (token && part_count < 10) {
                parts[part_count++] = token;
                token = strtok(NULL, ",");
            }

            if (part_count < 2) {
                continue;
            }

            int cmd_count = 0;
            file->cheat_codes[file->cheat_count] = malloc((size_t)(1 + 4 * (part_count-1)));
            char *codes_ptr = (char *)file->cheat_codes[file->cheat_count];
            *(codes_ptr++)=part_count - 1;
            for (int i = 0; i < part_count - 1; i++) {
                char *part = parts[i];
                int x = (int)strtol(part, NULL, 16);
                printf("x = %x\n", x);
                *(codes_ptr++)=x>>24;
                *(codes_ptr++)=(x>>16)&0xFF;
                *(codes_ptr++)=(x>>8)&0xFF;
                *(codes_ptr++)=x&0xFF;
                cmd_count++;
            }

            char *desc = parts[part_count - 1];
            if (desc) {
                while (*desc == ' ') desc++;
                desc = strndup(desc, 40);
            }

            if (file->cheat_count < MAX_CHEAT_CODES) {
                file->cheat_descs[file->cheat_count] = desc;
                file->cheat_count++;
            } else {
                printf("INFO: More than %d cheat codes...\n", MAX_CHEAT_CODES);
                break;
            }
        }
        fclose(cheat_file);
    }
    free(cheat_path);
    if (file->cheat_count)
        return;

    // Check for ggcodes cheat file (GB/GBC/NES)
    cheat_path = odroid_system_get_path(ODROID_PATH_CHEAT_GAME_GENIE, file->path);
    if (odroid_sdcard_get_filesize(cheat_path) > 0) {
        printf("Retro-Go: Found cheat file %s\n", cheat_path);
        file->cheat_codes = calloc(MAX_CHEAT_CODES, sizeof(char *));
        file->cheat_descs = calloc(MAX_CHEAT_CODES, sizeof(char *));
        FILE *cheat_file = fopen(cheat_path, "r");
        if (!cheat_file) {
            printf("Retro-Go: Failed to open cheat file %s\n", cheat_path);
            return;
        }
        char line[256];
        while (fgets(line, sizeof(line), cheat_file)) {
            char *trimmed_line = strtok(line, "\n");
            if (!trimmed_line || trimmed_line[0] == '#' ||
                (trimmed_line[0] == '/' && trimmed_line[1] == '/')) {
                continue;
            }

            char *parts[10];
            int part_count = 0;
            char *token = strtok(trimmed_line, ",");
            while (token && part_count < 10) {
                parts[part_count++] = token;
                token = strtok(NULL, ",");
            }
            printf("Retro-Go: Part count: %d\n", part_count);
            for (int i = 0; i < part_count; i++) {
                printf("Retro-Go: Part %d: %s\n", i, parts[i]);
            }

            file->cheat_codes[file->cheat_count] = strdup(parts[0]);

            char *desc = parts[part_count - 1];
            if (desc) {
                while (*desc == ' ') desc++; // Remove leading spaces
                desc = strndup(desc, 40);
            }

            if (file->cheat_count < MAX_CHEAT_CODES) {
                file->cheat_descs[file->cheat_count] = desc;
                file->cheat_count++;
            } else {
                printf("INFO: More than %d cheat codes...\n", MAX_CHEAT_CODES);
                break;
            }
        }
        fclose(cheat_file);
    }
    free(cheat_path);
    if (file->cheat_count)
        return;

    // Check for mfc cheat file (MSX)
    cheat_path = odroid_system_get_path(ODROID_PATH_CHEAT_MFC, file->path);
    if (odroid_sdcard_get_filesize(cheat_path) > 0) {
        printf("Retro-Go: Found cheat file %s\n", cheat_path);
        file->cheat_codes = calloc(MAX_CHEAT_CODES, sizeof(char *));
        file->cheat_descs = calloc(MAX_CHEAT_CODES, sizeof(char *));

        FILE *cheat_file = fopen(cheat_path, "r");
        if (!cheat_file) {
            printf("Retro-Go: Failed to open cheat file %s\n", cheat_path);
            return;
        }

        char line[256];
        while (fgets(line, sizeof(line), cheat_file)) {
            if (line[0] == '!') continue;
            char *last_comma = strrchr(line, ',');
            if (!last_comma) continue;
            *last_comma = '\0';

            printf("MFC: cheat: %s\n", line);
            printf("MFC: desc: %s\n", last_comma + 1);
            if (file->cheat_count < MAX_CHEAT_CODES) {
                file->cheat_codes[file->cheat_count] = strdup(line);
                file->cheat_descs[file->cheat_count] = strdup(last_comma + 1);
                file->cheat_count++;
            } else {
                printf("INFO: More than %d cheat codes...\n", MAX_CHEAT_CODES);
                break;
            }
        }
    }
    free(cheat_path);
}
#endif

bool emulator_show_file_menu(retro_emulator_file_t *file)
{
    int slot = -1;
    char *sram_path = odroid_system_get_path(ODROID_PATH_SAVE_SRAM, file->path);
    rg_emu_states_t *savestates = odroid_system_emu_get_states(file->path, 4);
    bool has_save = savestates->used > 0;
    bool has_sram = odroid_sdcard_get_filesize(sram_path) > 0;
//    bool is_fav = favorite_find(file) != NULL;
    bool force_redraw = false;

#if CHEAT_CODES == 1
    // Free previous cheat codes
    if (CHOSEN_FILE) {
        for (int i = 0; i < CHOSEN_FILE->cheat_count; i++) {
            if (CHOSEN_FILE->cheat_codes[i]) free(CHOSEN_FILE->cheat_codes[i]);
            if (CHOSEN_FILE->cheat_descs[i]) free(CHOSEN_FILE->cheat_descs[i]);
        }
        free(CHOSEN_FILE->cheat_codes);
        free(CHOSEN_FILE->cheat_descs);
    }

    CHOSEN_FILE = file;
    emulator_update_cheats_info(CHOSEN_FILE);
    odroid_dialog_choice_t last = ODROID_DIALOG_CHOICE_LAST;
    odroid_dialog_choice_t cheat_row = {4, curr_lang->s_Cheat_Codes, "", 1, NULL};
    odroid_dialog_choice_t cheat_choice = last; 
    if (CHOSEN_FILE->cheat_count != 0) {
        cheat_choice = cheat_row;
    }
#endif

    odroid_dialog_choice_t choices[] = {
        {0, curr_lang->s_Resume_game, "", (has_save || has_sram) ? 1:-1, NULL},
        {1, curr_lang->s_New_game, "", 1, NULL},
        ODROID_DIALOG_CHOICE_SEPARATOR,
//        {3, is_fav ? "Del favorite" : "Add favorite", "", 1, NULL},
        {2, curr_lang->s_Delete_save, "", (has_save || has_sram) ? 1 : -1, NULL},
#if CHEAT_CODES == 1
        ODROID_DIALOG_CHOICE_SEPARATOR,
        cheat_choice,
#endif
        ODROID_DIALOG_CHOICE_LAST
    };

#if CHEAT_CODES == 1
    if (CHOSEN_FILE->cheat_count == 0)
        choices[4] = last;
#endif

    int sel = odroid_overlay_dialog(file->name, choices, has_save ? 0 : 1, &gui_redraw_callback);

    if (sel == 0) { // Resume game
        if (has_save) {
            if ((slot = odroid_savestate_menu(curr_lang->s_Resume_game, file->path, true, &gui_redraw_callback)) != -1) {
                gui_save_current_tab();
                emulator_start(file, true, false, slot);
            }
        } else if (has_sram) {
            gui_save_current_tab();
            emulator_start(file, true, false, -2);
        }
    }
    if (sel == 1) { // New game
        gui_save_current_tab();
        emulator_start(file, false, false, 0);
    }
    else if (sel == 2) {
        while ((savestates->used > 0) &&
               ((slot = odroid_savestate_menu(curr_lang->s_Confirm_del_save, file->path, true, &gui_redraw_callback)) != -1))
        {
            odroid_sdcard_unlink(savestates->slots[slot].preview);
            odroid_sdcard_unlink(savestates->slots[slot].file);
            savestates->slots[slot].is_used = false;
            savestates->used--;
        }
        if (has_sram && odroid_overlay_confirm(curr_lang->s_Confirm_del_sram, false, &gui_redraw_callback))
        {
            odroid_sdcard_unlink(sram_path);
        }
    }
/*    else if (sel == 3) {
        if (is_fav)
            favorite_remove(file);
        else
            favorite_add(file);
    }*/
#if CHEAT_CODES == 1
    else if (sel == 4) {
        if (CHOSEN_FILE->cheat_count != 0)
            show_cheat_dialog();
        force_redraw = true;
    }
#endif

    free(sram_path);
    free(savestates);

#if CHEAT_CODES == 1
    CHOSEN_FILE = NULL;
#endif

    return force_redraw;
}

typedef int func(void);
extern LTDC_HandleTypeDef hltdc;

void emulator_start(retro_emulator_file_t *file, bool load_state, bool start_paused, int8_t save_slot)
{
    printf("Retro-Go: Starting game: %s\n", file->name);
    // odroid_settings_StartAction_set(load_state ? ODROID_START_ACTION_RESUME : ODROID_START_ACTION_NEWGAME);
    // odroid_settings_commit();

    // create a copy in heap ram as ram used by ram_malloc will be erase by emulator
    retro_emulator_file_t *newfile = calloc(sizeof(retro_emulator_file_t),1);
    memcpy(newfile,file,sizeof(retro_emulator_file_t));
    strcpy((char *)newfile->name,file->name);
    strcpy(newfile->path,file->path);
    newfile->ext = get_extension(newfile->path);

    const char *system_name = newfile->system->system_name;

    ACTIVE_FILE = newfile;
#if CHEAT_CODES == 1
    CHOSEN_FILE = newfile;

    emulator_update_cheats_info(CHOSEN_FILE);
#endif

    // Copy game data from SD card to flash if needed
    // dsk files are read from sd card, do not copy them in flash
    if ((newfile->system->game_data_type != NO_GAME_DATA) && (strcasecmp(newfile->ext, "dsk") !=0)) {
        newfile->address = odroid_overlay_cache_file_in_flash(newfile->path, &(newfile->size), newfile->system->game_data_type == GAME_DATA_BYTESWAP_16);
        ROM_DATA = newfile->address;
        ROM_EXT = newfile->ext;
        ROM_DATA_LENGTH = newfile->size;

        if (newfile->address == NULL) {
            // Rom was not loaded in flash, do not start emulator
            return;
        }
    }

    // It will free all ram allocated memory for use by emulators
    ahb_init();
    itc_init();
    ram_start = 0;
    // some pointers were freed, set them to null
#if SD_CARD == 1
    clear_flash_alloc_metadata();
    rg_reset_logo_buffers();
#endif

    // Refresh watchdog here in case previous actions did not refresh it
    wdog_refresh();

    if((strcmp(system_name, "Nintendo Gameboy") == 0) ||
       (strcmp(system_name, "Nintendo Gameboy Color") == 0)) {
        if (odroid_overlay_cache_file_in_ram("/cores/tgb.bin", (uint8_t *)&__RAM_EMU_START__)) {
            memset(&_OVERLAY_TGB_BSS_START, 0x0, (size_t)&_OVERLAY_TGB_BSS_SIZE);
            SCB_CleanDCache_by_Addr((uint32_t *)&__RAM_EMU_START__, (size_t)&_OVERLAY_TGB_SIZE);

            // Initializes the heap used by new and new[]
            cpp_heap_init((size_t) &_OVERLAY_TGB_BSS_END);

            app_main_gb_tgbdual(load_state, start_paused, save_slot);
        }
    } else if(strcmp(system_name, "Nintendo Entertainment System") == 0) {
#if FORCE_NOFRENDO == 1
        if (odroid_overlay_cache_file_in_ram("/cores/nes.bin", (uint8_t *)&__RAM_EMU_START__)) {
            memset(&_OVERLAY_NES_BSS_START, 0x0, (size_t)&_OVERLAY_NES_BSS_SIZE);
            SCB_CleanDCache_by_Addr((uint32_t *)&__RAM_EMU_START__, (size_t)&_OVERLAY_NES_SIZE);
            app_main_nes(load_state, start_paused, save_slot);
        }
#else
        if (odroid_overlay_cache_file_in_ram("/cores/nes_fceu.bin", (uint8_t *)&__RAM_FCEUMM_START__)) {
            memset(&_OVERLAY_NES_FCEU_BSS_START, 0x0, (size_t)&_OVERLAY_NES_FCEU_BSS_SIZE);
            SCB_CleanDCache_by_Addr((uint32_t *)&__RAM_EMU_START__, (size_t)&_OVERLAY_NES_FCEU_SIZE);
            app_main_nes_fceu(load_state, start_paused, save_slot);
        }
#endif
    } else if(strcmp(system_name, "Sega Master System") == 0 ||
              strcmp(system_name, "Sega Game Gear") == 0     ||
              strcmp(system_name, "Sega SG-1000") == 0       ||
              strcmp(system_name, "Colecovision") == 0 ) {
        if (odroid_overlay_cache_file_in_ram("/cores/sms.bin", (uint8_t *)&__RAM_EMU_START__)) {
            memset(&_OVERLAY_SMS_BSS_START, 0x0, (size_t)&_OVERLAY_SMS_BSS_SIZE);
            SCB_CleanDCache_by_Addr((uint32_t *)&__RAM_EMU_START__, (size_t)&_OVERLAY_SMS_SIZE);
            if (! strcmp(system_name, "Colecovision")) app_main_smsplusgx(load_state, start_paused, save_slot, SMSPLUSGX_ENGINE_COLECO);
            else
            if (! strcmp(system_name, "Sega SG-1000")) app_main_smsplusgx(load_state, start_paused, save_slot, SMSPLUSGX_ENGINE_SG1000);
            else                                            app_main_smsplusgx(load_state, start_paused, save_slot, SMSPLUSGX_ENGINE_OTHERS);
        }
    } else if(strcmp(system_name, "Game & Watch") == 0 ) {
        if (odroid_overlay_cache_file_in_ram("/cores/gw.bin", (uint8_t *)&__RAM_EMU_START__)) {
            memset(&_OVERLAY_GW_BSS_START, 0x0, (size_t)&_OVERLAY_GW_BSS_SIZE);
            SCB_CleanDCache_by_Addr((uint32_t *)&__RAM_EMU_START__, (size_t)&_OVERLAY_GW_SIZE);
            app_main_gw(load_state, save_slot);
        }
    } else if(strcmp(system_name, "PC Engine") == 0) {
      if (odroid_overlay_cache_file_in_ram("/cores/pce.bin", (uint8_t *)&__RAM_EMU_START__)) {
        memset(&_OVERLAY_PCE_BSS_START, 0x0, (size_t)&_OVERLAY_PCE_BSS_SIZE);
        SCB_CleanDCache_by_Addr((uint32_t *)&__RAM_EMU_START__, (size_t)&_OVERLAY_PCE_SIZE);
        app_main_pce(load_state, start_paused, save_slot);
      }
    } else if(strcmp(system_name, "MSX") == 0) {
      if (odroid_overlay_cache_file_in_ram("/cores/msx.bin", (uint8_t *)&__RAM_EMU_START__)) {
        memset(&_OVERLAY_MSX_BSS_START, 0x0, (size_t)&_OVERLAY_MSX_BSS_SIZE);
        SCB_CleanDCache_by_Addr((uint32_t *)&__RAM_EMU_START__, (size_t)&_OVERLAY_MSX_SIZE);
        app_main_msx(load_state, start_paused, save_slot);
      }
    } else if(strcmp(system_name, "Watara Supervision") == 0) {
      if (odroid_overlay_cache_file_in_ram("/cores/wsv.bin", (uint8_t *)&__RAM_EMU_START__)) {
        memset(&_OVERLAY_WSV_BSS_START, 0x0, (size_t)&_OVERLAY_WSV_BSS_SIZE);
        SCB_CleanDCache_by_Addr((uint32_t *)&__RAM_EMU_START__, (size_t)&_OVERLAY_WSV_SIZE);
        app_main_wsv(load_state, start_paused, save_slot);
      }
    } else if(strcmp(system_name, "Sega Genesis") == 0)  {
      if (odroid_overlay_cache_file_in_ram("/cores/md.bin", (uint8_t *)&__RAM_EMU_START__)) {
        memset(&_OVERLAY_MD_BSS_START, 0x0, (size_t)&_OVERLAY_MD_BSS_SIZE);
        SCB_CleanDCache_by_Addr((uint32_t *)&__RAM_EMU_START__, (size_t)&_OVERLAY_MD_SIZE);
        app_main_gwenesis(load_state, start_paused, save_slot);
      }
    } else if(strcmp(system_name, "Atari 2600") == 0) {
#ifdef ENABLE_EMULATOR_A2600
      if (odroid_overlay_cache_file_in_ram("/cores/a2600.bin", (uint8_t *)&__RAM_EMU_START__)) {
        memset(&_OVERLAY_A2600_BSS_START, 0x0, (size_t)&_OVERLAY_A2600_BSS_SIZE);
        SCB_CleanDCache_by_Addr((uint32_t *)&__RAM_EMU_START__, (size_t)&_OVERLAY_A2600_SIZE);

        // Initializes the heap used by new and new[]
        cpp_heap_init((size_t) &_OVERLAY_A2600_BSS_END);

        app_main_a2600(load_state, start_paused, save_slot);
      }
#endif
    } else if(strcmp(system_name, "Atari 7800") == 0)  {
      if (odroid_overlay_cache_file_in_ram("/cores/a7800.bin", (uint8_t *)&__RAM_EMU_START__)) {
        memset(&_OVERLAY_A7800_BSS_START, 0x0, (size_t)&_OVERLAY_A7800_BSS_SIZE);
        SCB_CleanDCache_by_Addr((uint32_t *)&__RAM_EMU_START__, (size_t)&_OVERLAY_A7800_SIZE);
        app_main_a7800(load_state, start_paused, save_slot);
      }
    } else if(strcmp(system_name, "Amstrad CPC") == 0)  {
      if (odroid_overlay_cache_file_in_ram("/cores/amstrad.bin", (uint8_t *)&__RAM_EMU_START__)) {
        memset(&_OVERLAY_AMSTRAD_BSS_START, 0x0, (size_t)&_OVERLAY_AMSTRAD_BSS_SIZE);
        SCB_CleanDCache_by_Addr((uint32_t *)&__RAM_EMU_START__, (size_t)&_OVERLAY_AMSTRAD_SIZE);
        app_main_amstrad(load_state, start_paused, save_slot);
      }
    } else if(strcmp(system_name, "Philips Vectrex") == 0)  {
#ifdef ENABLE_EMULATOR_VIDEOPAC
      if (odroid_overlay_cache_file_in_ram("/cores/videopac.bin", (uint8_t *)&__RAM_EMU_START__)) {
        memset(&_OVERLAY_VIDEOPAC_BSS_START, 0x0, (size_t)&_OVERLAY_VIDEOPAC_BSS_SIZE);
        SCB_CleanDCache_by_Addr((uint32_t *)&__RAM_EMU_START__, (size_t)&_OVERLAY_VIDEOPAC_SIZE);
        app_main_videopac(load_state, start_paused, save_slot);
      }
#endif
    } else if(strcmp(system_name, "Homebrew") == 0)  {
      if (odroid_overlay_cache_file_in_ram(ACTIVE_FILE->path, (uint8_t *)&__RAM_EMU_START__)) {
        if (strcmp(newfile->name,"celeste") == 0) {
            memset(&_OVERLAY_CELESTE_BSS_START, 0x0, (size_t)&_OVERLAY_CELESTE_BSS_SIZE);
            SCB_CleanDCache_by_Addr((uint32_t *)&__RAM_EMU_START__, (size_t)&_OVERLAY_CELESTE_SIZE);
            app_main_celeste(load_state, start_paused, save_slot);
        } else if (strcmp(newfile->name,"Zelda 3") == 0) {
            memset(&_OVERLAY_ZELDA3_BSS_START, 0x0, (size_t)&_OVERLAY_ZELDA3_BSS_SIZE);
            SCB_CleanDCache_by_Addr((uint32_t *)&__RAM_EMU_START__, (size_t)&_OVERLAY_ZELDA3_SIZE);
            app_main_zelda3(load_state, start_paused, save_slot);
        } else if (strcmp(newfile->name,"Super Mario World") == 0) {
            memset(&_OVERLAY_SMW_BSS_START, 0x0, (size_t)&_OVERLAY_SMW_BSS_SIZE);
            SCB_CleanDCache_by_Addr((uint32_t *)&__RAM_EMU_START__, (size_t)&_OVERLAY_SMW_SIZE);
            app_main_smw(load_state, start_paused, save_slot);
        }
#if 0
        uint32_t* ram_start = (uint32_t *)&__RAM_EMU_START__;
        uint32_t initial_sp = ram_start[0];
        uint32_t entry_point = ram_start[1];

        __disable_irq();
        __set_MSP(initial_sp);
        SCB->VTOR = 0x24000000;

        func* app_entry = (func*)entry_point;
        app_entry();
#endif
      }
    } else if(strcmp(system_name, "Tamagotchi") == 0) {
      if (odroid_overlay_cache_file_in_ram("/cores/tama.bin", (uint8_t *)&__RAM_EMU_START__)) {
        memset(&_OVERLAY_TAMA_BSS_START, 0x0, (size_t)&_OVERLAY_TAMA_BSS_SIZE);
        SCB_CleanDCache_by_Addr((uint32_t *)&__RAM_EMU_START__, (size_t)&_OVERLAY_TAMA_SIZE);
        app_main_tama(load_state, start_paused, save_slot);
      }
    } else if(strcmp(system_name, "Pokemon Mini") == 0) {
      if (odroid_overlay_cache_file_in_ram("/cores/pkmini.bin", (uint8_t *)&__RAM_EMU_START__)) {
        memset(&_OVERLAY_PKMINI_BSS_START, 0x0, (size_t)&_OVERLAY_PKMINI_BSS_SIZE);
        SCB_CleanDCache_by_Addr((uint32_t *)&__RAM_EMU_START__, (size_t)&_OVERLAY_PKMINI_SIZE);
        app_main_pkmini(load_state, start_paused, save_slot);
      }
    }

#if CHEAT_CODES == 1
    for (int i = 0; i < newfile->cheat_count; i++) {
        if (newfile->cheat_codes[i]) free(newfile->cheat_codes[i]);
        if (newfile->cheat_descs[i]) free(newfile->cheat_descs[i]);
    }
    if (newfile->cheat_codes) free(newfile->cheat_codes);
    if (newfile->cheat_descs) free(newfile->cheat_descs);
#endif
    free(newfile);

    ahb_init();
    itc_init();
    ram_start = 0;
#if SD_CARD == 1
    // some pointers were freed, set them to null
    clear_flash_alloc_metadata();
    rg_reset_logo_buffers();
#endif
}

void emulators_init()
{
    add_emulator("Nintendo Gameboy", "gb", "gb gbc", RG_LOGO_PAD_GB, RG_LOGO_HEADER_GB, NO_GAME_DATA);
    add_emulator("Nintendo Gameboy Color", "gbc", "gb gbc", RG_LOGO_PAD_GB, RG_LOGO_HEADER_GBC, NO_GAME_DATA);
    add_emulator("Nintendo Entertainment System", "nes", "nes fds nsf", RG_LOGO_PAD_NES, RG_LOGO_HEADER_NES, NO_GAME_DATA);
    add_emulator("Game & Watch", "gw", "gw", RG_LOGO_PAD_GW, RG_LOGO_HEADER_GW, NO_GAME_DATA);
    add_emulator("PC Engine", "pce", "pce", RG_LOGO_PAD_PCE, RG_LOGO_HEADER_PCE, NO_GAME_DATA);
    add_emulator("Sega Game Gear", "gg", "gg", RG_LOGO_PAD_GG, RG_LOGO_HEADER_GG, NO_GAME_DATA);
    add_emulator("Sega Master System", "sms", "sms", RG_LOGO_PAD_SMS, RG_LOGO_HEADER_SMS, NO_GAME_DATA);
    add_emulator("Sega Genesis", "md", "md gen bin", RG_LOGO_PAD_GEN, RG_LOGO_HEADER_GEN, GAME_DATA_BYTESWAP_16);
    add_emulator("Sega SG-1000", "sg", "sg", RG_LOGO_PAD_SG1000, RG_LOGO_HEADER_SG1000, NO_GAME_DATA);
    add_emulator("Colecovision", "col", "col", RG_LOGO_PAD_COL, RG_LOGO_HEADER_COL, NO_GAME_DATA);
    add_emulator("Watara Supervision", "wsv", "wsv sv bin", RG_LOGO_PAD_WSV, RG_LOGO_HEADER_WSV, NO_GAME_DATA);
    add_emulator("MSX", "msx", "dsk rom mx1 mx2", RG_LOGO_PAD_MSX, RG_LOGO_HEADER_MSX, NO_GAME_DATA);
//    add_emulator("Atari 2600", "a2600", "a2600", RG_LOGO_PAD_A7800, RG_LOGO_HEADER_A7800, GAME_DATA); // TODO : add specific gfx
    add_emulator("Atari 7800", "a7800", "a78", RG_LOGO_PAD_A7800, RG_LOGO_HEADER_A7800, NO_GAME_DATA);
    add_emulator("Amstrad CPC", "amstrad", "dsk", RG_LOGO_PAD_AMSTRAD, RG_LOGO_HEADER_AMSTRAD, NO_GAME_DATA);
//    add_emulator("Philips Vectrex", "videopac", "bin", "o2em-go", 0, &pad_gb, &header_gb); // TODO : change graphics
    add_emulator("Tamagotchi", "tama", "b", RG_LOGO_PAD_TAMA, RG_LOGO_HEADER_TAMA, NO_GAME_DATA);
    add_emulator("Pokemon Mini", "mini", "min", RG_LOGO_PAD_PKMINI, RG_LOGO_HEADER_PKMINI, NO_GAME_DATA);
    add_emulator("Homebrew", "homebrew", "bin", RG_LOGO_EMPTY, RG_LOGO_HEADER_HOMEBREW, NO_GAME_DATA);
}

bool emulator_is_file_valid(retro_emulator_file_t *file)
{
    for (int i = 0; i < emulators_count; i++) {
        for (int j = 0; j < emulators[i].roms.count; j++) {
            if (&emulators[i].roms.files[j] == file) {
                return true;
            }
        }
    }

    return false;
}

retro_emulator_file_t *emulator_get_file(char *file_path)
{
    for (int i = 0; i < emulators_count; i++) {
        emulators[i].roms.count = 0;
        emulator_refresh_list(&emulators[i]);
        for (int j = 0; j < emulators[i].roms.count; j++) {
            if (strcmp(emulators[i].roms.files[j].path, file_path) == 0) {
                return &emulators[i].roms.files[j];
            }
        }
    }
    return NULL;
}
