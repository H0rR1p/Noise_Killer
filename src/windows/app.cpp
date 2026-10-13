#include "noisekiller/io.hpp"
#include "noisekiller/windows.hpp"
#ifdef _WIN32
#include <windows.h>

#include <algorithm>
#include <array>
#include <commctrl.h>
#include <commdlg.h>
#include <filesystem>
#include <objbase.h>
#include <sstream>
#include <thread>

namespace {
constexpr UINT Completed = WM_APP + 1;
enum { Input = 101, Refresh, Record, Import, Remove, Filter, Stop, Bypass, Offline, Output };
std::wstring wide(const std::string &value) {
    if (value.empty())
        return {};
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring text(size, L' ');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), text.data(), size);
    return text;
}
std::wstring profile_location() {
    DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (!size)
        return {};
    std::wstring base(size, L'\0');
    const DWORD copied = GetEnvironmentVariableW(L"LOCALAPPDATA", base.data(), size);
    if (!copied || copied >= size)
        return {};
    base.resize(wcslen(base.c_str()));
    // Startup and --smoke only resolve/read the location. Creation belongs to
    // the explicitly requested enrollment operation, not application launch.
    return base + L"\\NoiseKiller\\active-profile.nkp";
}
bool select_file(HWND window, bool save, std::wstring &path) {
    std::array<wchar_t, 32768> name{};
    OPENFILENAMEW request{};
    request.lStructSize = sizeof(request);
    request.hwndOwner = window;
    request.lpstrFilter = L"WAV 音频 (*.wav)\0*.wav\0\0";
    request.lpstrFile = name.data();
    request.nMaxFile = static_cast<DWORD>(name.size());
    request.lpstrDefExt = L"wav";
    request.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&request) : GetOpenFileNameW(&request)))
        return false;
    path = name.data();
    return true;
}
struct App {
    HWND window = nullptr, input = nullptr, output = nullptr, status = nullptr, details = nullptr,
         profile_label = nullptr;
    HFONT font = nullptr;
    nk::win::AudioSession audio;
    std::vector<nk::win::Device> devices;
    std::vector<nk::win::Device> output_devices;
    nk::Profile profile{};
    std::wstring profile_path;
    std::thread job;
    bool busy = false, job_ok = false, job_has_profile = false;
    std::string job_error;
    std::wstring job_message;
    nk::Profile job_profile{};
    ~App() {
        audio.stop();
        if (job.joinable())
            job.join();
        SecureZeroMemory(&profile, sizeof(profile));
        SecureZeroMemory(&job_profile, sizeof(job_profile));
        if (font)
            DeleteObject(font);
    }
    HWND control(const wchar_t *type, const wchar_t *text, DWORD style, int x, int y, int w, int h,
                 int id = 0) {
        HWND result =
            CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, window,
                            reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return result;
    }
    void message(const std::wstring &text) { SetWindowTextW(status, text.c_str()); }
    void error(const std::string &text) { message(L"不可用 / 已静音：" + wide(text)); }
    void show_profile() {
        std::wstring text = L"本人档案：未注册";
        if (profile.valid())
            text = L"本人档案：已注册（" + std::to_wstring(profile.prototype_count) +
                   L" 个声学原型，长期锚点固定）";
        SetWindowTextW(profile_label, text.c_str());
    }
    std::wstring selection() {
        const LRESULT index = SendMessageW(input, CB_GETCURSEL, 0, 0);
        return index >= 0 && static_cast<std::size_t>(index) < devices.size()
                   ? devices[static_cast<std::size_t>(index)].id
                   : L"";
    }
    std::wstring output_selection() {
        const LRESULT index = SendMessageW(output, CB_GETCURSEL, 0, 0);
        return index >= 0 && static_cast<std::size_t>(index) < output_devices.size()
                   ? output_devices[static_cast<std::size_t>(index)].id
                   : L"";
    }
    bool confirm_route() {
        const LRESULT index = SendMessageW(output, CB_GETCURSEL, 0, 0);
        if (index < 0 || static_cast<std::size_t>(index) >= output_devices.size()) {
            error("Select an installed virtual-cable playback endpoint explicitly; no default speaker "
                  "fallback");
            return false;
        }
        const auto text =
            L"将写入线路播放端点：" + output_devices[static_cast<std::size_t>(index)].name +
            L"\n请确认目标应用已选择同一线路的录音端（例如 CABLE "
            L"Output），且此录音端没有启用‘侦听此设备’回路。\n"
            L"设备名/厂商仅作路由提示，不证明配对或安全认证；其他程序写入这条线路也可能混入音频。\n继续吗？";
        return MessageBoxW(window, text.c_str(), L"确认虚拟线路方向", MB_OKCANCEL | MB_ICONINFORMATION) ==
               IDOK;
    }
    void refresh() {
        const auto old = selection();
        const auto old_output = output_selection();
        std::vector<nk::win::Device> all;
        std::string failure;
        SendMessageW(input, CB_RESETCONTENT, 0, 0);
        devices.clear();
        if (!nk::win::enumerate_capture_devices(all, failure)) {
            error(failure);
            return;
        }
        unsigned selected = 0;
        for (auto &d : all) {
            if (d.virtual_microphone) {
                continue;
            }
            if (d.id == old)
                selected = static_cast<unsigned>(devices.size());
            SendMessageW(input, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(d.name.c_str()));
            devices.push_back(std::move(d));
        }
        if (!devices.empty())
            SendMessageW(input, CB_SETCURSEL, selected, 0);
        SendMessageW(output, CB_RESETCONTENT, 0, 0);
        output_devices.clear();
        if (!nk::win::enumerate_render_devices(all, failure)) {
            error(failure);
            return;
        }
        int selected_output = -1;
        for (auto &d : all) {
            if (!d.virtual_cable)
                continue;
            if (d.id == old_output)
                selected_output = static_cast<int>(output_devices.size());
            SendMessageW(output, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(d.name.c_str()));
            output_devices.push_back(std::move(d));
        }
        SendMessageW(output, CB_SETCURSEL, selected_output, 0);
        message(output_devices.empty()
                    ? L"未找到支持的 VB-CABLE "
                      L"播放端点。你可注册/离线实验；实时输出不可用。应用不会自动安装驱动或改默认设备。"
                    : L"请选择线路播放端（CABLE Input）；目标应用选择对应录音端（CABLE "
                      L"Output）。没有自动启用采集。");
    }
    void buttons() {
        const auto s = audio.snapshot().state;
        const bool active = s == nk::win::SessionState::Starting || s == nk::win::SessionState::Enrolling ||
                            s == nk::win::SessionState::Filtering || s == nk::win::SessionState::Bypass;
        for (int id : {Refresh, Record, Import, Remove, Filter, Bypass, Offline})
            EnableWindow(GetDlgItem(window, id), !busy && !active);
        EnableWindow(input, !busy && !active);
        EnableWindow(output, !busy && !active);
        EnableWindow(GetDlgItem(window, Stop), active || s == nk::win::SessionState::Fault);
    }
    void create() {
        font = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        control(L"STATIC", L"NoiseKiller · MECAG 本地声学过滤", 0, 24, 18, 690, 30);
        control(L"STATIC", L"物理麦克风（排除虚拟录音端，避免反馈）：", 0, 24, 60, 520, 26);
        input =
            control(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 24, 90, 558, 250, Input);
        control(L"BUTTON", L"刷新设备", WS_TABSTOP, 596, 89, 112, 30, Refresh);
        control(L"STATIC", L"虚拟线路播放端点（显式选择，不使用默认扬声器）：", 0, 24, 132, 684, 26);
        output =
            control(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 24, 162, 684, 230, Output);
        profile_label = control(L"STATIC", L"本人档案：未注册", 0, 24, 212, 684, 28);
        control(L"BUTTON", L"录制本人 15 秒", WS_TABSTOP, 24, 250, 162, 36, Record);
        control(L"BUTTON", L"导入注册 WAV", WS_TABSTOP, 200, 250, 160, 36, Import);
        control(L"BUTTON", L"删除本人档案", WS_TABSTOP, 374, 250, 160, 36, Remove);
        control(L"BUTTON", L"开启 MECAG", WS_TABSTOP, 24, 302, 162, 40, Filter);
        control(L"BUTTON", L"停止 / 静音", WS_TABSTOP, 200, 302, 160, 40, Stop);
        control(L"BUTTON", L"显式原声旁路", WS_TABSTOP, 374, 302, 160, 40, Bypass);
        control(L"BUTTON", L"离线处理 WAV", WS_TABSTOP, 548, 302, 160, 40, Offline);
        control(L"STATIC",
                L"方向：本程序 → CABLE Input（播放）→ 虚拟线路 → CABLE Output（录音）→ 游戏/通信。\n"
                L"故障不自动转原音；不改系统/应用默认设备，不保证能清除第三方线路内部余音。",
                0, 24, 362, 684, 54);
        status = control(L"STATIC", L"已停止：没有采集音频。", SS_LEFT, 24, 430, 684, 86);
        details = control(L"STATIC", L"", SS_LEFT, 24, 524, 684, 152);
        control(L"STATIC",
                L"默认不保存注册或环境 PCM，不上传音频；只保存当前用户 DPAPI "
                L"加密档案。\n当前为待验证构建：真人质量、线路实际消费者与完整延迟仍需验收。",
                0, 24, 686, 684, 55);
        profile_path = profile_location();
        std::string failure;
        if (!profile_path.empty() && GetFileAttributesW(profile_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (!nk::win::load_profile(profile_path, profile, failure))
                error(failure);
        }
        show_profile();
        refresh();
        buttons();
        SetTimer(window, 1, 200, nullptr);
    }
    void start_job(std::vector<float> samples) {
        if (job.joinable())
            job.join();
        busy = true;
        job_ok = false;
        job_has_profile = true;
        job_error.clear();
        job_message.clear();
        buttons();
        message(L"正在进行注册质量检查与统计建模；未开启实时采集。");
        job = std::thread([this, samples = std::move(samples)]() mutable {
            try {
                job_ok = nk::enroll(samples.data(), samples.size(), job_profile, job_error);
                if (job_ok) {
                    if (profile_path.empty()) {
                        job_ok = false;
                        job_error = "Current-user profile directory is unavailable";
                    } else {
                        const auto directory = std::filesystem::path(profile_path).parent_path();
                        if (!CreateDirectoryW(directory.c_str(), nullptr) &&
                            GetLastError() != ERROR_ALREADY_EXISTS) {
                            job_ok = false;
                            job_error =
                                "Unable to create current-user encrypted profile directory (Windows " +
                                std::to_string(GetLastError()) + ")";
                        } else
                            job_ok = nk::win::save_profile(profile_path, job_profile, job_error);
                    }
                }
                if (job_ok)
                    job_message = L"本人注册完成。原始 PCM 已从可管理内存清除；未自动开启实时过滤。";
            } catch (...) {
                job_ok = false;
                job_error = "Enrollment exception";
            }
            if (!samples.empty())
                SecureZeroMemory(samples.data(), samples.size() * sizeof(float));
            PostMessageW(window, Completed, 0, 0);
        });
    }
    void import_profile() {
        std::wstring path;
        if (!select_file(window, false, path))
            return;
        nk::AudioData data;
        std::string failure;
        if (!nk::read_wav(path, data, failure)) {
            error(failure);
            return;
        }
        const double seconds = data.sample_rate ? double(data.samples.size()) / data.sample_rate : 0;
        if (seconds < 10 || seconds > 30) {
            if (!data.samples.empty())
                SecureZeroMemory(data.samples.data(), data.samples.size() * sizeof(float));
            error("Reference WAV must contain 10 to 30 seconds of your voice only");
            return;
        }
        auto mono = nk::resample(data);
        if (!data.samples.empty())
            SecureZeroMemory(data.samples.data(), data.samples.size() * sizeof(float));
        if (mono.empty()) {
            error("Reference resampling failed");
            return;
        }
        start_job(std::move(mono));
    }
    void offline() {
        if (!profile.valid()) {
            error("Register a compatible profile first");
            return;
        }
        std::wstring from, to;
        if (!select_file(window, false, from) || !select_file(window, true, to))
            return;
        std::error_code path_error;
        if (_wcsicmp(from.c_str(), to.c_str()) == 0 || std::filesystem::equivalent(from, to, path_error)) {
            error("Input and output WAV paths must differ");
            return;
        }
        if (job.joinable())
            job.join();
        busy = true;
        job_ok = false;
        job_has_profile = false;
        job_error.clear();
        buttons();
        message(L"离线文件实验：正在运行 MECAG，结果将保存到你选择的 WAV；没有实时采集。");
        auto anchor = profile;
        job = std::thread([this, from, to, anchor]() mutable {
            try {
                nk::AudioData data;
                if (!nk::read_wav(from, data, job_error)) {
                    PostMessageW(window, Completed, 0, 0);
                    return;
                }
                auto mono = nk::resample(data);
                if (!data.samples.empty())
                    SecureZeroMemory(data.samples.data(), data.samples.size() * sizeof(float));
                if (mono.empty()) {
                    job_error = "Empty or unsupported resampled WAV";
                    PostMessageW(window, Completed, 0, 0);
                    return;
                }
                nk::Engine engine(nk::Config{}, anchor);
                engine.reset(1);
                std::vector<float> output(mono.size() + nk::Window, 0);
                std::array<float, nk::Hop> input{}, hop{};
                nk::FrameResult result{};
                const std::size_t frames =
                    (mono.size() + nk::Hop - 1) / nk::Hop + (nk::Window - nk::Hop) / nk::Hop;
                for (std::size_t f = 0; f < frames; ++f) {
                    input.fill(0);
                    for (std::size_t i = 0; i < nk::Hop && f * nk::Hop + i < mono.size(); ++i)
                        input[i] = mono[f * nk::Hop + i];
                    engine.process_hop(input, hop, result);
                    for (std::size_t i = 0; i < nk::Hop && f * nk::Hop + i < output.size(); ++i)
                        output[f * nk::Hop + i] = result.valid ? hop[i] : 0;
                }
                job_ok = nk::write_wav(to, output.data() + nk::Window - nk::Hop, mono.size(), nk::SampleRate,
                                       job_error);
                SecureZeroMemory(mono.data(), mono.size() * sizeof(float));
                SecureZeroMemory(output.data(), output.size() * sizeof(float));
                input.fill(0);
                hop.fill(0);
                SecureZeroMemory(&anchor, sizeof(anchor));
                if (job_ok)
                    job_message =
                        L"离线处理完成：" + to +
                        L"。已补齐尾部并移除声明的 320 样本算法等待；此结果不代表实时或质量验收通过。";
            } catch (...) {
                job_error = "Offline processing exception";
                job_ok = false;
            }
            PostMessageW(window, Completed, 0, 0);
        });
    }
    void command(int id) {
        if (busy && id != Stop)
            return;
        std::string failure;
        switch (id) {
        case Refresh:
            audio.stop();
            refresh();
            break;
        case Record:
            if (MessageBoxW(window,
                            L"将采集所选物理麦克风 15 "
                            L"秒。请确保只有本人讲话，朗读完整句子并包含轻声和短词。\n原始录音只用于注册，不"
                            L"默认保存。开始吗？",
                            L"本人注册", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
                return;
            if (audio.start_enrollment(selection(), 15, failure))
                message(L"正在采集本人参考语音（15 秒）。请连续正常讲话；停止会取消并清除本次录音。");
            else
                error(failure);
            break;
        case Import:
            audio.stop();
            import_profile();
            break;
        case Remove:
            audio.stop();
            if (MessageBoxW(window, L"删除当前本人档案并撤销内存引用？（不宣称磁盘安全擦除）", L"删除档案",
                            MB_OKCANCEL | MB_ICONWARNING) != IDOK)
                return;
            SecureZeroMemory(&profile, sizeof(profile));
            show_profile();
            if (!profile_path.empty() && nk::win::delete_profile(profile_path, failure))
                message(L"本人档案已删除，处理已停止。");
            else
                error(failure.empty() ? "Profile path unavailable" : failure);
            break;
        case Filter:
            if (!confirm_route())
                return;
            if (audio.start_live(selection(), output_selection(), profile, false, failure))
                message(L"MECAG "
                        L"已启用。只写入所选线路播放端；目标应用须手动选择对应线路录音端。未知或故障不自动旁"
                        L"路。");
            else
                error(failure);
            break;
        case Bypass:
            if (MessageBoxW(
                    window,
                    L"原声旁路不会过滤旁人或环境声，并可能向游戏/通信应用暴露这些声音。确认显式启用？",
                    L"原声旁路", MB_YESNO | MB_ICONWARNING) != IDYES)
                return;
            if (!confirm_route())
                return;
            if (audio.start_live(selection(), output_selection(), profile, true, failure))
                message(L"【原声旁路持续开启】正在发送未过滤输入，可能包含旁人与环境噪声。");
            else
                error(failure);
            break;
        case Stop:
            audio.stop();
            message(L"已停止采集，清空本程序 epoch、OLA、输入/输出队列并 Stop/Reset 自己的 WASAPI "
                    L"流。不保证清空第三方线路内部 FIFO 或其他写入者。");
            break;
        case Offline:
            audio.stop();
            offline();
            break;
        }
        buttons();
    }
    void tick() {
        const auto s = audio.snapshot();
        if (s.state == nk::win::SessionState::EnrollmentReady && !busy) {
            std::vector<float> samples;
            std::string failure;
            if (audio.take_enrollment(samples, failure))
                start_job(std::move(samples));
            else
                error(failure);
        } else if (s.state == nk::win::SessionState::Fault) {
            // Join the producer before clearing packet/reference PCM. Control
            // path only; never wait or clear shared storage in the callback.
            audio.stop();
            error(s.error);
        } else if (s.state == nk::win::SessionState::Enrolling)
            message(L"【正在采集注册音】" + std::to_wstring(s.enrollment_samples / nk::SampleRate) +
                    L" / 15 秒。原始 PCM 仅在内存。点击停止可取消。");
        std::wostringstream text;
        text << L"输入：" << s.input_rate << L" Hz / " << s.input_channels << L" 声道；事件周期 "
             << s.capture_period_frames << L" 帧\n"
             << L"已处理 hop：" << s.processed_hops << L"；队列最高：" << s.queue_high_water
             << L" / 8；丢块：" << s.dropped_packets << L"；不连续：" << s.discontinuities << L"\n"
             << L"完整 worker 最近/最大：" << s.compute_us << L" / " << s.maximum_compute_us << L" µs；引擎："
             << s.engine_compute_us << L" µs；超 10 ms：" << s.deadline_misses << L"\n"
             << L"线路：" << s.output.sample_rate << L" Hz / " << s.output.channels << L" 声道；周期/填充："
             << s.output.period_frames << L" / " << s.output.padding_frames << L" 帧\n"
             << L"播放模式：" << (s.output.client3 ? L"IAudioClient3" : L"共享事件 fallback")
             << L"；fallback HRESULT：" << s.output.shared_fallback_hresult << L"\n"
             << L"输出 FIFO 当前/最高：" << s.output.queued_input_samples << L" / "
             << s.output.queue_high_water << L" 样本；欠载：" << s.output.underflow_events
             << L" 次；时钟补偿：" << s.output.drift_ppm << L" ppm\n"
             << L"目标置信：" << s.target_confidence << L"；未知残差：" << s.residual_ratio
             << L"（诊断不是已验收身份概率）";
        SetWindowTextW(details, text.str().c_str());
        buttons();
    }
    void completed() {
        if (job.joinable())
            job.join();
        busy = false;
        if (job_ok) {
            if (job_has_profile) {
                SecureZeroMemory(&profile, sizeof(profile));
                profile = job_profile;
                SecureZeroMemory(&job_profile, sizeof(job_profile));
                show_profile();
            }
            message(job_message);
        } else {
            SecureZeroMemory(&job_profile, sizeof(job_profile));
            error(job_error);
        }
        buttons();
    }
};
LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
    auto *app = reinterpret_cast<App *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        app->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app)
        return DefWindowProcW(window, message, w, l);
    switch (message) {
    case WM_CREATE:
        app->create();
        return 0;
    case WM_COMMAND:
        if (HIWORD(w) == BN_CLICKED)
            app->command(LOWORD(w));
        return 0;
    case WM_TIMER:
        app->tick();
        return 0;
    case Completed:
        app->completed();
        return 0;
    case WM_POWERBROADCAST:
        if (w == PBT_APMSUSPEND) {
            app->audio.stop();
            app->message(L"系统休眠：已停止并清空会话。唤醒后须重新选择设备并显式开启。");
            app->buttons();
        }
        return TRUE;
    case WM_CLOSE:
        KillTimer(window, 1);
        app->audio.stop();
        if (app->job.joinable())
            app->job.join();
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, w, l);
    }
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR arguments, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSEXW type{sizeof(type)};
    type.lpfnWndProc = procedure;
    type.hInstance = instance;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    type.lpszClassName = L"NoiseKillerMecagWindow";
    RegisterClassExW(&type);
    App app;
    HWND window = CreateWindowExW(0, type.lpszClassName, L"NoiseKiller · MECAG（待验证构建）",
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT,
                                  CW_USEDEFAULT, 752, 800, nullptr, nullptr, instance, &app);
    if (!window) {
        CoUninitialize();
        return 1;
    }
    const bool smoke = arguments && wcscmp(arguments, L"--smoke") == 0;
    if (smoke) {
        const auto snapshot = app.audio.snapshot();
        const bool stopped = snapshot.state == nk::win::SessionState::Stopped &&
                             snapshot.captured_packets == 0 && snapshot.processed_hops == 0 && app.input &&
                             app.output && app.status && app.details && app.profile_label;
        SendMessageW(window, WM_CLOSE, 0, 0);
        CoUninitialize();
        return stopped ? 0 : 2;
    }
    ShowWindow(window, show);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
#endif
