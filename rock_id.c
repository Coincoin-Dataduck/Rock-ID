#include <furi.h>
#include <gui/gui.h>
#include <gui/elements.h>
#include <input/input.h>
#include <stdio.h>
#include <string.h>

#define MAX_OPTIONS 6
#define VISIBLE_OPTIONS 4
#define BODY_LINES 3
#define MAX_LINES 32
#define LINE_BUF 32

typedef enum { SCREEN_SPLASH, SCREEN_BASICS, SCREEN_KEY } Screen;

typedef struct {
    const char* text;
    const char* detail;
    bool is_result;
    int num_options;
    const char* options[MAX_OPTIONS];
    int next[MAX_OPTIONS];
} Node;

typedef struct {
    FuriMessageQueue* input_queue;
    FuriMutex* mutex;
    Screen screen;
    int current_node;
    int selected;
    int scroll;
    int last_total;   // nb de lignes wrappées au dernier dessin (pour borner le scroll)
    int history[32];
    int history_depth;
} RockIdApp;

static const char* BASICS_TEXT =
    "Rocks fall into 3 families.\n\n"
    "IGNEOUS: cooled magma or lava. Hard, with crystals or glass.\n\n"
    "SEDIMENTARY: layers of grains, mud or shells. Often softer.\n\n"
    "METAMORPHIC: cooked and squeezed. Shows bands or shiny mica.\n\n"
    "FIELD TESTS: vinegar fizz means limestone or chalk. Scratch with a "
    "nail, coin or glass to judge hardness. Check grain size, layers, "
    "weight and colour.\n\n"
    "Pick what you see and follow the key, then use the Test line to "
    "confirm. Good luck!";

// ---------- Word-wrap ----------
static int wrap_text(Canvas* canvas, const char* text, int max_w, char out[MAX_LINES][LINE_BUF]) {
    int count = 0;
    char line[LINE_BUF];
    line[0] = '\0';
    const char* p = text;

    while(*p && count < MAX_LINES) {
        if(*p == ' ') {
            p++;
            continue;
        }
        if(*p == '\n') {
            strncpy(out[count], line, LINE_BUF - 1);
            out[count][LINE_BUF - 1] = '\0';
            count++;
            line[0] = '\0';
            p++;
            continue;
        }
        char word[LINE_BUF];
        int wl = 0;
        while(*p && *p != ' ' && *p != '\n' && wl < LINE_BUF - 1) word[wl++] = *p++;
        word[wl] = '\0';

        char test[LINE_BUF * 2];
        if(line[0] != '\0')
            snprintf(test, sizeof(test), "%s %s", line, word);
        else
            snprintf(test, sizeof(test), "%s", word);

        if(line[0] == '\0' || canvas_string_width(canvas, test) <= (uint16_t)max_w) {
            strncpy(line, test, LINE_BUF - 1);
            line[LINE_BUF - 1] = '\0';
        } else {
            strncpy(out[count], line, LINE_BUF - 1);
            out[count][LINE_BUF - 1] = '\0';
            count++;
            if(count >= MAX_LINES) break;
            strncpy(line, word, LINE_BUF - 1);
            line[LINE_BUF - 1] = '\0';
        }
    }
    if(line[0] != '\0' && count < MAX_LINES) {
        strncpy(out[count], line, LINE_BUF - 1);
        out[count][LINE_BUF - 1] = '\0';
        count++;
    }
    return count;
}

// encart "Back" style firmware
static void draw_back_pill(Canvas* canvas, const char* str) {
    canvas_set_font(canvas, FontSecondary);
    uint8_t tw = canvas_string_width(canvas, str);
    uint8_t w = tw + 8;
    uint8_t h = 13;
    uint8_t x = 128 - w;
    uint8_t y = 64 - h;
    canvas_set_color(canvas, ColorBlack);
    elements_slightly_rounded_box(canvas, x, y, w, h);
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_str(canvas, x + 4, y + 9, str);
    canvas_set_color(canvas, ColorBlack);
}

// ---------- Splash : coupe de strates ----------
// Courbe de "cuvette" : plonge au centre, remonte sur les bords (comme un bassin)
static int basin_y(int x, int base, int sag) {
    int dx = x - 64;
    return base + sag - (sag * dx * dx) / 4096; // 4096 = 64*64
}

static void draw_splash(Canvas* canvas) {
    canvas_set_color(canvas, ColorBlack);

    for(int x = 0; x < 128; x++) {
        int y0 = basin_y(x, 15, 3);   // surface du sol
        int y1 = basin_y(x, 21, 8);
        int y2 = basin_y(x, 29, 10);
        int y3 = basin_y(x, 37, 11);
        int y4 = basin_y(x, 45, 10);
        int yb = 64;

        // Couche 0 : dépôts récents (pointillés)
        for(int y = y0; y < y1; y++)
            if(((x + y) % 3) == 0) canvas_draw_dot(canvas, x, y);
        // Couche 1 : plein
        canvas_draw_line(canvas, x, y1, x, y2);
        // Couche 2 : hachures horizontales
        for(int y = y2; y < y3; y++)
            if((y % 3) == 0) canvas_draw_dot(canvas, x, y);
        // Couche 3 : claire (rien)
        // Couche 4 : socle plein
        canvas_draw_line(canvas, x, y4, x, yb);

        // Traits de séparation (les courbes)
        canvas_draw_dot(canvas, x, y0);
        canvas_draw_dot(canvas, x, y3);
    }

    // Titre discret, sans légende
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_box(canvas, 39, 1, 50, 13);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_rframe(canvas, 39, 1, 50, 13, 3);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 8, AlignCenter, AlignCenter, "ROCK ID");

    canvas_set_color(canvas, ColorBlack);
    elements_button_center(canvas, "Continue");
}

// ---------- L'arbre + mini-encyclo ----------
static const Node tree[] = {
    // 0 : RACINE
    { .text = "Main feature?", .num_options = 5,
      .options = {"Fizzes w/ vinegar", "Glassy or bubbly", "Layers or bands",
                  "Visible grains", "Smooth & fine"},
      .next = {1, 2, 3, 4, 5} },
    { .text = "Best match?", .num_options = 4,
      .options = {"Soft white chalk", "Shiny bands", "Shells / fossils", "Grey & dense"},
      .next = {6, 7, 8, 9} },
    { .text = "Best match?", .num_options = 3,
      .options = {"Black glass", "Pale, floats", "Dark, heavy, holes"},
      .next = {10, 11, 12} },
    { .text = "How does it split?", .num_options = 4,
      .options = {"Hard flat plates", "Soft mud layers", "Sparkly (mica)", "Light/dark bands"},
      .next = {13, 14, 15, 16} },
    { .text = "Grain type?", .num_options = 5,
      .options = {"Sand, rubs off", "Pebbles cemented", "Light crystals",
                  "Dark/green cryst.", "Grey crystals"},
      .next = {17, 18, 19, 20, 21} },
    { .text = "Best match?", .num_options = 5,
      .options = {"Dark, heavy, hard", "Pale, hard, plain", "Waxy, sharp edge",
                  "Soft, greasy", "Black, light"},
      .next = {22, 23, 24, 25, 26} },

    // ---- FICHES ----
    { .text = "Chalk", .is_result = true, .detail =
        "Sedimentary, very soft (~1), white and powdery.\n"
        "Test: leaves a white streak and fizzes in vinegar.\n"
        "Find: chalk cliffs and downs.\n"
        "Vs: harder limestone won't powder on your finger.\n"
        "Use: lime and blackboard chalk; the White Cliffs of Dover are chalk." },
    { .text = "Marble", .is_result = true, .detail =
        "Metamorphic, ~3, sugary interlocking crystals, often veined.\n"
        "Test: fizzes in vinegar AND takes a polish.\n"
        "Find: mountain belts and quarries like Carrara.\n"
        "Vs: quartzite looks similar but does NOT fizz.\n"
        "Use: statues and floors; Michelangelo's David is marble." },
    { .text = "Fossil limestone", .is_result = true, .detail =
        "Sedimentary, ~3, packed with visible shells and fossils.\n"
        "Test: fizzes in vinegar; fossils stand out.\n"
        "Find: old sea beds and sedimentary basins.\n"
        "Vs: plain limestone shows no fossils.\n"
        "Use: building stone; the Egyptian pyramids contain fossil limestone." },
    { .text = "Limestone", .is_result = true, .detail =
        "Sedimentary, ~3, grey, fine and dull (calcite).\n"
        "Test: fizzes strongly in vinegar.\n"
        "Find: hills, caves and quarries worldwide.\n"
        "Vs: sandstone is gritty and won't fizz.\n"
        "Use: makes most of the world's cement; also lime." },
    { .text = "Obsidian", .is_result = true, .detail =
        "Igneous volcanic glass, ~5-6, black and glossy.\n"
        "Test: breaks in curved shiny shells, razor-sharp.\n"
        "Find: around young volcanoes.\n"
        "Vs: coal is soft and sooty; obsidian scratches glass.\n"
        "Use: prehistoric blades; sharper than a steel scalpel." },
    { .text = "Pumice", .is_result = true, .detail =
        "Igneous foam, pale, full of gas holes, ~6 but crumbly.\n"
        "Test: it FLOATS on water.\n"
        "Find: volcanic areas, even washed up on beaches.\n"
        "Vs: scoria is darker and sinks.\n"
        "Use: skin scrub and light concrete; can raft across oceans." },
    { .text = "Scoria", .is_result = true, .detail =
        "Igneous, dark red to black, bubbly, heavier than pumice, ~5-6.\n"
        "Test: full of holes but SINKS in water.\n"
        "Find: volcanic cones and lava fields.\n"
        "Vs: basalt is dense with few holes.\n"
        "Use: BBQ and garden rock; spreads heat in gas grills." },
    { .text = "Slate", .is_result = true, .detail =
        "Metamorphic, ~3-4, splits into flat thin plates.\n"
        "Test: splits cleanly and rings when tapped.\n"
        "Find: mountain regions like Wales.\n"
        "Vs: shale is soft and crumbles instead of ringing.\n"
        "Use: roof tiles and old blackboards." },
    { .text = "Shale", .is_result = true, .detail =
        "Sedimentary, ~2-3, thin mud layers that crumble.\n"
        "Test: splits in layers and smells earthy when wet.\n"
        "Find: river banks and road cuts.\n"
        "Vs: mudstone breaks in blocks with no layers.\n"
        "Use: source of shale gas and oil; also brick clay." },
    { .text = "Schist", .is_result = true, .detail =
        "Metamorphic, ~4, sparkly mica in wavy flaky layers.\n"
        "Test: mica flakes glitter; may show garnet crystals.\n"
        "Find: mountain cores like the Alps.\n"
        "Vs: gneiss is banded and harder, not flaky.\n"
        "Use: often hosts gemstones; Manhattan sits on schist." },
    { .text = "Gneiss", .is_result = true, .detail =
        "Metamorphic, ~6-7, coarse with light and dark bands.\n"
        "Test: clear banding and won't crumble.\n"
        "Find: ancient shields and mountain roots.\n"
        "Vs: granite is evenly speckled with no bands.\n"
        "Use: countertops and curling stones; among Earth's oldest rocks." },
    { .text = "Sandstone", .is_result = true, .detail =
        "Sedimentary, ~6-7, gritty cemented sand grains.\n"
        "Test: rub it and sand grains come off on your fingers.\n"
        "Find: deserts, cliffs and canyons.\n"
        "Vs: granite has crystals, not loose grains.\n"
        "Use: building stone; Petra is carved from sandstone." },
    { .text = "Conglomerate", .is_result = true, .detail =
        "Sedimentary, rounded pebbles cemented like concrete.\n"
        "Test: visible rounded pebbles set in finer matrix.\n"
        "Find: old river beds and alluvial fans.\n"
        "Vs: breccia has ANGULAR fragments, not rounded.\n"
        "Use: decorative stone, nicknamed puddingstone." },
    { .text = "Granite", .is_result = true, .detail =
        "Igneous, ~6-7, salt-and-pepper interlocking crystals.\n"
        "Test: spot glassy quartz, pink/white feldspar, black mica.\n"
        "Find: mountain massifs and tors.\n"
        "Vs: diorite lacks quartz and pink feldspar.\n"
        "Use: countertops and monuments; Mount Rushmore is granite." },
    { .text = "Gabbro", .is_result = true, .detail =
        "Igneous, ~6, dark coarse crystals, heavy, no quartz.\n"
        "Test: coarse dark crystals and noticeably heavy.\n"
        "Find: layered intrusions and ocean crust.\n"
        "Vs: basalt is fine-grained with no visible crystals.\n"
        "Use: road stone and 'black granite' counters." },
    { .text = "Diorite", .is_result = true, .detail =
        "Igneous, ~6-7, grey black-and-white speckled, no quartz.\n"
        "Test: salt-and-pepper look but no glassy quartz.\n"
        "Find: mountain-building zones.\n"
        "Vs: granite shows glassy quartz and pink feldspar.\n"
        "Use: tough stone; Hammurabi's law code is carved on diorite." },
    { .text = "Basalt", .is_result = true, .detail =
        "Igneous, ~6, dark, dense, fine-grained, sometimes tiny holes.\n"
        "Test: heavy and fine; often slightly magnetic.\n"
        "Find: lava plateaus and ocean islands.\n"
        "Vs: gabbro is the same rock but coarse-grained.\n"
        "Use: road aggregate; the Giant's Causeway is basalt." },
    { .text = "Rhyolite", .is_result = true, .detail =
        "Igneous, ~6-7, pale, fine-grained, may show flow bands.\n"
        "Test: pale, fine, no fizz; sometimes banded.\n"
        "Find: silicic volcanic fields.\n"
        "Vs: basalt is dark; rhyolite is pale.\n"
        "Use: decorative stone; erupts from volcanoes like Yellowstone." },
    { .text = "Flint / Chert", .is_result = true, .detail =
        "Sedimentary silica, very hard (~7), waxy and glassy.\n"
        "Test: scratches glass and sparks against steel.\n"
        "Find: as nodules inside chalk and limestone.\n"
        "Vs: obsidian is glassy black; flint is opaque and waxy.\n"
        "Use: fire-starting and Stone Age tools." },
    { .text = "Mudstone", .is_result = true, .detail =
        "Sedimentary, ~2-3, smooth hardened mud, no layers.\n"
        "Test: smooth, breaks in blocks, earthy smell.\n"
        "Find: sedimentary basins and coastlines.\n"
        "Vs: shale splits into layers; mudstone doesn't.\n"
        "Use: brick and pottery clay." },
    { .text = "Coal", .is_result = true, .detail =
        "Sedimentary, ~1-2, black, dull and very light.\n"
        "Test: leaves black dust on your fingers and can burn.\n"
        "Find: coal seams in sedimentary basins.\n"
        "Vs: obsidian is hard and glassy; coal is soft and sooty.\n"
        "Use: fuel and power; formed in swamps 300 million years ago." },
};

// ---------- Article (titre + texte plein largeur + scrollbar) ----------
static void draw_article(Canvas* canvas, RockIdApp* app, const char* title, const char* body) {
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, title);
    canvas_draw_line(canvas, 0, 13, 127, 13);

    canvas_set_font(canvas, FontSecondary);
    char lines[MAX_LINES][LINE_BUF];
    int total = wrap_text(canvas, body, 116, lines);
    app->last_total = total; // mémorisé pour borner le scroll

    for(int i = 0; i < BODY_LINES; i++) {
        int idx = app->scroll + i;
        if(idx >= total) break;
        canvas_draw_str(canvas, 2, 24 + i * 11, lines[idx]);
    }

    if(total > BODY_LINES)
        elements_scrollbar_pos(canvas, 125, 15, 34, app->scroll, total - BODY_LINES + 1);
}

static void draw_menu(Canvas* canvas, RockIdApp* app, Node node) {
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, node.text);
    canvas_draw_line(canvas, 0, 14, 127, 14);
    canvas_set_font(canvas, FontSecondary);

    int first = 0;
    if(app->selected >= VISIBLE_OPTIONS) first = app->selected - VISIBLE_OPTIONS + 1;
    for(int i = first; i < node.num_options && i < first + VISIBLE_OPTIONS; i++) {
        int idx = i - first;
        int y = 15 + idx * 12;
        if(i == app->selected) {
            canvas_set_color(canvas, ColorBlack);
            elements_slightly_rounded_box(canvas, 0, y, 122, 11);
            canvas_set_color(canvas, ColorWhite);
        } else {
            canvas_set_color(canvas, ColorBlack);
        }
        canvas_draw_str(canvas, 4, y + 9, node.options[i]);
    }
    canvas_set_color(canvas, ColorBlack);
    if(node.num_options > VISIBLE_OPTIONS)
        elements_scrollbar(canvas, app->selected, node.num_options);
}

static void draw_callback(Canvas* canvas, void* ctx) {
    RockIdApp* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);

    if(app->screen == SCREEN_SPLASH) {
        draw_splash(canvas);
    } else if(app->screen == SCREEN_BASICS) {
        draw_article(canvas, app, "The basics", BASICS_TEXT);
        elements_button_left(canvas, "Back");
        elements_button_center(canvas, "Start");
    } else {
        Node node = tree[app->current_node];
        if(node.is_result) {
            draw_article(canvas, app, node.text, node.detail);
            draw_back_pill(canvas, "Back = start");
        } else {
            draw_menu(canvas, app, node);
        }
    }

    furi_mutex_release(app->mutex);
}

static void input_callback(InputEvent* input_event, void* ctx) {
    RockIdApp* app = ctx;
    furi_message_queue_put(app->input_queue, input_event, FuriWaitForever);
}

int32_t rock_id_app(void* p) {
    UNUSED(p);

    RockIdApp* app = malloc(sizeof(RockIdApp));
    app->input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->screen = SCREEN_SPLASH;
    app->current_node = 0;
    app->selected = 0;
    app->scroll = 0;
    app->last_total = 0;
    app->history_depth = 0;

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, draw_callback, app);
    view_port_input_callback_set(view_port, input_callback, app);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    InputEvent event;
    bool running = true;
    while(running) {
        if(furi_message_queue_get(app->input_queue, &event, FuriWaitForever) == FuriStatusOk) {
            if(event.type == InputTypePress) {
                furi_mutex_acquire(app->mutex, FuriWaitForever);

                if(app->screen == SCREEN_SPLASH) {
                    if(event.key == InputKeyOk) {
                        app->screen = SCREEN_BASICS;
                        app->scroll = 0;
                    } else if(event.key == InputKeyBack) {
                        running = false;
                    }
                } else if(app->screen == SCREEN_BASICS) {
                    switch(event.key) {
                        case InputKeyUp:
                            if(app->scroll > 0) app->scroll--;
                            break;
                        case InputKeyDown: {
                            int maxs = app->last_total - BODY_LINES;
                            if(maxs < 0) maxs = 0;
                            if(app->scroll < maxs) app->scroll++;
                            break;
                        }
                        case InputKeyOk:
                            app->screen = SCREEN_KEY;
                            app->current_node = 0;
                            app->selected = 0;
                            app->scroll = 0;
                            app->history_depth = 0;
                            break;
                        case InputKeyBack:
                            app->screen = SCREEN_SPLASH;
                            app->scroll = 0;
                            break;
                        default:
                            break;
                    }
                } else { // SCREEN_KEY
                    Node node = tree[app->current_node];
                    if(node.is_result) {
                        switch(event.key) {
                            case InputKeyUp:
                                if(app->scroll > 0) app->scroll--;
                                break;
                            case InputKeyDown: {
                                int maxs = app->last_total - BODY_LINES;
                                if(maxs < 0) maxs = 0;
                                if(app->scroll < maxs) app->scroll++;
                                break;
                            }
                            case InputKeyBack:
                                app->current_node = 0;
                                app->selected = 0;
                                app->scroll = 0;
                                app->history_depth = 0;
                                break;
                            default:
                                break;
                        }
                    } else {
                        switch(event.key) {
                            case InputKeyUp:
                                app->selected--;
                                if(app->selected < 0) app->selected = node.num_options - 1;
                                break;
                            case InputKeyDown:
                                app->selected++;
                                if(app->selected >= node.num_options) app->selected = 0;
                                break;
                            case InputKeyOk:
                                app->history[app->history_depth++] = app->current_node;
                                app->current_node = node.next[app->selected];
                                app->selected = 0;
                                app->scroll = 0;
                                break;
                            case InputKeyBack:
                                if(app->history_depth > 0) {
                                    app->current_node = app->history[--app->history_depth];
                                    app->selected = 0;
                                } else {
                                    app->screen = SCREEN_SPLASH;
                                    app->scroll = 0;
                                }
                                break;
                            default:
                                break;
                        }
                    }
                }

                furi_mutex_release(app->mutex);
            }
            view_port_update(view_port);
        }
    }

    gui_remove_view_port(gui, view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(view_port);
    furi_mutex_free(app->mutex);
    furi_message_queue_free(app->input_queue);
    free(app);

    return 0;
}