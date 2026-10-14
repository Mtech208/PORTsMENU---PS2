#ifndef UI_MENU_H
#define UI_MENU_H

#include <tamtypes.h>
#include "gs_renderer.h"

#define MAX_GAMES 128
#define GRID_COLS 4
#define GRID_ROWS 2
#define MAX_VISIBLE (GRID_COLS * GRID_ROWS)

typedef struct {
    char title[128];
    char elf_path[256];
    char args[256];
    char cover[128];
    char background[128];
    char logo[128];
    char icon[128];
    char description[512];
    char genre[64];
    char year[16];
    char publisher[64];
    char players[16];
    int  rating;
    float anim_x, anim_y;
    float anim_scale;
    float anim_alpha;
    float target_x, target_y;
    float target_scale;
    float target_alpha;
} GameEntry;

typedef struct {
    GameEntry games[MAX_GAMES];
    int count;
    int selected;
    int page;
    int games_per_page;
    float page_anim;
    float target_page_anim;
} GameList;

typedef struct {
    float time;
    float dt;
    int frame;
    int show_help;
    int show_details;
    float bg_anim;
    float particle_time;
    char error_msg[128];
    float error_time;
    int show_settings;
    int settings_sel;
    int language;
    int sort_mode;
} UIState;

void ui_init(void);
void ui_shutdown(void);
void ui_load_games(void);
void ui_save_config(void);
void ui_load_config(void);

/* precarga progresiva: 1 cover por llamada (para hacerlo durante el boot splash).
   devuelve 1 cuando todos estan cargados. */
int ui_precache_step(void);

/* diagnostico del escaneo para mostrar en pantalla (PS2 real sin host/log) */
int ui_get_diag_count(void);
const char *ui_get_diag_line(int i);

void ui_update(float dt);
void ui_render(void);

int ui_handle_input(unsigned short btn, unsigned short btn_pressed);

GameEntry *ui_get_selected(void);
int ui_get_selected_idx(void);
int ui_get_game_count(void);

void ui_set_boot_path(const char *path);
void ui_launch_selected(void);

#endif