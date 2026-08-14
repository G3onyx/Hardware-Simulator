#include <iostream>
#include <chrono>
#include <thread>
#include <algorithm>
#include <fstream>
#include <unordered_map>
#include <GLFW/glfw3.h>

#include "../testing/AutoTester.h"
#include "Utils.h"
#include "DynamicChipManager.h"


struct KeyboardDriver {
    struct KeyBind { int glfw_key; uint64_t mask; };
    std::vector<KeyBind> low_binds, high_binds;

    struct PinBind { std::vector<int> glfw_keys; std::string pin_name; mutable bool was_pressed = false; };
    std::vector<PinBind> pin_binds;

    void load(const std::string &path, const Config &config) {
        static const std::unordered_map<std::string, int> glfw_map{
            {"KEY_A", GLFW_KEY_A}, {"KEY_B", GLFW_KEY_B}, {"KEY_C", GLFW_KEY_C},
            {"KEY_D", GLFW_KEY_D}, {"KEY_E", GLFW_KEY_E}, {"KEY_F", GLFW_KEY_F},
            {"KEY_G", GLFW_KEY_G}, {"KEY_H", GLFW_KEY_H}, {"KEY_I", GLFW_KEY_I},
            {"KEY_J", GLFW_KEY_J}, {"KEY_K", GLFW_KEY_K}, {"KEY_L", GLFW_KEY_L},
            {"KEY_M", GLFW_KEY_M}, {"KEY_N", GLFW_KEY_N}, {"KEY_O", GLFW_KEY_O},
            {"KEY_P", GLFW_KEY_P}, {"KEY_Q", GLFW_KEY_Q}, {"KEY_R", GLFW_KEY_R},
            {"KEY_S", GLFW_KEY_S}, {"KEY_T", GLFW_KEY_T}, {"KEY_U", GLFW_KEY_U},
            {"KEY_V", GLFW_KEY_V}, {"KEY_W", GLFW_KEY_W}, {"KEY_X", GLFW_KEY_X},
            {"KEY_Y", GLFW_KEY_Y}, {"KEY_Z", GLFW_KEY_Z},

            {"KEY_0", GLFW_KEY_0}, {"KEY_1", GLFW_KEY_1}, {"KEY_2", GLFW_KEY_2},
            {"KEY_3", GLFW_KEY_3}, {"KEY_4", GLFW_KEY_4}, {"KEY_5", GLFW_KEY_5},
            {"KEY_6", GLFW_KEY_6}, {"KEY_7", GLFW_KEY_7}, {"KEY_8", GLFW_KEY_8},
            {"KEY_9", GLFW_KEY_9},

            {"KEY_SPACE", GLFW_KEY_SPACE}, {"KEY_ENTER", GLFW_KEY_ENTER},
            {"KEY_BACKSPACE", GLFW_KEY_BACKSPACE}, {"KEY_ESCAPE", GLFW_KEY_ESCAPE},

            {"KEY_MINUS", GLFW_KEY_MINUS}, {"KEY_EQUAL", GLFW_KEY_EQUAL},
            {"KEY_LEFT_BRACKET", GLFW_KEY_LEFT_BRACKET}, {"KEY_RIGHT_BRACKET", GLFW_KEY_RIGHT_BRACKET},
            {"KEY_SEMICOLON", GLFW_KEY_SEMICOLON}, {"KEY_APOSTROPHE", GLFW_KEY_APOSTROPHE},
            {"KEY_COMMA", GLFW_KEY_COMMA}, {"KEY_PERIOD", GLFW_KEY_PERIOD}, {"KEY_SLASH", GLFW_KEY_SLASH},

            {"KEY_UP", GLFW_KEY_UP}, {"KEY_DOWN", GLFW_KEY_DOWN},
            {"KEY_LEFT", GLFW_KEY_LEFT}, {"KEY_RIGHT", GLFW_KEY_RIGHT},

            {"KEY_LEFT_SHIFT", GLFW_KEY_LEFT_SHIFT}, {"KEY_RIGHT_SHIFT", GLFW_KEY_RIGHT_SHIFT},
            {"KEY_LEFT_CONTROL", GLFW_KEY_LEFT_CONTROL}, {"KEY_RIGHT_CONTROL", GLFW_KEY_RIGHT_CONTROL},
            {"KEY_LEFT_ALT", GLFW_KEY_LEFT_ALT}, {"KEY_RIGHT_ALT", GLFW_KEY_RIGHT_ALT},

            {"KEY_F1", GLFW_KEY_F1}, {"KEY_F2", GLFW_KEY_F2}, {"KEY_F3", GLFW_KEY_F3},
            {"KEY_F4", GLFW_KEY_F4}, {"KEY_F5", GLFW_KEY_F5}, {"KEY_F6", GLFW_KEY_F6},
            {"KEY_F7", GLFW_KEY_F7}, {"KEY_F8", GLFW_KEY_F8}, {"KEY_F9", GLFW_KEY_F9},
            {"KEY_F10", GLFW_KEY_F10}, {"KEY_F11", GLFW_KEY_F11}, {"KEY_F12", GLFW_KEY_F12}
        };

        std::ifstream stream(path);
        if (!stream.is_open()) {
            std::string abs_path = std::filesystem::absolute(path).string();
            throw std::runtime_error("\n[CRITICAL ERROR] Could not open keys file\nExpected absolute location:\n -> " + abs_path + "\n");
        }

        const auto trim = [](std::string& s) {
            s.erase(0, s.find_first_not_of(" \t\r\n"));
            s.erase(s.find_last_not_of(" \t\r\n") + 1);
        };

        std::string line;
        bool is_high_bus = false;

        // Process keys.const
        while (std::getline(stream, line)) {
            if (line.find("HIGH KEYS") != std::string::npos)
                is_high_bus = true;

            if (const auto pos = line.find("//"); pos != std::string::npos)
                line.resize(pos);

            const auto eq = line.find('=');
            if (eq == std::string::npos)
                continue;

            std::string id = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            trim(id);
            trim(val);

            if (!val.empty() && val.back() == ';')
                val.pop_back();

            if (!glfw_map.contains(id))
                continue;

            const auto bind = KeyBind{glfw_map.at(id), 1ULL << std::stoull(val)};
            (is_high_bus ? high_binds : low_binds).push_back(bind);
        }

        std::cout << "[Keyboard] Loaded " << low_binds.size() << " low keys and " << high_binds.size() << " high keys\n";

        // Process pin actions
        for (const auto& [pin_name, key_str] : config.pin_actions) {
            std::vector<int> keys;
            std::stringstream ss(key_str);
            std::string token;

            while (std::getline(ss, token, '+')) {
                if (!glfw_map.contains("KEY_" + token)) {
                    std::cerr << "[Keyboard] Warning: Unknown key '" << token << "' in combination for PIN_ACTION\n";
                    keys.clear();
                    break;
                }
                keys.push_back(glfw_map.at("KEY_" + token));
            }

            if (keys.empty())
                continue;

            pin_binds.push_back({keys, pin_name});

            std::cout << "[Keyboard] Bound combination '";
            for (size_t i = 0; i < keys.size(); ++i) {
                if (i) std::cout << " + ";

                auto it = std::ranges::find_if(glfw_map,
                    [&](const auto& pair) { return pair.second == keys[i]; });

                if (it != glfw_map.end())
                    std::cout << it->first.substr(4);
            }
            std::cout << "' to pin '" << pin_name << "'\n";
        }
    }

    [[nodiscard]] uint64_t poll_low(GLFWwindow *window) const {
        uint64_t keys = 0;
        for (const auto &b: low_binds) {
            if (glfwGetKey(window, b.glfw_key) == GLFW_PRESS) keys |= b.mask;
        }
        return keys;
    }

    [[nodiscard]] uint64_t poll_high(GLFWwindow *window) const {
        uint64_t keys = 0;
        for (const auto &b: high_binds) {
            if (glfwGetKey(window, b.glfw_key) == GLFW_PRESS) keys |= b.mask;
        }
        return keys;
    }

    void apply_pins(GLFWwindow* window, IDynamicChip* chip) const {
        for (const PinBind& b : pin_binds) {
            try {
                // Ensure all keys in the combination are pressed
                bool is_pressed = true;
                for (const int key : b.glfw_keys) {
                    if (glfwGetKey(window, key) != GLFW_PRESS) {
                        is_pressed = false;
                        break;
                    }
                }

                if (is_pressed && !b.was_pressed) std::cout << "[Simulator] Triggered '" << b.pin_name << "'\n";
                b.was_pressed = is_pressed;
                if (chip) chip->set_pin(b.pin_name.c_str(), is_pressed ? 1 : 0);
            } catch (...) {}
        }
    }
};


/// Window creation and setup
GLFWwindow* init_glfw_window() {
    if (!glfwInit()) return nullptr;

    GLFWmonitor* primary_monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* video_mode = glfwGetVideoMode(primary_monitor);
    const int max_dim = static_cast<int>(std::min(video_mode->width, video_mode->height) * 0.75);

    GLFWwindow* window = glfwCreateWindow(max_dim, max_dim, "Hardware Simulator", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return nullptr;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(0);
    return window;
}


/// Hot reloading pipeline
bool handle_hot_reloading(GLFWwindow* window, DynamicChipManager& chip_manager, Config& config,
                          const std::string& exe_dir, bool& was_F5_pressed, const GLuint screen_texture) {

    // Check for manual recompilation trigger (F5)
    const bool is_F5_pressed = glfwGetKey(window, GLFW_KEY_F5) == GLFW_PRESS;
    if (is_F5_pressed && !was_F5_pressed && !chip_manager.is_busy()) {
        std::cout << "\n[Host] Hot-reload requested via F5\n";
        try {
            config = Config::load();
            chip_manager.request_load(config.target_chip, exe_dir, config.search_paths);
        } catch (const std::exception& e) {
            std::cerr << e.what() << "\nFix the config file and press F5 again to retry\n";
        }
    }
    was_F5_pressed = is_F5_pressed;

    // Check if a pending async compilation has successfully finished
    if (chip_manager.check_and_swap(config.target_chip)) {
        const HardwareState info = chip_manager.get_chip()->get_info();

        // Reconfigure the window's aspect ratio and OpenGL texture dimensions for the new chip
        if (info.has_screen) {
            glfwSetWindowAspectRatio(window, static_cast<int>(info.screen_width), static_cast<int>(info.screen_height));
            glBindTexture(GL_TEXTURE_2D, screen_texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<int>(info.screen_width), static_cast<int>(info.screen_height), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        } else
            glfwSetWindowAspectRatio(window, GLFW_DONT_CARE, GLFW_DONT_CARE);

        if (IDynamicChip* chip = chip_manager.get_chip()) {
            for (const auto& data : config.rom_map)
                chip->load_rom(data.first.c_str(), data.second.c_str());
        }

        return true;
    }

    return false;
}


/// OpenGL rendering
void render_screen(const HardwareState& info, const GLuint screen_texture, const bool state_changed) {
    if (!info.has_screen) return;

    glBindTexture(GL_TEXTURE_2D, screen_texture);

    // Only blast pixels to the GPU if the simulation actually advanced this frame
    if (state_changed)
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<int>(info.screen_width), static_cast<int>(info.screen_height), GL_RGBA, GL_UNSIGNED_BYTE, info.screen_pixels);

    glEnable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f, 1.0f);
    glTexCoord2f(1.0f, 0.0f); glVertex2f(1.0f, 1.0f);
    glTexCoord2f(1.0f, 1.0f); glVertex2f(1.0f, -1.0f);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(-1.0f, -1.0f);
    glEnd();
    glDisable(GL_TEXTURE_2D);
}


struct PerformanceAnalyzer {
    int frames_this_second = 0;
    uint64_t cycles_this_second = 0;
    double sim_time_this_second = 0.0;
    std::chrono::time_point<std::chrono::high_resolution_clock> last_report_time = std::chrono::high_resolution_clock::now();

    void record_batch(const uint64_t cycles, const double sim_time_sec) {
        cycles_this_second += cycles;
        sim_time_this_second += sim_time_sec;
        frames_this_second++;
    }

    void update_and_report(GLFWwindow* window, const std::string& chip_name, const double target_hz, const double actual_hz) {
        const auto current_time = std::chrono::high_resolution_clock::now();
        const std::chrono::duration<double> elapsed = current_time - last_report_time;

        if (elapsed.count() >= 1.0) {
            double current_fps = frames_this_second / elapsed.count();
            double current_mhz = cycles_this_second / elapsed.count() / 1000000.0;

            // Percentage of the frame spent purely in emulate_frame() vs OS/rendering
            double sim_load_pct = (sim_time_this_second / elapsed.count()) * 100.0;

            std::string title;
            if (target_hz > 0.0)
                title = std::format("Simulating {} @ {:.2f} MHz [{:.1f} FPS | Sim Load: {:.1f}%]", chip_name, target_hz / 1000000.0, current_fps, sim_load_pct);
            else
                title = std::format("Simulating {} @ MAX {:.2f} MHz [{:.1f} FPS | Sim Load: {:.1f}%]", chip_name, current_mhz, current_fps, sim_load_pct);

            glfwSetWindowTitle(window, title.c_str());

            frames_this_second = 0;
            cycles_this_second = 0;
            sim_time_this_second = 0.0;
            last_report_time = current_time;
        }
    }
};


/// CAPPED MODE: Accurately paces the simulator to a specific frequency
uint64_t run_capped_mode(IDynamicChip* chip, const double target_hz, const double dt, const bool is_paused, bool& step_requested,
                         double& cycle_accumulator, const KeyboardDriver& kb_driver, GLFWwindow* window, double& sim_time_sec) {

    cycle_accumulator += dt * target_hz;
    const auto cycles_this_frame = static_cast<uint64_t>(cycle_accumulator);
    cycle_accumulator -= static_cast<double>(cycles_this_frame);

    uint64_t cycles_to_run = cycles_this_frame;
    if (is_paused) {
        cycles_to_run = step_requested ? 1 : 0;
        step_requested = false;
        cycle_accumulator = 0.0; // Prevent tick-bursting on unpause
    }

    if (cycles_to_run > 0) {
        const auto t0 = std::chrono::high_resolution_clock::now();
        chip->emulate_frame(cycles_to_run, kb_driver.poll_low(window), kb_driver.poll_high(window));
        const auto t1 = std::chrono::high_resolution_clock::now();

        sim_time_sec = std::chrono::duration<double>(t1 - t0).count();
        return cycles_to_run;
    }

    std::this_thread::yield();
    sim_time_sec = 0.0;
    return 0;
}


/// UNCAPPED MODE: dynamically discovers max speed and saturates 95% of the frame budget
uint64_t run_uncapped_mode(IDynamicChip* chip, const bool is_paused, bool& step_requested, double& actual_hz,
                           const KeyboardDriver& kb_driver, GLFWwindow* window,
                           const std::chrono::time_point<std::chrono::high_resolution_clock>& frame_start_time,
                           double& sim_time_sec) {

    static uint64_t cycles_per_frame = 50000;
    constexpr double TARGET_FRAME_DT = 1.0 / Constants::MAX_FPS;

    uint64_t cycles_to_run = cycles_per_frame;
    if (is_paused) {
        cycles_to_run = step_requested ? 1 : 0;
        step_requested = false;
    }

    const auto t0 = std::chrono::high_resolution_clock::now();
    if (cycles_to_run > 0) chip->emulate_frame(cycles_to_run, kb_driver.poll_low(window), kb_driver.poll_high(window));
    const auto t1 = std::chrono::high_resolution_clock::now();
    sim_time_sec = std::chrono::duration<double>(t1 - t0).count();

    // Tune dynamic batch size
    if (!is_paused && sim_time_sec > 0.0001) {
        const double current_cps = cycles_per_frame / sim_time_sec;
        static double smoothed_cps = current_cps;
        smoothed_cps = smoothed_cps * 0.90 + current_cps * 0.10; // Stabilize heavily with EMA

        // Target 95% of the frame budget to leave a sliver of time for OS execution/rendering
        cycles_per_frame = static_cast<uint64_t>(smoothed_cps * TARGET_FRAME_DT * 0.95);
        if (cycles_per_frame < 1000) cycles_per_frame = 1000; // Floor safety net
    }

    // Accurate hybrid spin-sleep lock
    while (true) {
        std::chrono::duration<double> total_elapsed = std::chrono::high_resolution_clock::now() - frame_start_time;
        if (total_elapsed.count() >= TARGET_FRAME_DT) break;

        if (TARGET_FRAME_DT - total_elapsed.count() > 0.002)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        else
            std::this_thread::yield();
    }

    actual_hz = is_paused ? 0.0 : (static_cast<double>(cycles_per_frame) / TARGET_FRAME_DT);
    return cycles_to_run;
}


/// Main simulator
int main([[maybe_unused]] const int argc, char *argv[]) {
    // File system anchor
    const std::string exe_dir = Utils::normalize_working_dir(argv[0]);
    Utils::confirm_resource_dirs();

    // Config setup
    Config config;
    try { config = Config::load(); }
    catch (const std::exception& e) { std::cerr << "\n" << e.what() << "\n\n"; return -1; }

    // Driver setup
    KeyboardDriver kb_driver;
    try { kb_driver.load(Utils::find_keys_file(config.search_paths), config); }
    catch (const std::exception& e) { std::cerr << "\n" << e.what() << "\n\n"; return -1; }

    // Testing
    if (config.run_tests) return AutoTester::generate_and_run_suite(config.search_paths, exe_dir);

    // Window and memory setup
    GLFWwindow* window = init_glfw_window();
    if (!window) return -1;

    GLuint screen_texture = 0;
    glGenTextures(1, &screen_texture);

    DynamicChipManager chip_manager;
    chip_manager.request_load(config.target_chip, exe_dir, config.search_paths);

    // Performance analyzer and simulation state
    PerformanceAnalyzer analyzer;
    auto last_frame_time = std::chrono::high_resolution_clock::now();
    double cycle_accumulator = 0.0;
    bool was_F5_pressed = false;

    bool is_paused = config.start_paused;
    uint64_t total_cycles = 0;

    bool step_requested = false;
    bool was_pause_pressed = false;
    bool was_step_pressed = false;

    // Execution loop
    while (!glfwWindowShouldClose(window)) {
        auto current_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> dt = current_time - last_frame_time;
        last_frame_time = current_time;

        glfwPollEvents();

        // Pausing (Shift + P)
        bool shift_pressed = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
        bool p_pressed = glfwGetKey(window, GLFW_KEY_P) == GLFW_PRESS;
        bool pause_pressed = p_pressed && shift_pressed;
        if (pause_pressed && !was_pause_pressed) {
            is_paused = !is_paused;
            std::cout << "[Simulator] " << (is_paused ? "Paused" : "Resumed") << "\n";
        }
        was_pause_pressed = pause_pressed;

        // Stepping (O key, when paused)
        bool step_pressed = glfwGetKey(window, GLFW_KEY_O) == GLFW_PRESS;
        if (step_pressed && !was_step_pressed && is_paused) {
            step_requested = true;
            std::cout << "[Simulator] Step: " << total_cycles + 1 << "\n";
        }
        was_step_pressed = step_pressed;

        // Handle hot-reloading
        if (handle_hot_reloading(window, chip_manager, config, exe_dir, was_F5_pressed, screen_texture)) {
            cycle_accumulator = 0.0;

            is_paused = config.start_paused;
            total_cycles = 0;
        }

        // Prep rendering context
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(.1f, .1f, .1f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);

        if (IDynamicChip* active_chip = chip_manager.get_chip()) {
            const HardwareState info = active_chip->get_info();
            const double target_hz = info.target_hz > 0.0 ? info.target_hz : config.clock_speed;
            double actual_hz = target_hz;

            kb_driver.apply_pins(window, active_chip);

            uint64_t cycles_run = 0;
            double sim_time_sec = 0.0;

            if (target_hz > 0.0) {
                cycles_run = run_capped_mode(active_chip, target_hz, dt.count(), is_paused, step_requested, cycle_accumulator, kb_driver, window, sim_time_sec);
            } else {
                cycles_run = run_uncapped_mode(active_chip, is_paused, step_requested, actual_hz, kb_driver, window, current_time, sim_time_sec);
                last_frame_time = std::chrono::high_resolution_clock::now(); // Prevent large dt jump after uncapped sleep
            }

            total_cycles += cycles_run;
            analyzer.record_batch(cycles_run, sim_time_sec);

            render_screen(info, screen_texture, cycles_run > 0);
            analyzer.update_and_report(window, config.target_chip, target_hz, actual_hz);

        } else if (chip_manager.is_busy()) {
            glfwSetWindowTitle(window, ("Loading " + chip_manager.target_name + "...").c_str());
            last_frame_time = std::chrono::high_resolution_clock::now();
        }

        glfwSwapBuffers(window);
    }

    // Cleanup
    glDeleteTextures(1, &screen_texture);
    glfwTerminate();
    return 0;
}
