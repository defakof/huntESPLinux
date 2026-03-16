/*
 * Hunt ESP Overlay - OpenGL version
 *
 * Uses X11 ARGB visual + OpenGL + ImGui for transparent overlay on XWayland.
 * Reads game memory via kernel module (/dev/hunt_read).
 */

#include <GL/gl.h>

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_opengl3.h"
#include "imgui/backends/imgui_impl_glfw.h"

#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xfixes.h>
#include <X11/keysym.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <unistd.h>
#include <signal.h>
#include <time.h>

#include "game_data.h"

static constexpr double TARGET_FPS = 144.0;
static constexpr double FRAME_TIME = 1.0 / TARGET_FPS;

static volatile bool g_running = true;

static void signal_handler(int) {
    g_running = false;
}

/* ===== Settings ===== */
struct EspSettings {
    bool show_hunters = true;
    bool show_bosses = true;
    bool show_grunts = true;
    bool show_all = false;
    bool raw_names_hunters = false;
    bool raw_names_bosses = false;
    bool raw_names_grunts = false;
    float color_hunter[4] = {1.0f, 0.12f, 0.12f, 0.9f};
    float color_boss[4]   = {1.0f, 0.55f, 0.0f, 0.9f};
    float color_grunt[4]  = {1.0f, 1.0f, 0.2f, 0.78f};
    float color_unknown[4]= {0.78f, 0.78f, 0.78f, 0.7f};
};

static EspSettings g_settings;
static bool g_menu_open = false;
static std::vector<char> g_selected; /* per-entity selection state (char to avoid vector<bool>) */
static int g_last_clicked = -1;      /* last clicked row for shift-select */

static ImU32 color4_to_u32(const float c[4]) {
    return IM_COL32((int)(c[0]*255), (int)(c[1]*255), (int)(c[2]*255), (int)(c[3]*255));
}

static ImU32 get_type_color(EntityType type) {
    switch (type) {
    case ENT_HUNTER: return color4_to_u32(g_settings.color_hunter);
    case ENT_BOSS:   return color4_to_u32(g_settings.color_boss);
    case ENT_GRUNT:  return color4_to_u32(g_settings.color_grunt);
    default:         return color4_to_u32(g_settings.color_unknown);
    }
}

static const char *get_entity_label(EntityType type) {
    switch (type) {
    case ENT_HUNTER: return "Hunter";
    case ENT_BOSS:   return "Boss";
    case ENT_GRUNT:  return "Grunt";
    default:         return "???";
    }
}

static bool is_type_enabled(EntityType type) {
    switch (type) {
    case ENT_HUNTER: return g_settings.show_hunters;
    case ENT_BOSS:   return g_settings.show_bosses;
    case ENT_GRUNT:  return g_settings.show_grunts;
    default:         return g_settings.show_all;
    }
}

static bool use_raw_name(EntityType type) {
    switch (type) {
    case ENT_HUNTER: return g_settings.raw_names_hunters;
    case ENT_BOSS:   return g_settings.raw_names_bosses;
    case ENT_GRUNT:  return g_settings.raw_names_grunts;
    default:         return true;
    }
}

/* ===== World-to-Screen projection ===== */
static bool world_to_screen(const Matrix44 &render_mat, const Matrix44 &proj_mat,
                            const Vec3 &pos, float screen_w, float screen_h,
                            float &sx, float &sy) {
    float tx = pos.x * render_mat.m[0][0] + pos.y * render_mat.m[1][0] +
               pos.z * render_mat.m[2][0] + render_mat.m[3][0];
    float ty = pos.x * render_mat.m[0][1] + pos.y * render_mat.m[1][1] +
               pos.z * render_mat.m[2][1] + render_mat.m[3][1];
    float tz = pos.x * render_mat.m[0][2] + pos.y * render_mat.m[1][2] +
               pos.z * render_mat.m[2][2] + render_mat.m[3][2];
    float tw = pos.x * render_mat.m[0][3] + pos.y * render_mat.m[1][3] +
               pos.z * render_mat.m[2][3] + render_mat.m[3][3];

    if (tz >= 0.0f) return false;

    float px = tx * proj_mat.m[0][0] + ty * proj_mat.m[1][0] +
               tz * proj_mat.m[2][0] + tw * proj_mat.m[3][0];
    float py = tx * proj_mat.m[0][1] + ty * proj_mat.m[1][1] +
               tz * proj_mat.m[2][1] + tw * proj_mat.m[3][1];
    float pw = tx * proj_mat.m[0][3] + ty * proj_mat.m[1][3] +
               tz * proj_mat.m[2][3] + tw * proj_mat.m[3][3];

    if (pw == 0.0f) return false;
    px /= pw;
    py /= pw;
    if (fabsf(px) > 1.5f || fabsf(py) > 1.5f) return false;

    sx = (1.0f + px) * screen_w * 0.5f;
    sy = (1.0f - py) * screen_h * 0.5f;
    return true;
}

/* ===== ESP Drawing ===== */
static void draw_esp(const GameState &state) {
    ImDrawList *draw = ImGui::GetBackgroundDrawList();
    float sw = (float)state.screen_w;
    float sh = (float)state.screen_h;

    for (const auto &ent : state.entities) {
        if (!is_type_enabled(ent.type)) continue;

        float dist = state.local_pos.distance(ent.position);
        ImU32 color = get_type_color(ent.type);

        float sx, sy;
        if (!world_to_screen(state.render_mat, state.proj_mat, ent.position, sw, sh, sx, sy))
            continue;

        /* Box top = head or estimated top of entity */
        Vec3 top = ent.position;
        top.z += (ent.type == ENT_HUNTER) ? 1.8f : 2.0f;
        float hx, hy;
        if (!world_to_screen(state.render_mat, state.proj_mat, top, sw, sh, hx, hy))
            continue;

        float box_h = fabsf(sy - hy);
        if (box_h < 3.0f) continue;
        float box_w = box_h * 0.45f;

        float x1 = sx - box_w * 0.5f, y1 = hy;
        float x2 = sx + box_w * 0.5f, y2 = sy;

        draw->AddRect(ImVec2(x1-1,y1-1), ImVec2(x2+1,y2+1), IM_COL32(0,0,0,200), 0, 0, 1.0f);
        draw->AddRect(ImVec2(x1,y1), ImVec2(x2,y2), color, 0, 0, 1.5f);

        /* Head dot — disabled for now, bone offsets need more work
        float dot_r = box_h * 0.06f;
        if (dot_r < 2.0f) dot_r = 2.0f;
        float hdx, hdy;
        if (ent.has_head &&
            world_to_screen(state.render_mat, state.proj_mat, ent.head_pos, sw, sh, hdx, hdy)) {
            draw->AddCircleFilled(ImVec2(hdx, hdy), dot_r + 1.0f, IM_COL32(0,0,0,200));
            draw->AddCircleFilled(ImVec2(hdx, hdy), dot_r, color);
        } else {
            float head_y = y1 + box_h * 0.08f;
            draw->AddCircleFilled(ImVec2(sx, head_y), dot_r + 1.0f, IM_COL32(0,0,0,200));
            draw->AddCircleFilled(ImVec2(sx, head_y), dot_r, color);
        }
        */

        /* Label */
        char label[128];
        const char *disp = ent.name[0] ? ent.name : get_entity_label(ent.type);
        if (use_raw_name(ent.type) && ent.raw_name[0])
            disp = ent.raw_name;
        snprintf(label, sizeof(label), "%s [%.0fm]", disp, dist);

        ImVec2 tsz = ImGui::CalcTextSize(label);
        float lx = sx - tsz.x * 0.5f;
        float ly = y1 - tsz.y - 2;
        draw->AddText(ImVec2(lx+1,ly+1), IM_COL32(0,0,0,220), label);
        draw->AddText(ImVec2(lx,ly), color, label);

        /* Snap line */
        draw->AddLine(ImVec2(sw*0.5f, sh), ImVec2(sx, sy),
                      IM_COL32(color&0xFF,(color>>8)&0xFF,(color>>16)&0xFF,80), 1.0f);
    }

    /* Status bar */
    char status[64];
    snprintf(status, sizeof(status), "ESP | %zu entities", state.entities.size());
    draw->AddText(ImVec2(10, 10), IM_COL32(0, 255, 0, 180), status);
}

/* ===== Menu ===== */
static void draw_menu(const GameState &state) {
    ImGui::SetNextWindowSize(ImVec2(420, 400), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(100, 100), ImGuiCond_FirstUseEver);
    ImGui::Begin("Hunt ESP", &g_menu_open, ImGuiWindowFlags_NoCollapse);

    if (ImGui::BeginTabBar("Tabs")) {
        if (ImGui::BeginTabItem("Settings")) {
            ImGui::SeparatorText("Visibility");
            ImGui::Checkbox("Show Hunters", &g_settings.show_hunters);
            ImGui::SameLine(); ImGui::Checkbox("Raw names##h", &g_settings.raw_names_hunters);
            ImGui::Checkbox("Show Bosses", &g_settings.show_bosses);
            ImGui::SameLine(); ImGui::Checkbox("Raw names##b", &g_settings.raw_names_bosses);
            ImGui::Checkbox("Show Grunts", &g_settings.show_grunts);
            ImGui::SameLine(); ImGui::Checkbox("Raw names##g", &g_settings.raw_names_grunts);
            ImGui::Separator();
            ImGui::Checkbox("Show ALL entities (unfiltered)", &g_settings.show_all);

            ImGui::SeparatorText("Colors");
            ImGui::ColorEdit4("Hunter##col", g_settings.color_hunter, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
            ImGui::ColorEdit4("Boss##col",   g_settings.color_boss,   ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
            ImGui::ColorEdit4("Grunt##col",  g_settings.color_grunt,  ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
            ImGui::ColorEdit4("Unknown##col",g_settings.color_unknown,ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);

            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Entities")) {
            /* Resize selection vector to match entity count */
            g_selected.resize(state.entities.size(), 0);
            if (g_selected.size() > state.entities.size())
                g_selected.resize(state.entities.size());

            /* Count selected */
            int sel_count = 0;
            for (bool s : g_selected) if (s) sel_count++;

            ImGui::Text("Visible: %zu | Selected: %d", state.entities.size(), sel_count);
            ImGui::SameLine();
            if (ImGui::Button("Copy Selected") && sel_count > 0) {
                std::string clip;
                for (size_t i = 0; i < state.entities.size(); i++) {
                    if (!g_selected[i]) continue;
                    auto &e = state.entities[i];
                    float d = state.local_pos.distance(e.position);
                    char line[256];
                    snprintf(line, sizeof(line), "%s\t%s\t%.0fm\t%.0f,%.0f,%.0f\n",
                             get_entity_label(e.type),
                             e.raw_name[0] ? e.raw_name : e.name,
                             d, e.position.x, e.position.y, e.position.z);
                    clip += line;
                }
                ImGui::SetClipboardText(clip.c_str());
            }
            ImGui::SameLine();
            if (ImGui::Button("Select All")) {
                for (auto &s : g_selected) s = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear")) {
                for (auto &s : g_selected) s = false;
                g_last_clicked = -1;
            }
            ImGui::Separator();

            if (ImGui::BeginTable("ent_table", 4,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                    ImVec2(0, -1))) {
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Dist", ImGuiTableColumnFlags_WidthFixed, 50);
                ImGui::TableSetupColumn("Pos", ImGuiTableColumnFlags_WidthFixed, 140);
                ImGui::TableHeadersRow();

                for (size_t i = 0; i < state.entities.size(); i++) {
                    auto &ent = state.entities[i];
                    ImGui::TableNextRow();
                    ImU32 col = get_type_color(ent.type);
                    ImVec4 colv = ImGui::ColorConvertU32ToFloat4(col);

                    /* Selectable spanning full row */
                    ImGui::TableSetColumnIndex(0);
                    char sel_id[32];
                    snprintf(sel_id, sizeof(sel_id), "##row%zu", i);
                    bool selected = (i < g_selected.size()) ? g_selected[i] : false;
                    if (ImGui::Selectable(sel_id, selected,
                            ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                        ImGuiIO &sio = ImGui::GetIO();
                        if (sio.KeyShift && g_last_clicked >= 0) {
                            /* Shift+click: range select */
                            int lo = (g_last_clicked < (int)i) ? g_last_clicked : (int)i;
                            int hi = (g_last_clicked > (int)i) ? g_last_clicked : (int)i;
                            for (int r = lo; r <= hi; r++)
                                g_selected[r] = true;
                        } else if (sio.KeyCtrl) {
                            /* Ctrl+click: toggle single */
                            g_selected[i] = !g_selected[i];
                        } else {
                            /* Plain click: select only this */
                            for (auto &s : g_selected) s = false;
                            g_selected[i] = true;
                        }
                        g_last_clicked = (int)i;
                    }
                    ImGui::SameLine();
                    ImGui::TextColored(colv, "%s", get_entity_label(ent.type));

                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextColored(colv, "%s", ent.raw_name[0] ? ent.raw_name : ent.name);

                    ImGui::TableSetColumnIndex(2);
                    float dist = state.local_pos.distance(ent.position);
                    ImGui::Text("%.0fm", dist);

                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%.0f,%.0f,%.0f", ent.position.x, ent.position.y, ent.position.z);
                }

                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}

/* ===== Click-through toggling ===== */
static void set_clickthrough(Display *dpy, Window win, bool passthrough) {
    if (passthrough) {
        XserverRegion region = XFixesCreateRegion(dpy, NULL, 0);
        XFixesSetWindowShapeRegion(dpy, win, ShapeInput, 0, 0, region);
        XFixesDestroyRegion(dpy, region);
    } else {
        XFixesSetWindowShapeRegion(dpy, win, ShapeInput, 0, 0, None);
    }
    XFlush(dpy);
}

/* ===== Find Hunt process PID ===== */
static int32_t find_hunt_pid() {
    FILE *fp = popen("pgrep -f 'HuntGame.exe' | head -1", "r");
    if (!fp) return 0;
    int32_t pid = 0;
    if (fscanf(fp, "%d", &pid) != 1) pid = 0;
    pclose(fp);
    return pid;
}

/* ===== Main ===== */
int main(int argc, char **argv) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    int32_t pid;
    if (argc > 1) {
        pid = atoi(argv[1]);
    } else {
        pid = find_hunt_pid();
        if (pid == 0) {
            fprintf(stderr, "Hunt: Showdown not found. Pass PID manually: %s <pid>\n", argv[0]);
            return 1;
        }
        printf("Found HuntGame.exe at PID %d\n", pid);
    }

    GameData game;
    if (!game.init(pid)) {
        fprintf(stderr, "Failed to initialize game data reader\n");
        return 1;
    }

    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
    if (!glfwInit()) { fprintf(stderr, "Failed to initialize GLFW\n"); return 1; }

    GLFWmonitor *monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode *mode = glfwGetVideoMode(monitor);
    int screen_w = mode->width, screen_h = mode->height;
    printf("Screen: %dx%d\n", screen_w, screen_h);

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
    glfwWindowHint(GLFW_MOUSE_PASSTHROUGH, GLFW_TRUE);
    glfwWindowHint(GLFW_ALPHA_BITS, 8);
    glfwWindowHint(GLFW_SAMPLES, 0);

    GLFWwindow *window = glfwCreateWindow(screen_w, screen_h, "overlay", nullptr, nullptr);
    if (!window) { fprintf(stderr, "Failed to create window\n"); glfwTerminate(); return 1; }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(0);
    glfwSetWindowPos(window, 0, 0);

    /* X11 setup */
    Display *xdpy = glfwGetX11Display();
    Window xwin = glfwGetX11Window(window);

    /* Override-redirect: bypass window manager stacking.
     * This makes the overlay an unmanaged surface that renders above
     * fullscreen windows on KDE Wayland/XWayland. */
    XUnmapWindow(xdpy, xwin);
    XSetWindowAttributes or_attrs = {};
    or_attrs.override_redirect = True;
    XChangeWindowAttributes(xdpy, xwin, CWOverrideRedirect, &or_attrs);
    XMapWindow(xdpy, xwin);
    XFlush(xdpy);

    set_clickthrough(xdpy, xwin, true);

    /* Position at top-left (re-apply after remap) */
    glfwSetWindowPos(window, 0, 0);

    /* ImGui setup */
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowBorderSize = 1.0f;
    style.WindowRounding = 6.0f;
    style.FrameRounding = 3.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.08f, 0.92f);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    GameState state = {};
    state.screen_w = screen_w;
    state.screen_h = screen_h;

    printf("ESP overlay running. Insert=menu, Ctrl+C=quit.\n");

    Atom wm_state_atom = XInternAtom(xdpy, "_NET_WM_STATE", False);
    Atom wm_above_atom = XInternAtom(xdpy, "_NET_WM_STATE_ABOVE", False);
    int frame_count = 0;
    bool insert_was_pressed = false;
    KeyCode insert_kc = XKeysymToKeycode(xdpy, XK_Insert);

    while (!glfwWindowShouldClose(window) && g_running) {
        glfwPollEvents();

        /* Poll global X11 keyboard for Insert key (works without focus) */
        char keymap[32] = {};
        XQueryKeymap(xdpy, keymap);
        bool insert_pressed = (keymap[insert_kc / 8] >> (insert_kc % 8)) & 1;
        if (insert_pressed && !insert_was_pressed) {
            g_menu_open = !g_menu_open;
            set_clickthrough(xdpy, xwin, !g_menu_open);
            if (g_menu_open)
                glfwFocusWindow(window);
        }
        insert_was_pressed = insert_pressed;

        /* Pass show_all to game data reader */
        state.show_all = g_settings.show_all;

        /* Raise window periodically */
        if (++frame_count % 288 == 0) {
            XRaiseWindow(xdpy, xwin);
            XEvent ev = {};
            ev.xclient.type = ClientMessage;
            ev.xclient.window = xwin;
            ev.xclient.message_type = wm_state_atom;
            ev.xclient.format = 32;
            ev.xclient.data.l[0] = 1;
            ev.xclient.data.l[1] = wm_above_atom;
            XSendEvent(xdpy, DefaultRootWindow(xdpy), False,
                       SubstructureRedirectMask | SubstructureNotifyMask, &ev);
            XFlush(xdpy);
        }

        game.update(state);

        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (state.valid)
            draw_esp(state);
        else {
            ImDrawList *draw = ImGui::GetBackgroundDrawList();
            draw->AddText(ImVec2(10, 10), IM_COL32(255, 0, 0, 255),
                         "ESP: No game data (reading memory...)");
        }

        if (g_menu_open)
            draw_menu(state);

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);

        /* Frame limiter */
        double now = glfwGetTime();
        static double last_frame = 0.0;
        double elapsed = now - last_frame;
        if (elapsed < FRAME_TIME) {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = (long)((FRAME_TIME - elapsed) * 1e9);
            nanosleep(&ts, nullptr);
        }
        last_frame = glfwGetTime();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    printf("ESP overlay stopped.\n");
    return 0;
}
