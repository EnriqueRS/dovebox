// DoveBox Dashboard (Fase 3, Pieza B) — panel LVGL de 5 vistas con swipe
// Reproduce el diseño validado en /preview (negro puro, sin emoticonos,
// puntos de paginación, round-robin de noticias, reloj con anillo armónico).
//
// Integración: el GLOB de main/CMakeLists.txt compila automáticamente
// cualquier *.cc del directorio del board. Se instancia desde
// CustomLcdDisplay::SetupUI() del board.
#include "config.h"
#include "board.h"
#include "application.h"
#include "display/lcd_display.h"
#include "display/lvgl_display/lvgl_theme.h"

#include <lvgl.h>
#include "src/misc/lv_timer.h"
#include <cJSON.h>
#include <esp_log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>
#include <cmath>

// Logo embebido del proveedor (dovebox_logos.cc, generado por gen_logos.py)
const lv_image_dsc_t* dovebox_logo(const char* feed);

#define TAG "DoveboxDashboard"

// ---------------------------------------------------------------------------
// Design tokens (aprobados por Quique, 2026-08-06)
// ---------------------------------------------------------------------------
#define CLR_BG        lv_color_hex(0x000000)
#define CLR_CIAN      lv_color_hex(0x4dd7ff)
#define CLR_AZUL      lv_color_hex(0x2f6bff)
#define CLR_TEXT      lv_color_hex(0xe8ecf1)
#define CLR_DIM       lv_color_hex(0x7c8798)
#define CLR_FAINT     lv_color_hex(0x4a5260)
#define CLR_MINT      lv_color_hex(0x4ade80)
#define CLR_SKY       lv_color_hex(0x38bdf8)
#define CLR_PANEL_A   lv_color_hex(0x0e1116)
#define CLR_PANEL_B   lv_color_hex(0x131722)
#define CLR_BORDER    lv_color_hex(0x181d27)

#define AGGREGATOR_HOST "192.168.100.73"
#define AGGREGATOR_PORT 18100
#define POLL_INTERVAL_MS 60000
#define CLOCK_TICK_MS 100
#define SWIPE_THRESHOLD 50
#define NUM_VIEWS 5

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

static lv_color_t news_color(const char* feed) {
    if (!feed) return CLR_DIM;
    if (strcmp(feed, "marca") == 0) return lv_color_hex(0xff5252);
    if (strcmp(feed, "besoccer") == 0) return lv_color_hex(0xff9f43);
    if (strcmp(feed, "mundodeportivo") == 0) return lv_color_hex(0x5b8cff);
    if (strcmp(feed, "elespanol") == 0) return lv_color_hex(0xffd166);
    if (strcmp(feed, "xataka") == 0) return lv_color_hex(0x9b5cff);
    if (strcmp(feed, "google") == 0) return lv_color_hex(0x66a3ff);
    return CLR_DIM;
}

static const char* news_label(const char* feed) {
    if (!feed) return "?";
    if (strcmp(feed, "marca") == 0) return "MARCA";
    if (strcmp(feed, "besoccer") == 0) return "BeSoccer";
    if (strcmp(feed, "mundodeportivo") == 0) return "M. Deportivo";
    if (strcmp(feed, "elespanol") == 0) return "El Español";
    if (strcmp(feed, "xataka") == 0) return "Xataka";
    if (strcmp(feed, "google") == 0) return "Google Noticias";
    return feed;
}

// Color de serie por sensor (índice de habitación). MISMA paleta en la
// gráfica y en el fondo ténue de las tarjetas del home, para poder
// identificar a qué línea corresponde cada sensor.
static lv_color_t room_series_color(size_t idx) {
    static const lv_color_t colors[] = {
        CLR_CIAN,                 // 0
        lv_color_hex(0x4ade80),   // verde
        lv_color_hex(0xffd166),   // amarillo
        lv_color_hex(0xff7a45),   // naranja
        lv_color_hex(0x9b5cff),   // morado
        lv_color_hex(0xff5252),   // rojo
        lv_color_hex(0x38bdf8),   // azul claro
    };
    return colors[idx % (sizeof(colors) / sizeof(colors[0]))];
}

struct TodoItem {
    std::string text;
    bool done = false;
};

struct RoomInfo {
    std::string name;
    std::string temp;
    std::string humidity;
    std::vector<float> history;  // 24h de temperatura (12 muestras, 2h)
};

struct NewsItem {
    std::string feed;
    std::string provider;  // nombre visible del medio (para mostrar junto al logo)
    std::string title;
};

class DoveboxDashboard {
public:
    DoveboxDashboard(LcdDisplay* display, lv_obj_t* screen)
        : display_(display), screen_(screen) {
        s_instance_ = this;
        BuildUI();
        clock_timer_ = lv_timer_create(ClockTickCb, CLOCK_TICK_MS, this);
        // Auto-cycle: rota por las vistas (1-4) cada 12s, nunca vuelve a la
        // face (0) automáticamente — la cara es la pantalla de reposo.
        auto_cycle_timer_ = lv_timer_create(AutoCycleCb, 12000, this);
        // El primer fetch NO se hace aquí: SetupUI() corre antes de que la red
        // esté inicializada (crash lwip "Invalid mbox"). El poll_timer_ empieza
        // con periodo corto (3s) y FetchDashboard sube el periodo a 60s cuando
        // la red ya está conectada y ha recibido datos.
        poll_timer_ = lv_timer_create(PollTickCb, 3000, this);
    }

    // Callbacks LVGL / FreeRTOS (declarados aquí para que el constructor y
    // el timer los vean; las definiciones están más abajo)
    static void ClockTickCb(lv_timer_t* t);
    static void PollTickCb(lv_timer_t* t);
    static void AutoCycleCb(lv_timer_t* t);

private:
    LcdDisplay* display_;
    lv_obj_t* screen_;
    static DoveboxDashboard* s_instance_;
    lv_timer_t* clock_timer_ = nullptr;
    lv_timer_t* poll_timer_ = nullptr;
    lv_timer_t* auto_cycle_timer_ = nullptr;

    lv_obj_t* views_[NUM_VIEWS] = {nullptr};
    int current_view_ = 0;

    // Face
    lv_obj_t* eye_left_ = nullptr;
    lv_obj_t* eye_right_ = nullptr;
    lv_obj_t* pupil_left_ = nullptr;
    lv_obj_t* pupil_right_ = nullptr;
    lv_obj_t* face_time_ = nullptr;
    int last_blink_ms_ = 0;
    int blink_start_ms_ = -1;  // -1 = sin parpadeo activo (máquina de estados)

    // Clock
    lv_obj_t* clock_time_ = nullptr;
    lv_obj_t* clock_date_ = nullptr;
    lv_obj_t* clock_dow_ = nullptr;
    lv_obj_t* clock_arc_ = nullptr;

    // Home / Todo / News
    lv_obj_t* home_rows_ = nullptr;
    lv_obj_t* todo_list_ = nullptr;
    lv_obj_t* news_list_ = nullptr;
    int news_offset_ = 0;

    // Paginación (contenedor flex + dots)
    lv_obj_t* dots_[NUM_VIEWS] = {nullptr};

    // Gestos
    lv_point_t press_start_ = {0, 0};
    bool pressed_ = false;

    // Datos
    static std::vector<TodoItem> g_todos;
    static std::vector<RoomInfo> g_rooms;
    static std::vector<NewsItem> g_news;
    static bool g_data_dirty;

    static lv_obj_t* MakePanel(lv_obj_t* parent) {
        lv_obj_t* p = lv_obj_create(parent);
        lv_obj_set_size(p, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_radius(p, 18, 0);
        lv_obj_set_style_bg_color(p, CLR_PANEL_A, 0);
        lv_obj_set_style_bg_grad_color(p, CLR_PANEL_B, 0);
        lv_obj_set_style_bg_grad_dir(p, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_border_color(p, CLR_BORDER, 0);
        lv_obj_set_style_border_width(p, 1, 0);
        lv_obj_set_style_pad_all(p, 14, 0);
        lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
        return p;
    }

    static lv_obj_t* MakeLabel(lv_obj_t* parent, const char* text, lv_color_t color, const lv_font_t* font) {
        lv_obj_t* l = lv_label_create(parent);
        lv_label_set_text(l, text ? text : "");
        lv_obj_set_style_text_color(l, color, 0);
        if (font) lv_obj_set_style_text_font(l, font, 0);
        return l;
    }
    static lv_obj_t* MakeView() {
        lv_obj_t* v = lv_obj_create(lv_screen_active());
        lv_obj_set_size(v, 480, 480);
        lv_obj_set_pos(v, 0, 0);
        lv_obj_set_style_bg_color(v, CLR_BG, 0);
        lv_obj_set_style_border_width(v, 0, 0);
        lv_obj_set_style_radius(v, 0, 0);
        lv_obj_set_style_pad_all(v, 0, 0);
        lv_obj_clear_flag(v, LV_OBJ_FLAG_SCROLLABLE);
        return v;
    }

    // ---------------- BuildUI ----------------
    // Un lv_font_t válido SIEMPRE tiene los callbacks de glyph. Los fonts
    // binarios del asset (LvglCBinFont) pueden quedar con punteros basura →
    // InstrFetchProhibited en lv_font_get_glyph_width al hacer layout.
    static bool FontUsable(const lv_font_t* font) {
        return font != nullptr && font->get_glyph_dsc != nullptr && font->get_glyph_bitmap != nullptr;
    }

    // Fuente del tema con fallback: en SetupUI() el screen puede no tener aún
    // la fuente configurada, y pasar NULL a lv_obj_set_style_text_font deja el
    // label con fuente inválida → crash en lv_font_get_glyph_width al render.
    const lv_font_t* GetThemeFont() {
        Theme* t = display_ ? display_->GetTheme() : nullptr;
        if (t) {
            auto* lt = static_cast<LvglTheme*>(t);
            auto tf = lt->text_font();
            const lv_font_t* f = (tf && tf->font()) ? tf->font() : nullptr;
            if (FontUsable(f)) return f;
        }
        const lv_font_t* f = lv_obj_get_style_text_font(screen_, LV_PART_MAIN);
        if (FontUsable(f)) return f;
        return LV_FONT_DEFAULT;
    }

    void BuildUI() {
        const lv_font_t* theme_font = GetThemeFont();
        views_[0] = BuildFace(theme_font);
        views_[1] = BuildClock(theme_font);
        views_[2] = BuildHome(theme_font);
        views_[3] = BuildTodo(theme_font);
        views_[4] = BuildNews(theme_font);
        BuildDots();
        ShowView(0);
    }

    lv_obj_t* BuildFace(const lv_font_t* font) {
        lv_obj_t* v = MakeView();
        // Ojos estilo preview (validado por Quique 2026-08-07):
        // elípticos, fondo oscuro con borde cian, pupila cian brillante con destello.
        eye_left_ = lv_obj_create(v);
        lv_obj_set_size(eye_left_, 104, 126);
        lv_obj_set_style_radius(eye_left_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(eye_left_, lv_color_hex(0x0a0f14), 0);
        lv_obj_set_style_border_width(eye_left_, 2, 0);
        lv_obj_set_style_border_color(eye_left_, lv_color_hex(0x2e5f78), 0);
        lv_obj_set_style_shadow_color(eye_left_, CLR_CIAN, 0);
        lv_obj_set_style_shadow_opa(eye_left_, LV_OPA_30, 0);
        lv_obj_set_style_shadow_width(eye_left_, 22, 0);
        lv_obj_set_style_shadow_spread(eye_left_, 2, 0);
        lv_obj_set_style_pad_all(eye_left_, 0, 0);
        lv_obj_align(eye_left_, LV_ALIGN_CENTER, -64, -46);

        eye_right_ = lv_obj_create(v);
        lv_obj_set_size(eye_right_, 104, 126);
        lv_obj_set_style_radius(eye_right_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(eye_right_, lv_color_hex(0x0a0f14), 0);
        lv_obj_set_style_border_width(eye_right_, 2, 0);
        lv_obj_set_style_border_color(eye_right_, lv_color_hex(0x2e5f78), 0);
        lv_obj_set_style_shadow_color(eye_right_, CLR_CIAN, 0);
        lv_obj_set_style_shadow_opa(eye_right_, LV_OPA_30, 0);
        lv_obj_set_style_shadow_width(eye_right_, 22, 0);
        lv_obj_set_style_shadow_spread(eye_right_, 2, 0);
        lv_obj_set_style_pad_all(eye_right_, 0, 0);
        lv_obj_align(eye_right_, LV_ALIGN_CENTER, 64, -46);

        // Pupila: círculo cian brillante con gradiente (oscuro en la base)
        pupil_left_ = lv_obj_create(eye_left_);
        lv_obj_set_size(pupil_left_, 34, 40);
        lv_obj_set_style_radius(pupil_left_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(pupil_left_, lv_color_hex(0x4dd7ff), 0);
        lv_obj_set_style_bg_grad_color(pupil_left_, lv_color_hex(0x0e5a75), 0);
        lv_obj_set_style_bg_grad_dir(pupil_left_, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_border_width(pupil_left_, 0, 0);
        lv_obj_set_style_shadow_color(pupil_left_, CLR_CIAN, 0);
        lv_obj_set_style_shadow_opa(pupil_left_, LV_OPA_60, 0);
        lv_obj_set_style_shadow_width(pupil_left_, 14, 0);
        lv_obj_align(pupil_left_, LV_ALIGN_CENTER, 0, 0);

        pupil_right_ = lv_obj_create(eye_right_);
        lv_obj_set_size(pupil_right_, 34, 40);
        lv_obj_set_style_radius(pupil_right_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(pupil_right_, lv_color_hex(0x4dd7ff), 0);
        lv_obj_set_style_bg_grad_color(pupil_right_, lv_color_hex(0x0e5a75), 0);
        lv_obj_set_style_bg_grad_dir(pupil_right_, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_border_width(pupil_right_, 0, 0);
        lv_obj_set_style_shadow_color(pupil_right_, CLR_CIAN, 0);
        lv_obj_set_style_shadow_opa(pupil_right_, LV_OPA_60, 0);
        lv_obj_set_style_shadow_width(pupil_right_, 14, 0);
        lv_obj_align(pupil_right_, LV_ALIGN_CENTER, 0, 0);

        // Destello blanco en la pupila (como el ::after del preview)
        lv_obj_t* glint_l = lv_obj_create(pupil_left_);
        lv_obj_set_size(glint_l, 9, 8);
        lv_obj_set_style_radius(glint_l, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(glint_l, lv_color_hex(0xdff4ff), 0);
        lv_obj_set_style_border_width(glint_l, 0, 0);
        lv_obj_set_style_bg_opa(glint_l, LV_OPA_90, 0);
        lv_obj_align(glint_l, LV_ALIGN_TOP_LEFT, 6, 5);

        lv_obj_t* glint_r = lv_obj_create(pupil_right_);
        lv_obj_set_size(glint_r, 9, 8);
        lv_obj_set_style_radius(glint_r, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(glint_r, lv_color_hex(0xdff4ff), 0);
        lv_obj_set_style_border_width(glint_r, 0, 0);
        lv_obj_set_style_bg_opa(glint_r, LV_OPA_90, 0);
        lv_obj_align(glint_r, LV_ALIGN_TOP_LEFT, 6, 5);

        lv_obj_t* label = MakeLabel(v, "DoveBox", CLR_DIM, font);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 68);
        lv_obj_set_style_text_letter_space(label, 8, 0);
        lv_obj_set_style_text_opa(label, LV_OPA_60, 0);
        // Hora en la cara (sustituye a "desliza para explorar")
        face_time_ = lv_label_create(v);
        lv_label_set_text(face_time_, "00:00");
        lv_obj_set_style_text_color(face_time_, CLR_FAINT, 0);
        lv_obj_set_style_text_font(face_time_, font, 0);
        lv_obj_align(face_time_, LV_ALIGN_CENTER, 0, 102);
        return v;
    }

    lv_obj_t* BuildClock(const lv_font_t* font) {
        lv_obj_t* v = MakeView();
        clock_arc_ = lv_arc_create(v);
        lv_obj_set_size(clock_arc_, 336, 336);
        lv_obj_align(clock_arc_, LV_ALIGN_CENTER, 0, 0);
        lv_arc_set_rotation(clock_arc_, 270);
        lv_arc_set_bg_angles(clock_arc_, 0, 360);
        lv_arc_set_value(clock_arc_, 0);
        lv_arc_set_range(clock_arc_, 0, 100);
        lv_obj_remove_style(clock_arc_, NULL, LV_PART_KNOB);
        lv_obj_remove_flag(clock_arc_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(clock_arc_, 10, LV_PART_MAIN);
        lv_obj_set_style_arc_color(clock_arc_, lv_color_hex(0x1a2029), LV_PART_MAIN);
        lv_obj_set_style_arc_width(clock_arc_, 10, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(clock_arc_, CLR_CIAN, LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(clock_arc_, LV_OPA_COVER, LV_PART_INDICATOR);

        clock_time_ = lv_label_create(v);
        lv_label_set_text(clock_time_, "00:00");
        lv_obj_set_style_text_color(clock_time_, CLR_TEXT, 0);
        lv_obj_set_style_text_font(clock_time_, font, 0);
        lv_obj_set_style_transform_scale_x(clock_time_, 240, 0);
        lv_obj_set_style_transform_scale_y(clock_time_, 240, 0);
        lv_obj_set_style_transform_pivot_x(clock_time_, lv_pct(50), 0);
        lv_obj_set_style_transform_pivot_y(clock_time_, lv_pct(50), 0);
        lv_obj_align(clock_time_, LV_ALIGN_CENTER, 0, -8);

        clock_date_ = MakeLabel(v, "7 agosto 2026", CLR_DIM, font);
        lv_obj_align(clock_date_, LV_ALIGN_CENTER, 0, 62);
        clock_dow_ = MakeLabel(v, "viernes", CLR_FAINT, font);
        lv_obj_align(clock_dow_, LV_ALIGN_CENTER, 0, 96);
        return v;
    }

    lv_obj_t* BuildHome(const lv_font_t* font) {
        lv_obj_t* v = MakeView();
        lv_obj_t* title = MakeLabel(v, "Casa", CLR_TEXT, font);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 24);
        home_rows_ = lv_obj_create(v);
        lv_obj_set_size(home_rows_, 432, 388);
        lv_obj_align(home_rows_, LV_ALIGN_TOP_MID, 0, 72);
        lv_obj_set_style_bg_opa(home_rows_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(home_rows_, 0, 0);
        lv_obj_set_flex_flow(home_rows_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(home_rows_, 14, 0);
        lv_obj_set_scrollbar_mode(home_rows_, LV_SCROLLBAR_MODE_OFF);
        return v;
    }

    lv_obj_t* BuildTodo(const lv_font_t* font) {
        lv_obj_t* v = MakeView();
        lv_obj_t* title = MakeLabel(v, "To Do", CLR_TEXT, font);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 24);
        todo_list_ = lv_obj_create(v);
        lv_obj_set_size(todo_list_, 432, 380);
        lv_obj_align(todo_list_, LV_ALIGN_TOP_MID, 0, 70);
        lv_obj_set_style_bg_opa(todo_list_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(todo_list_, 0, 0);
        lv_obj_set_flex_flow(todo_list_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(todo_list_, 8, 0);
        lv_obj_set_scrollbar_mode(todo_list_, LV_SCROLLBAR_MODE_OFF);
        return v;
    }

    lv_obj_t* BuildNews(const lv_font_t* font) {
        lv_obj_t* v = MakeView();
        lv_obj_t* title = MakeLabel(v, "Noticias", CLR_TEXT, font);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 24);
        news_list_ = lv_obj_create(v);
        lv_obj_set_size(news_list_, 432, 380);
        lv_obj_align(news_list_, LV_ALIGN_TOP_MID, 0, 70);
        lv_obj_set_style_bg_opa(news_list_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(news_list_, 0, 0);
        lv_obj_set_flex_flow(news_list_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(news_list_, 12, 0);
        lv_obj_set_scrollbar_mode(news_list_, LV_SCROLLBAR_MODE_OFF);
        return v;
    }

    void BuildDots() {
        lv_obj_t* bar = lv_obj_create(screen_);
        lv_obj_set_size(bar, 200, 20);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -12);
        lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(bar, 6, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        for (int i = 0; i < NUM_VIEWS; i++) {
            lv_obj_t* dot = lv_obj_create(bar);
            lv_obj_set_size(dot, 6, 6);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(dot, CLR_FAINT, 0);
            lv_obj_set_style_border_width(dot, 0, 0);
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
            dots_[i] = dot;
        }
    }

    void ShowView(int idx) {
        if (idx < 0 || idx >= NUM_VIEWS) return;
        current_view_ = idx;
        for (int i = 0; i < NUM_VIEWS; i++) {
            if (views_[i]) {
                if (i == idx) lv_obj_remove_flag(views_[i], LV_OBJ_FLAG_HIDDEN);
                else lv_obj_add_flag(views_[i], LV_OBJ_FLAG_HIDDEN);
            }
            if (dots_[i]) {
                if (i == idx) {
                    lv_obj_set_size(dots_[i], 22, 6);
                    lv_obj_set_style_bg_color(dots_[i], CLR_CIAN, 0);
                    lv_obj_set_style_bg_grad_color(dots_[i], CLR_AZUL, 0);
                    lv_obj_set_style_bg_grad_dir(dots_[i], LV_GRAD_DIR_HOR, 0);
                    lv_obj_set_style_shadow_color(dots_[i], CLR_CIAN, 0);
                    lv_obj_set_style_shadow_opa(dots_[i], LV_OPA_50, 0);
                    lv_obj_set_style_shadow_width(dots_[i], 8, 0);
                } else {
                    lv_obj_set_size(dots_[i], 6, 6);
                    lv_obj_set_style_bg_color(dots_[i], CLR_FAINT, 0);
                    lv_obj_set_style_shadow_opa(dots_[i], LV_OPA_0, 0);
                    lv_obj_set_style_bg_grad_dir(dots_[i], LV_GRAD_DIR_NONE, 0);
                }
            }
        }
        if (idx == 2) RenderHome();
        if (idx == 3) RenderTodo();
        if (idx == 4) RenderNews();
    }

    // Cuando el chat de voz está activo (escuchando/hablando), ocultamos el
    // dashboard para que se vea la UI de chat por defecto; al volver a idle
    // restauramos la vista actual.
    void UpdateStateVisibility() {
        auto& app = Application::GetInstance();
        DeviceState state = app.GetDeviceState();
        bool chat_active = (state == kDeviceStateListening || state == kDeviceStateSpeaking ||
                            state == kDeviceStateConnecting || state == kDeviceStateUpgrading ||
                            state == kDeviceStateWifiConfiguring || state == kDeviceStateAudioTesting ||
                            state == kDeviceStateActivating);
        for (int i = 0; i < NUM_VIEWS; i++) {
            if (!views_[i]) continue;
            if (chat_active) {
                lv_obj_add_flag(views_[i], LV_OBJ_FLAG_HIDDEN);
            } else if (i == current_view_) {
                lv_obj_remove_flag(views_[i], LV_OBJ_FLAG_HIDDEN);
            }
            if (dots_[i]) {
                if (chat_active) lv_obj_add_flag(dots_[i], LV_OBJ_FLAG_HIDDEN);
                else lv_obj_remove_flag(dots_[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    void UpdateClock() {
        if (!clock_time_) return;
        time_t now = time(nullptr);
        struct tm tmv;
        localtime_r(&now, &tmv);

        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
        lv_label_set_text(clock_time_, buf);
        if (face_time_) lv_label_set_text(face_time_, buf);

        static const char* meses[] = {"enero","febrero","marzo","abril","mayo","junio",
                                      "julio","agosto","septiembre","octubre","noviembre","diciembre"};
        static const char* dias[] = {"domingo","lunes","martes","miércoles","jueves","viernes","sábado"};
        snprintf(buf, sizeof(buf), "%d %s %d", tmv.tm_mday, meses[tmv.tm_mon], tmv.tm_year + 1900);
        lv_label_set_text(clock_date_, buf);
        lv_label_set_text(clock_dow_, dias[tmv.tm_wday]);

        // Anillo armónico de 2 min: impar = llenar, par = vaciar (SIEMPRE horario)
        double frac = (tmv.tm_sec + 0.0) / 60.0;
        if (tmv.tm_min % 2 == 1) {
            lv_arc_set_bg_angles(clock_arc_, 0, 360);
            lv_arc_set_value(clock_arc_, (int)(frac * 100.0));
        } else {
            lv_arc_set_bg_angles(clock_arc_, (int)(frac * 360.0), 360);
            lv_arc_set_value(clock_arc_, 100);
        }
    }

    // ---------------- Cara (ojos) ----------------
    // Parpadeo DETERMINISTA (fix 2026-08-07): las animaciones lv_anim con
    // ready_cb se quedaban achatadas si la vista se ocultaba a mitad del
    // parpadeo (el callback nunca llegaba y blink_busy_ quedaba a true).
    // Ahora es una máquina de estados por tiempo: SIEMPRE restaura al 100%
    // tras 90ms, pase lo que pase. Mismo comportamiento que el preview.
    void UpdateEyes() {
        if (!eye_left_) return;
        int now_ms = lv_tick_get();

        if (blink_start_ms_ < 0) {
            if (now_ms - last_blink_ms_ < 2500 + (rand() % 3500)) return;
            last_blink_ms_ = now_ms;
            blink_start_ms_ = now_ms;
        }

        int dt = now_ms - blink_start_ms_;
        if (dt >= 90) {
            lv_obj_set_style_transform_scale_y(eye_left_, 100, 0);
            lv_obj_set_style_transform_scale_y(eye_right_, 100, 0);
            blink_start_ms_ = -1;
            return;
        }

        // Cerrar 0-45ms (100→10), abrir 45-90ms (10→100)
        int32_t v;
        if (dt < 45) {
            v = 100 - (int32_t)(90 * dt / 45);
        } else {
            v = 10 + (int32_t)(90 * (dt - 45) / 45);
        }
        lv_obj_set_style_transform_pivot_y(eye_left_, lv_pct(50), 0);
        lv_obj_set_style_transform_pivot_y(eye_right_, lv_pct(50), 0);
        lv_obj_set_style_transform_scale_y(eye_left_, v, 0);
        lv_obj_set_style_transform_scale_y(eye_right_, v, 0);
    }

    // Polling de gestos desde el timer del reloj (más fiable que eventos,
    // que solo burbujean si los objetos tienen LV_OBJ_FLAG_EVENT_BUBBLE).
    void UpdateGestures() {
        lv_indev_t* indev = lv_indev_active();
        if (!indev) return;
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        bool pressed = (lv_indev_get_state(indev) & LV_INDEV_STATE_PRESSED) != 0;

        if (pressed && !pressed_) {
            pressed_ = true;
            press_start_ = p;
        } else if (pressed && pressed_) {
            // swipe en curso: seguimos las pupilas si estamos en la vista face
            if (current_view_ == 0 && eye_left_) {
                for (int which = 0; which < 2; which++) {
                    lv_obj_t* eye = which == 0 ? eye_left_ : eye_right_;
                    lv_obj_t* pupil = which == 0 ? pupil_left_ : pupil_right_;
                    int cx = lv_obj_get_x(eye) + lv_obj_get_width(eye) / 2;
                    int cy = lv_obj_get_y(eye) + lv_obj_get_height(eye) / 2;
                    int dx = (p.x - cx) * 12 / 70; if (dx > 12) dx = 12; if (dx < -12) dx = -12;
                    int dy = (p.y - cy) * 12 / 70; if (dy > 12) dy = 12; if (dy < -12) dy = -12;
                    lv_obj_set_style_translate_x(pupil, dx, 0);
                    lv_obj_set_style_translate_y(pupil, dy, 0);
                }
            }
        } else if (!pressed && pressed_) {
            pressed_ = false;
            // suelta: si estamos en face, volvemos las pupilas al centro
            if (current_view_ == 0 && pupil_left_) {
                lv_obj_set_style_translate_x(pupil_left_, 0, 0);
                lv_obj_set_style_translate_y(pupil_left_, 0, 0);
                lv_obj_set_style_translate_x(pupil_right_, 0, 0);
                lv_obj_set_style_translate_y(pupil_right_, 0, 0);
            }
            int dx = p.x - press_start_.x;
            if (dx < -SWIPE_THRESHOLD && current_view_ < NUM_VIEWS - 1) {
                ShowView(current_view_ + 1);
            } else if (dx > SWIPE_THRESHOLD && current_view_ > 0) {
                ShowView(current_view_ - 1);
            }
        }
    }

    // ---------------- Gestos de swipe (eventos, ya no usados) ----------------
    static void GestureCb(lv_event_t* e) {
        (void)e;
    }

    // ---------------- Fetch HTTP (en task separado para no bloquear LVGL) ----------------
    static void FetchTask(void* arg) {
        auto* self = static_cast<DoveboxDashboard*>(arg);
        self->FetchDashboard();
        vTaskDelete(NULL);
    }

    void FetchDashboardAsync() {
        xTaskCreate(FetchTask, "dovebox_fetch", 8192, this, 5, nullptr);
    }

    void FetchDashboard() {
        // Red no lista todavía → salir y reintentar en el próximo tick (3s).
        // Evita el assert de lwip "Invalid mbox" cuando SetupUI() corre antes
        // de que WiFi/lwIP esté inicializado. El estado Idle implica red
        // conectada y app lista.
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() != kDeviceStateIdle) {
            return;
        }
        auto network = Board::GetInstance().GetNetwork();
        if (!network) {
            return;
        }
        std::string body;
        auto http = network->CreateHttp(3);
        if (!http) return;
        http->SetHeader("Accept", "application/json");
        if (!http->Open("GET", "http://" AGGREGATOR_HOST ":" STR(AGGREGATOR_PORT) "/api/dashboard")) {
            ESP_LOGW(TAG, "GET dashboard failed");
            return;
        }
        if (http->GetStatusCode() != 200) {
            ESP_LOGW(TAG, "GET dashboard -> %d", http->GetStatusCode());
            http->Close();
            return;
        }
        body = http->ReadAll();
        http->Close();
        if (body.empty()) return;

        // Primer fetch con éxito → subimos el periodo del timer a 60s
        // (empezaba en 3s para reintentar hasta que la red estuviera lista).
        if (poll_timer_) {
            lv_timer_set_period(poll_timer_, POLL_INTERVAL_MS);
        }

        cJSON* root = cJSON_Parse(body.c_str());
        if (!root) { ESP_LOGW(TAG, "JSON parse failed"); return; }

        {
            // parseamos y copiamos a datos globales
            std::vector<TodoItem> todos;
            std::vector<RoomInfo> rooms;
            std::vector<NewsItem> news;

            cJSON* todos_j = cJSON_GetObjectItem(root, "todos");
            if (cJSON_IsArray(todos_j)) {
                cJSON* it;
                cJSON_ArrayForEach(it, todos_j) {
                    cJSON* text = cJSON_GetObjectItem(it, "text");
                    cJSON* done = cJSON_GetObjectItem(it, "done");
                    if (cJSON_IsString(text)) {
                        TodoItem item;
                        item.text = text->valuestring;
                        item.done = cJSON_IsTrue(done);
                        todos.push_back(item);
                    }
                }
            }

            cJSON* home = cJSON_GetObjectItem(root, "home");
            cJSON* rooms_j = home ? cJSON_GetObjectItem(home, "rooms") : nullptr;
            if (cJSON_IsArray(rooms_j)) {
                cJSON* it;
                cJSON_ArrayForEach(it, rooms_j) {
                    RoomInfo r;
                    cJSON* name = cJSON_GetObjectItem(it, "name");
                    cJSON* temp = cJSON_GetObjectItem(it, "temperature");
                    cJSON* hum = cJSON_GetObjectItem(it, "humidity");
                    if (cJSON_IsString(name)) r.name = name->valuestring;
                    if (temp) {
                        cJSON* v = cJSON_GetObjectItem(temp, "value");
                        if (cJSON_IsString(v)) r.temp = v->valuestring;
                    }
                    if (hum) {
                        cJSON* v = cJSON_GetObjectItem(hum, "value");
                        if (cJSON_IsString(v)) r.humidity = v->valuestring;
                    }
                    // Histórico 24h de temperatura (para la gráfica del home)
                    cJSON* hist = cJSON_GetObjectItem(it, "history");
                    if (cJSON_IsArray(hist)) {
                        cJSON* hv;
                        cJSON_ArrayForEach(hv, hist) {
                            if (cJSON_IsNumber(hv)) r.history.push_back((float)hv->valuedouble);
                        }
                    }
                    rooms.push_back(r);
                }
            }

            // News: round-robin estricto (1 de cada fuente en orden fijo)
            cJSON* news_j = cJSON_GetObjectItem(root, "news");
            cJSON* feeds = news_j ? cJSON_GetObjectItem(news_j, "feeds") : nullptr;
            if (cJSON_IsObject(feeds)) {
                static const char* order[] = {"marca", "besoccer", "mundodeportivo", "elespanol", "xataka", "google"};
                constexpr int kNumFeeds = 6;
                std::vector<std::vector<NewsItem>> by_feed(kNumFeeds);
                for (int f = 0; f < kNumFeeds; f++) {
                    cJSON* arr = cJSON_GetObjectItem(feeds, order[f]);
                    if (cJSON_IsArray(arr)) {
                        cJSON* it;
                        cJSON_ArrayForEach(it, arr) {
                            cJSON* title = cJSON_GetObjectItem(it, "title");
                            if (cJSON_IsString(title)) {
                                NewsItem n;
                                n.feed = order[f];
                                cJSON* prov = cJSON_GetObjectItem(it, "provider");
                                if (cJSON_IsString(prov)) n.provider = prov->valuestring;
                                n.title = title->valuestring;
                                by_feed[f].push_back(n);
                            }
                        }
                    }
                }
                size_t max_items = 0;
                for (auto& v : by_feed) if (v.size() > max_items) max_items = v.size();
                for (size_t i = 0; i < max_items; i++) {
                    for (int f = 0; f < kNumFeeds; f++) {
                        if (i < by_feed[f].size()) news.push_back(by_feed[f][i]);
                    }
                }
            }

            cJSON_Delete(root);

            // El swap de datos va bajo el lock del display para no chocar con
            // el render de LVGL (el parseo previo fue fuera del lock).
            {
                DisplayLockGuard lock(display_);
                g_todos = std::move(todos);
                g_rooms = std::move(rooms);
                g_news = std::move(news);
                g_data_dirty = true;
            }
        }
    }

    // ---------------- Render de vistas de datos ----------------
    void ClearChildren(lv_obj_t* parent) {
        lv_obj_t* child;
        while ((child = lv_obj_get_child(parent, 0)) != nullptr) {
            lv_obj_del(child);
        }
    }

    void RenderAllData() {
        if (current_view_ == 2) RenderHome();
        if (current_view_ == 3) RenderTodo();
        if (current_view_ == 4) RenderNews();
    }

    void RenderHome() {
        if (!home_rows_) return;
        ClearChildren(home_rows_);
        const lv_font_t* font = GetThemeFont();
        if (g_rooms.empty()) {
            MakeLabel(home_rows_, "sin datos todavía", CLR_FAINT, font);
            return;
        }
        size_t room_idx = 0;
        for (auto& r : g_rooms) {
            lv_obj_t* panel = MakePanel(home_rows_);
            lv_obj_set_size(panel, 432, 96);
            // Fondo ténue con el color de su serie en la gráfica: así se
            // identifica qué sensor corresponde a cada línea (alpha ~12%).
            lv_color_t tint = room_series_color(room_idx);
            lv_obj_set_style_bg_color(panel, tint, 0);
            lv_obj_set_style_bg_grad_color(panel, tint, 0);
            lv_obj_set_style_bg_opa(panel, 32, 0);

            lv_obj_t* dot = lv_obj_create(panel);
            lv_obj_set_size(dot, 14, 14);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(dot, 0, 0);
            float t = atof(r.temp.c_str());
            lv_color_t dotc = CLR_CIAN;
            if (t >= 28.0f) dotc = lv_color_hex(0xff7a45);
            else if (t <= 18.0f) dotc = CLR_SKY;
            lv_obj_set_style_bg_color(dot, dotc, 0);
            lv_obj_set_style_shadow_color(dot, dotc, 0);
            lv_obj_set_style_shadow_opa(dot, LV_OPA_50, 0);
            lv_obj_set_style_shadow_width(dot, 12, 0);
            lv_obj_align(dot, LV_ALIGN_LEFT_MID, 16, 0);

            lv_obj_t* name = lv_label_create(panel);
            lv_label_set_text(name, r.name.c_str());
            lv_obj_set_style_text_color(name, CLR_DIM, 0);
            lv_obj_set_style_text_font(name, font, 0);
            lv_obj_align(name, LV_ALIGN_TOP_LEFT, 42, 14);

            char buf[32];
            snprintf(buf, sizeof(buf), "%s°C", r.temp.c_str());
            lv_obj_t* temp = lv_label_create(panel);
            lv_label_set_text(temp, buf);
            lv_obj_set_style_text_color(temp, CLR_TEXT, 0);
            lv_obj_set_style_text_font(temp, font, 0);
            lv_obj_set_style_transform_scale_x(temp, 140, 0);
            lv_obj_set_style_transform_scale_y(temp, 140, 0);
            lv_obj_set_style_transform_pivot_x(temp, lv_pct(0), 0);
            lv_obj_set_style_transform_pivot_y(temp, lv_pct(100), 0);
            lv_obj_align(temp, LV_ALIGN_BOTTOM_LEFT, 42, -12);

            snprintf(buf, sizeof(buf), "%s%%", r.humidity.c_str());
            lv_obj_t* hum = lv_label_create(panel);
            lv_label_set_text(hum, buf);
            lv_obj_set_style_text_color(hum, CLR_SKY, 0);
            lv_obj_set_style_text_font(hum, font, 0);
            lv_obj_align(hum, LV_ALIGN_TOP_RIGHT, -16, 14);

            lv_obj_t* bar = lv_bar_create(panel);
            lv_obj_set_size(bar, 160, 6);
            lv_obj_align(bar, LV_ALIGN_BOTTOM_RIGHT, -16, -24);
            lv_bar_set_range(bar, 0, 100);
            lv_bar_set_value(bar, (int)atof(r.humidity.c_str()), LV_ANIM_OFF);
            lv_obj_set_style_bg_color(bar, lv_color_hex(0x1a2029), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
            lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(bar, CLR_CIAN, LV_PART_INDICATOR);
            lv_obj_set_style_bg_grad_color(bar, CLR_AZUL, LV_PART_INDICATOR);
            lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
            room_idx++;
        }

        // Gráfica de temperatura 24h (2 series, una por habitación con datos)
        size_t max_hist = 0;
        for (auto& r : g_rooms) if (r.history.size() > max_hist) max_hist = r.history.size();
        if (max_hist >= 2) {
            lv_obj_t* chart_panel = MakePanel(home_rows_);
            lv_obj_set_size(chart_panel, 432, 148);

            lv_obj_t* title = lv_label_create(chart_panel);
            lv_label_set_text(title, "Temperatura · 24h");
            lv_obj_set_style_text_color(title, CLR_DIM, 0);
            lv_obj_set_style_text_font(title, font, 0);
            lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 6);

            lv_obj_t* chart = lv_chart_create(chart_panel);
            lv_obj_set_size(chart, 408, 104);
            lv_obj_align(chart, LV_ALIGN_BOTTOM_MID, 0, -6);
            lv_chart_set_point_count(chart, max_hist);
            lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 10, 40);
            lv_chart_set_div_line_count(chart, 3, 0);
            lv_obj_set_style_line_color(chart, lv_color_hex(0x1a2029), LV_PART_MAIN);
            lv_obj_set_style_line_width(chart, 1, LV_PART_MAIN);
            lv_obj_set_style_pad_all(chart, 6, LV_PART_MAIN);
            lv_obj_set_style_pad_all(chart, 6, LV_PART_ITEMS);
            lv_obj_set_style_size(chart, 3, 3, LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, LV_PART_MAIN);

            int room_idx = 0;
            for (auto& r : g_rooms) {
                if (r.history.size() < 2) continue;
                std::vector<int32_t> values;
                values.reserve(r.history.size());
                for (float v : r.history) values.push_back((int32_t)(v * 10.0f));
                // Mismo color que el fondo de la tarjeta del sensor
                lv_color_t sc = room_series_color(room_idx);
                lv_chart_series_t* ser = lv_chart_add_series(chart, sc, LV_CHART_AXIS_PRIMARY_Y);
                lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
                lv_obj_set_style_size(chart, 2, 2, LV_PART_INDICATOR);
                lv_chart_set_series_values(chart, ser, values.data(), values.size());
                room_idx++;
            }
        }
    }

    void RenderTodo() {
        if (!todo_list_) return;
        ClearChildren(todo_list_);
        const lv_font_t* font = GetThemeFont();
        if (g_todos.empty()) {
            MakeLabel(todo_list_, "nada pendiente", CLR_FAINT, font);
            return;
        }
        size_t shown = g_todos.size() > 12 ? 12 : g_todos.size();
        for (size_t i = 0; i < shown; i++) {
            auto& item = g_todos[i];
            lv_obj_t* row = lv_obj_create(todo_list_);
            lv_obj_set_size(row, 432, 44);
            lv_obj_set_style_radius(row, 12, 0);
            lv_obj_set_style_bg_color(row, CLR_PANEL_A, 0);
            lv_obj_set_style_border_color(row, CLR_BORDER, 0);
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_user_data(row, (void*)(uintptr_t)i);

            lv_obj_t* box = lv_obj_create(row);
            lv_obj_set_size(box, 18, 18);
            lv_obj_set_style_radius(box, 4, 0);
            lv_obj_set_style_border_width(box, 2, 0);
            lv_obj_set_style_border_color(box, item.done ? CLR_MINT : CLR_FAINT, 0);
            lv_obj_set_style_bg_color(box, item.done ? CLR_MINT : CLR_BG, 0);
            lv_obj_align(box, LV_ALIGN_LEFT_MID, 12, 0);

            lv_obj_t* text = lv_label_create(row);
            lv_label_set_text(text, item.text.c_str());
            lv_obj_set_style_text_color(text, item.done ? CLR_FAINT : CLR_TEXT, 0);
            lv_obj_set_style_text_font(text, font, 0);
            if (item.done) lv_obj_set_style_text_decor(text, LV_TEXT_DECOR_STRIKETHROUGH, 0);
            lv_obj_align(text, LV_ALIGN_LEFT_MID, 40, 0);

            lv_obj_add_event_cb(row, TodoTapCb, LV_EVENT_CLICKED, this);
        }
    }

    static void PatchTask(void* arg);
    struct PatchArgs {
        DoveboxDashboard* self;
        size_t idx;
        bool done;
    };

    static void TodoTapCb(lv_event_t* e) {
        auto* self = static_cast<DoveboxDashboard*>(lv_event_get_user_data(e));
        lv_obj_t* row = static_cast<lv_obj_t*>(lv_event_get_target(e));
        size_t idx = (size_t)(uintptr_t)lv_obj_get_user_data(row);
        if (idx >= g_todos.size()) return;
        bool done = !g_todos[idx].done;

        // PATCH al agregador (en task separado)
        auto* args = new PatchArgs{self, idx, done};
        xTaskCreate(PatchTask, "dovebox_patch", 4096, args, 5, nullptr);
    }

    void RenderNews() {
        if (!news_list_) return;
        ClearChildren(news_list_);
        const lv_font_t* font = GetThemeFont();
        if (g_news.empty()) {
            MakeLabel(news_list_, "sin noticias todavía", CLR_FAINT, font);
            return;
        }
        for (int k = 0; k < 6; k++) {
            size_t idx = (news_offset_ + k) % g_news.size();
            auto& n = g_news[idx];
            lv_obj_t* row = lv_obj_create(news_list_);
            lv_obj_set_size(row, 432, LV_SIZE_CONTENT);
            lv_obj_set_style_radius(row, 14, 0);
            lv_obj_set_style_bg_color(row, CLR_PANEL_A, 0);
            lv_obj_set_style_border_color(row, CLR_BORDER, 0);
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_pad_all(row, 10, 0);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

            // Logo del proveedor (imagen embebida LVGL, ver dovebox_logos.cc)
            const lv_image_dsc_t* logo = dovebox_logo(n.feed.c_str());
            if (logo != nullptr) {
                lv_obj_t* img = lv_image_create(row);
                lv_image_set_src(img, logo);
                // 64px -> 40px
                lv_image_set_scale(img, 160);
                lv_obj_align(img, LV_ALIGN_TOP_LEFT, 8, 6);
            } else {
                // Fallback: punto de color si no hay logo
                lv_obj_t* dot = lv_obj_create(row);
                lv_obj_set_size(dot, 8, 8);
                lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
                lv_obj_set_style_border_width(dot, 0, 0);
                lv_obj_set_style_bg_color(dot, news_color(n.feed.c_str()), 0);
                lv_obj_align(dot, LV_ALIGN_TOP_LEFT, 12, 14);
            }

            // Nombre del proveedor junto al logo (dato del agregador o
            // fallback por feed)
            const char* provider = n.provider.empty() ? news_label(n.feed.c_str()) : n.provider.c_str();
            lv_obj_t* prov = lv_label_create(row);
            lv_label_set_text(prov, provider);
            lv_obj_set_style_text_color(prov, CLR_DIM, 0);
            lv_obj_set_style_text_font(prov, font, 0);
            lv_obj_align(prov, LV_ALIGN_TOP_LEFT, 56, 8);

            lv_obj_t* title = lv_label_create(row);
            lv_label_set_text(title, n.title.c_str());
            lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(title, 368);
            lv_obj_set_style_text_color(title, CLR_TEXT, 0);
            lv_obj_set_style_text_font(title, font, 0);
            lv_obj_align(title, LV_ALIGN_TOP_LEFT, 56, 30);
        }
    }
};

// Definiciones estáticas
std::vector<TodoItem> DoveboxDashboard::g_todos;
std::vector<RoomInfo> DoveboxDashboard::g_rooms;
std::vector<NewsItem> DoveboxDashboard::g_news;
bool DoveboxDashboard::g_data_dirty = false;
DoveboxDashboard* DoveboxDashboard::s_instance_ = nullptr;

// Definiciones de los callbacks (fuera de la clase: las declaraciones están
// dentro, y C++ no permite cuerpo inline + declaración a la vez)
void DoveboxDashboard::ClockTickCb(lv_timer_t* t) {
    auto* self = static_cast<DoveboxDashboard*>(lv_timer_get_user_data(t));
    if (!self) return;
    self->UpdateStateVisibility();
    self->UpdateGestures();
    self->UpdateClock();
    self->UpdateEyes();
    if (g_data_dirty) {
        g_data_dirty = false;
        self->RenderAllData();
    }
}

void DoveboxDashboard::PollTickCb(lv_timer_t* t) {
    auto* self = static_cast<DoveboxDashboard*>(lv_timer_get_user_data(t));
    if (!self) return;
    self->FetchDashboardAsync();
}

// Auto-cycle: cada 12s avanza a la siguiente vista (1→2→3→4→1...), nunca a la
// face (0). Si el usuario está en la cara, se queda ahí (pantalla de reposo).
void DoveboxDashboard::AutoCycleCb(lv_timer_t* t) {
    auto* self = static_cast<DoveboxDashboard*>(lv_timer_get_user_data(t));
    if (!self) return;
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() != kDeviceStateIdle) return;  // no rotar durante chat
    if (self->current_view_ == 0) return;                  // la face no rota sola
    int next = self->current_view_ + 1;
    if (next >= NUM_VIEWS) next = 1;                       // salta la face al dar la vuelta
    self->ShowView(next);
}

void DoveboxDashboard::PatchTask(void* arg) {
    auto* args = static_cast<PatchArgs*>(arg);
    auto* self = args->self;
    size_t idx = args->idx;
    bool done = args->done;
    delete args;

    char url[160];
    snprintf(url, sizeof(url), "http://" AGGREGATOR_HOST ":%d/api/todos/%zu", AGGREGATOR_PORT, idx);
    char body[32];
    snprintf(body, sizeof(body), "{\"done\":%s}", done ? "true" : "false");
    auto network = Board::GetInstance().GetNetwork();
    if (!network) { vTaskDelete(NULL); return; }
    auto http = network->CreateHttp(3);
    if (!http) { vTaskDelete(NULL); return; }
    http->SetHeader("Content-Type", "application/json");
    if (http->Open("PATCH", url)) {
        http->Write(body, strlen(body));
        http->Write("", 0);
        http->Close();
    }
    // actualización local (en el hilo LVGL vía flag)
    {
        DisplayLockGuard lock(self->display_);
        if (idx < g_todos.size()) g_todos[idx].done = done;
        g_data_dirty = true;
    }
    vTaskDelete(NULL);
}

// Factory para el board (evita exponer la clase completa en un header)
extern "C" void* dovebox_dashboard_create(void* display, void* screen) {
    return new DoveboxDashboard(static_cast<LcdDisplay*>(display), static_cast<lv_obj_t*>(screen));
}
