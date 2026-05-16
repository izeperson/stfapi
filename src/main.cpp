#include <iostream>
#include <vector>
#include <filesystem>
#include <cstdlib>
#include <cstdio>
#include <thread>
#include <atomic>
#include <mutex>
#include <unistd.h>
#include <sys/wait.h>
#include <cstring>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string server_url = "http://10.0.0.4:35061";
std::string default_install_root = "./SteamRIP_Games/";
bool show_settings_window = false;

// --- Global state (before any function that references them) ---
std::string login_error_msg = "";
char username_input[128] = "";
char password_input[128] = "";
bool is_authenticated = false;

// --- Game struct with thread-safe cross-thread fields ---
struct Game {
    int id;
    std::string title;
    std::string filename;
    std::string folder_name;
    std::string detected_exe; // Cache the detected executable path
    char        custom_install_path[512] = "";

    std::atomic<bool>   is_installed      { false };
    std::atomic<bool>   is_downloading    { false };
    std::atomic<float>  download_progress { 0.0f  };

    mutable std::mutex  error_mtx;
    std::string         error_msg;

    void set_error(const std::string& msg) {
        std::lock_guard<std::mutex> lock(error_mtx);
        error_msg = msg;
    }
    std::string get_error() const {
        std::lock_guard<std::mutex> lock(error_mtx);
        return error_msg;
    }

    std::string get_effective_install_path() const {
        if (strlen(custom_install_path) > 0) return std::string(custom_install_path);
        return default_install_root + folder_name;
    }
};

using GamePtr = std::shared_ptr<Game>;
std::vector<GamePtr> game_library;

// Track download threads so we can join them on shutdown
struct DownloadTask {
    std::thread handle;
    GamePtr game;
};
std::vector<DownloadTask> download_tasks;
std::mutex threads_mutex;

// --- Data sink for CURL writes ---
size_t WriteStringCallback(void* contents, size_t size, size_t nmemb, std::string* s) {
    size_t newLength = size * nmemb;
    s->append(static_cast<char*>(contents), newLength);
    return newLength;
}

// --- Fetch the remote game manifest ---
void FetchGameLibrary() {
    CURL* curl = curl_easy_init();
    if (!curl) return;

    std::string response_string;
    curl_easy_setopt(curl, CURLOPT_URL, (server_url + "/games.json").c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteStringCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    if (curl_easy_perform(curl) == CURLE_OK) {
        try {
            auto j = json::parse(response_string);
            std::vector<GamePtr> new_library;

            for (auto& item : j) {
                int id = item["id"];
                
                // Check if game already exists to preserve atomic states and custom paths
                GamePtr g = nullptr;
                for (auto& existing : game_library)
                    if (existing->id == id) { g = existing; break; }

                if (!g) {
                    g = std::make_shared<Game>();
                    g->id = id;
                    g->title = item["title"];
                    g->filename = item["filename"];
                    g->folder_name = g->filename;
                    size_t lastdot = g->folder_name.find_last_of(".");
                    if (lastdot != std::string::npos)
                        g->folder_name = g->folder_name.substr(0, lastdot);
                }

                g->is_installed = fs::exists(g->get_effective_install_path());
                new_library.push_back(g);
            }
            game_library = std::move(new_library);
        } catch (json::parse_error& e) {
            std::cerr << "JSON Parsing Exception: " << e.what() << std::endl;
            // Optionally show a global error notification in UI here
        }
    }
    curl_easy_cleanup(curl);
}

// --- Authenticate with the server ---
bool AttemptServerLogin(const std::string& username, const std::string& password) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string response_string;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!headers)
        return false;

    json login_json;
    login_json["username"] = username;
    login_json["password"] = password;
    std::string json_str = login_json.dump();

    curl_easy_setopt(curl, CURLOPT_URL, (server_url + "/api/login").c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteStringCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    long response_code = 0;
    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res == CURLE_OK && response_code == 200) {
        login_error_msg = "";
        return true;
    } else {
        try {
            auto err_json = json::parse(response_string);
            login_error_msg = err_json.value("message", "Login rejected.");
        } catch (...) {
            login_error_msg = "Server unreachable or invalid response.";
        }
        return false;
    }
}

// --- Launch a game using fork/exec (no shell injection) ---
void LaunchGameNative(GamePtr game) {
    std::string game_dir = game->get_effective_install_path();
    std::string target_exe = game->detected_exe; // Try using cached path first

    if (!fs::exists(game_dir)) {
        std::cerr << "[launcher] Game directory not found: " << game_dir << std::endl;
        return;
    }

    if (target_exe.empty()) {
        // Walk the entire extracted tree looking for any viable executable
        // Note: This walk is blocking; if it's too slow, move to a background thread.
        for (const auto& entry : fs::recursive_directory_iterator(game_dir)) {
            if (!entry.is_regular_file()) continue;

            std::string ext = entry.path().extension().string();
            bool is_exe  = (ext == ".exe"  || ext == ".EXE");
            bool is_bin  = (ext == ".x86_64" || ext == ".bin" || ext == ".sh" ||
                            ext == ".x86"  || ext == ".amd64");

            if (!is_exe && !is_bin) continue;

            std::string stem = entry.path().stem().string();
            std::string stem_lower;
            for (char c : stem) stem_lower += static_cast<char>(std::tolower(c));

            if (stem_lower.find("unity") != std::string::npos || 
                stem_lower.find("crash") != std::string::npos ||
                stem_lower.find("uninstall") != std::string::npos) continue;

            std::string parent_name = entry.path().parent_path().filename().string();
            bool matches_folder = (stem_lower == parent_name);

            if (target_exe.empty() || matches_folder) {
                target_exe = fs::absolute(entry.path()).string();
                if (matches_folder) break; 
            }
        }
        game->detected_exe = target_exe; // Cache it for next time
    }

    if (target_exe.empty()) {
        // Last resort: list what we found for debugging
        std::cerr << "[launcher] No executable found in: " << game_dir << std::endl;
        std::cerr << "[launcher] Contents:" << std::endl;
        for (const auto& entry : fs::recursive_directory_iterator(game_dir)) {
            std::cerr << "  " << entry.path().string()
                      << (entry.is_directory() ? "/" : "") << std::endl;
        }
        return;
    }

    std::cout << "[launcher] Launching: " << target_exe << std::endl;

    // Force executable permissions
    try {
        fs::permissions(target_exe,
                        fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                        fs::perm_options::add);
    } catch (const std::exception& e) {
        std::cerr << "Failed setting permissions: " << e.what() << std::endl;
    }

    std::string exe_dir = fs::absolute(fs::path(target_exe).parent_path()).string();
    std::string ext = fs::path(target_exe).extension().string();
    bool is_wine = (ext == ".exe" || ext == ".EXE");

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        chdir(exe_dir.c_str());
        setenv("WINEDEBUG", "-all", 1);  // suppress all Wine debug spam

        if (is_wine) {
            // Build a custom hosts file so Wine's getaddrinfo doesn't fail
            const char* home = getenv("HOME");
            if (!home) home = "/tmp";
            std::string hosts_path = std::string(home) + "/.local/share/stf/hosts";
            fs::create_directories(fs::path(hosts_path).parent_path());

            std::string hn = "localhost";
            if (const char* e = getenv("HOSTNAME")) hn = e;

            {
                std::ofstream hf(hosts_path);
                hf << "127.0.0.1 localhost\n"
                   << "127.0.1.1 " << hn << "\n"
                   << "::1 localhost ip6-localhost ip6-loopback\n";
            }

            // Redirect stdin/stdout/stderr to /dev/null (no terminal window)
            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                if (devnull > STDERR_FILENO) close(devnull);
            }

            // Try unshare (no-root user namespace + bind-mount custom hosts)
            std::string shell_cmd = "mount --bind " + hosts_path
                + " /etc/hosts && exec wine \"" + target_exe + "\"";
            execlp("unshare", "unshare", "--user", "--map-root-user",
                   "--mount", "--mount-proc",
                   "sh", "-c", shell_cmd.c_str(),
                   static_cast<char*>(nullptr));

            // Fallback 1: plain wine (hostname warning is cosmetic, game still runs)
            execlp("wine", "wine", target_exe.c_str(),
                   static_cast<char*>(nullptr));

            // Fallback 2: let kernel binfmt_misc handle the .exe directly
            execl(target_exe.c_str(), target_exe.c_str(),
                  static_cast<char*>(nullptr));
        } else {
            execl(target_exe.c_str(), target_exe.c_str(),
                  static_cast<char*>(nullptr));
        }
        _exit(1);
    } else if (pid < 0) {
        std::cerr << "Fork failed for game launch: " << std::strerror(errno) << std::endl;
    }
}

// --- Download a game archive and extract it (runs in its own thread) ---
void DownloadAndExtract(GamePtr game) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        game->set_error("Failed to initialize CURL.");
        game->is_downloading = false;
        return;
    }

    std::string d_url  = server_url + "/files/" + game->filename;
    std::string install_dir = game->get_effective_install_path();
    std::string s_path = install_dir + ".zip"; 

    fs::create_directories(fs::path(install_dir).parent_path());

    FILE* fp = std::fopen(s_path.c_str(), "wb");
    if (!fp) {
        game->set_error("Failed to create file on disk.");
        curl_easy_cleanup(curl);
        game->is_downloading = false;
        return;
    }

    auto WriteData = [](void* ptr, size_t size, size_t nmemb, FILE* stream) -> size_t {
        return std::fwrite(ptr, size, nmemb, stream);
    };

    auto ProgressCallback = [](void* clientp, curl_off_t dltotal, curl_off_t dlnow,
                               curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) -> int {
        if (dltotal > 0) {
            Game* target_game = static_cast<Game*>(clientp);
            target_game->download_progress = static_cast<float>(
                static_cast<double>(dlnow) / static_cast<double>(dltotal));
        }
        return 0;
    };

    curl_easy_setopt(curl, CURLOPT_URL, d_url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +WriteData);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, +ProgressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, game.get());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode res = curl_easy_perform(curl);
    std::fclose(fp);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        // Clean up partial download
        if (fs::exists(s_path)) fs::remove(s_path);
        game->set_error("Download failed: " + std::string(curl_easy_strerror(res)));
        game->is_downloading = false;
        return;
    }

    // --- Extract the archive using fork/exec (no shell injection) ---
    std::string archive_path = s_path;
    fs::create_directories(install_dir);

    pid_t pid = fork();
    if (pid == 0) {
        // Child: run unzip
        execlp("unzip", "unzip", "-q", "-o",
               archive_path.c_str(), "-d", install_dir.c_str(),
               static_cast<char*>(nullptr));
        _exit(1);
    } else if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            // Success: remove the archive
            if (fs::exists(archive_path)) fs::remove(archive_path);
            game->is_installed = true;
        } else {
            game->set_error("Extraction failed (unzip error).");
        }
    } else {
        game->set_error("Failed to fork for extraction.");
    }

    game->is_downloading = false;
}

// --- Render login window ---
void RenderLoginUI() {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);

    ImGui::Begin("LoginPanel", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    ImVec2 display_size = ImGui::GetIO().DisplaySize;
    float box_width  = 340.0f;
    float box_height = 290.0f;

    ImGui::SetCursorPos(ImVec2((display_size.x - box_width) * 0.5f,
                              (display_size.y - box_height) * 0.5f));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyle().Colors[ImGuiCol_ChildBg]);
    ImGui::BeginChild("LoginBox", ImVec2(box_width, box_height), true,
                      ImGuiWindowFlags_NoScrollbar);

    ImGui::Spacing(); ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.40f, 0.75f, 0.95f, 1.00f), "  SIGN IN TO ACCOUNT");
    ImGui::Separator();
    ImGui::Spacing(); ImGui::Spacing();

    ImGui::Text("Username");
    ImGui::InputText("##user", username_input, IM_COUNTOF(username_input));
    ImGui::Spacing();

    ImGui::Text("Password");
    ImGui::InputText("##pass", password_input, IM_COUNTOF(password_input),
                     ImGuiInputTextFlags_Password);
    ImGui::Spacing(); ImGui::Spacing();

    if (!login_error_msg.empty()) {
        ImGui::TextColored(ImVec4(0.90f, 0.25f, 0.25f, 1.00f), "%s", login_error_msg.c_str());
        ImGui::Spacing();
    }

    if (ImGui::Button("LOGIN", ImVec2(-1, 42))) {
        if (AttemptServerLogin(username_input, password_input)) {
            is_authenticated = true;
            FetchGameLibrary();
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::End();
}

// --- Render settings window ---
void RenderSettingsUI() {
    if (!show_settings_window) return;

    ImGui::SetNextWindowSize(ImVec2(550, 240), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Launcher Settings", &show_settings_window)) {
        ImGui::TextColored(ImVec4(0.40f, 0.75f, 0.95f, 1.00f), "GLOBAL CONFIGURATION");
        ImGui::Separator();
        ImGui::Spacing();

        static char s_url[256];
        static char i_root[256];
        static bool init = false;
        if (!init) {
            strncpy(s_url, server_url.c_str(), sizeof(s_url));
            strncpy(i_root, default_install_root.c_str(), sizeof(i_root));
            init = true;
        }

        if (ImGui::InputText("Server URL", s_url, sizeof(s_url))) {
            server_url = s_url;
        }
        if (ImGui::InputText("Default Install Root", i_root, sizeof(i_root))) {
            default_install_root = i_root;
        }

        ImGui::Spacing();
        if (ImGui::Button("Refresh Library", ImVec2(140, 34))) {
            FetchGameLibrary();
        }
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(100, 34))) { show_settings_window = false; }
    }
    ImGui::End();
}

// --- Render main dashboard ---
void RenderSteamRIPUI() {
    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowPadding = ImVec2(24, 24);
    style.FramePadding  = ImVec2(16, 12);
    style.ItemSpacing   = ImVec2(20, 20);
    style.CellPadding   = ImVec2(16, 16);
    style.WindowRounding = 0.0f;
    style.FrameRounding  = 6.0f;
    style.ChildRounding  = 8.0f;

    ImVec4 DarkBg       = ImVec4(0.08f, 0.09f, 0.13f, 1.00f);
    ImVec4 CardBg       = ImVec4(0.13f, 0.15f, 0.21f, 1.00f);
    ImVec4 AccentBlue   = ImVec4(0.14f, 0.51f, 0.85f, 1.00f);
    ImVec4 HoverBlue    = ImVec4(0.24f, 0.61f, 0.95f, 1.00f);
    ImVec4 AccentGreen  = ImVec4(0.19f, 0.65f, 0.33f, 1.00f);
    ImVec4 HoverGreen   = ImVec4(0.25f, 0.75f, 0.40f, 1.00f);
    ImVec4 TextMuted    = ImVec4(0.55f, 0.60f, 0.68f, 1.00f);
    ImVec4 ErrorRed     = ImVec4(0.90f, 0.25f, 0.25f, 1.00f);

    style.Colors[ImGuiCol_WindowBg]      = DarkBg;
    style.Colors[ImGuiCol_Border]        = ImVec4(0, 0, 0, 0);
    style.Colors[ImGuiCol_Button]        = AccentBlue;
    style.Colors[ImGuiCol_ButtonHovered] = HoverBlue;
    style.Colors[ImGuiCol_ButtonActive]  = ImVec4(0.10f, 0.40f, 0.70f, 1.00f);

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);

    ImGui::Begin("MainViewport", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    ImGui::BeginGroup();
    ImGui::TextColored(ImVec4(0.92f, 0.95f, 0.98f, 1.00f), "MY NETWORK GAMES");
    ImGui::SameLine(ImGui::GetWindowWidth() - 130);
    if (ImGui::Button("SETTINGS", ImVec2(100, 0))) { show_settings_window = true; }
    ImGui::EndGroup();

    ImGui::TextColored(TextMuted, "Family Sharing Dashboard • Local Server Connected");
    ImGui::Spacing(); ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing(); ImGui::Spacing();

    float panelWidth  = ImGui::GetContentRegionAvail().x;
    float cardWidth   = 260.0f;
    int   columnCount = static_cast<int>(panelWidth / (cardWidth + style.ItemSpacing.x));
    if (columnCount < 1) columnCount = 1;

    if (ImGui::BeginTable("ResponsiveGamesGrid", columnCount, ImGuiTableFlags_None)) {
        for (size_t idx = 0; idx < game_library.size(); ++idx) {
            auto& game = game_library[idx];
            ImGui::TableNextColumn();

            ImGui::PushStyleColor(ImGuiCol_ChildBg, CardBg);
            std::string childId = "Card_" + std::to_string(game->id);

            ImGui::BeginChild(childId.c_str(), ImVec2(cardWidth, 230.0f), true,
                              ImGuiWindowFlags_NoScrollbar);

            ImGui::PushFont(NULL, ImGui::GetStyle().FontSizeBase * 1.15f);
            ImGui::TextWrapped("%s", game->title.c_str());
            ImGui::PopFont();

            ImGui::Spacing();
            ImGui::TextDisabled("Install Folder:");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText(("##path" + std::to_string(game->id)).c_str(),
                                 game->custom_install_path, sizeof(game->custom_install_path))) {
                game->is_installed = fs::exists(game->get_effective_install_path());
            }

            ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 66.0f);

            if (game->is_installed) {
                ImGui::PushStyleColor(ImGuiCol_Button, AccentGreen);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, HoverGreen);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                      ImVec4(0.15f, 0.50f, 0.25f, 1.00f));

                if (ImGui::Button(("LAUNCH GAME##" + std::to_string(game->id)).c_str(),
                                   ImVec2(-1, 42))) {
                    LaunchGameNative(game);
                }
                ImGui::PopStyleColor(3);
            }
            else if (game->is_downloading) {
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImVec4(0.18f, 0.20f, 0.27f, 1.00f));
                ImGui::Button(("DOWNLOADING...##" + std::to_string(game->id)).c_str(),
                              ImVec2(-1, 34));
                ImGui::PopStyleColor(1);

                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, AccentBlue);
                ImGui::ProgressBar(game->download_progress.load(), ImVec2(-1, 4), "");
                ImGui::PopStyleColor(1);

                std::string err = game->get_error();
                if (!err.empty()) {
                    ImGui::Spacing();
                    ImGui::TextColored(ErrorRed, "%s", err.c_str());
                }
            }
            else {
                if (ImGui::Button(("GET INSTALL##" + std::to_string(game->id)).c_str(),
                                   ImVec2(-1, 42))) {
                    game->is_downloading = true;
                    game->download_progress = 0.0f;
                    game->set_error("");

                    // Capture shared_ptr by value to keep Game alive for the thread's lifetime
                    std::thread dlThread([game]() {
                        DownloadAndExtract(game);
                    });

                    std::lock_guard<std::mutex> lock(threads_mutex);
                    download_tasks.push_back({std::move(dlThread), game});
                }
            }

            ImGui::EndChild();
            ImGui::PopStyleColor(1);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

// ==========  main  ==========

int main() {
    // Initialise libcurl globally (required before any curl_easy_init call)
    curl_global_init(CURL_GLOBAL_ALL);

    if (!glfwInit()) {
        curl_global_cleanup();
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    GLFWwindow* window = glfwCreateWindow(1024, 640, "SteamRIP Family Network",
                                          nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        curl_global_cleanup();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;

    // Load the first available font
    ImFont* customFont = nullptr;
    std::vector<std::string> fontPaths = {
        "/usr/share/fonts/TTF/Inter-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf"
    };
    for (const auto& path : fontPaths) {
        if (std::filesystem::exists(path)) {
            customFont = io.Fonts->AddFontFromFileTTF(path.c_str(), 17.0f);
            break;
        }
    }

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    // --- Main loop ---
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Non-blocking cleanup of finished download threads
        {
            std::lock_guard<std::mutex> lock(threads_mutex);
            for (auto it = download_tasks.begin(); it != download_tasks.end(); ) {
                if (!it->game->is_downloading) {
                    if (it->handle.joinable()) it->handle.join();
                    it = download_tasks.erase(it);
                } else {
                    ++it;
                }
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (!is_authenticated) {
            RenderLoginUI();
        } else {
            RenderSteamRIPUI();
            RenderSettingsUI();
        }

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.08f, 0.09f, 0.13f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // --- Shutdown: join any still-running download threads ---
    {
        std::lock_guard<std::mutex> lock(threads_mutex);
        for (auto& t : download_tasks) {
            if (t.handle.joinable()) t.handle.join();
        }
        download_tasks.clear();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    curl_global_cleanup();
    return 0;
}