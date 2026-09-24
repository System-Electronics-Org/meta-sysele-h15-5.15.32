// SPDX-License-Identifier: MIT

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <cxxopts/cxxopts.hpp>

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "media_library/signal_utils.hpp"

static constexpr const char *DEFAULT_MEDIALIB_CONFIG = "/tmp/dsi_ai_isp/medialib_config.json";
static constexpr const char *RUNTIME_MEDIALIB_CONFIG = "/tmp/dsi_ai_isp/runtime_config.json";
static constexpr const char *DEFAULT_OUTPUT = "/tmp/astrial-isp.png";
static constexpr const char *STREAM_ID = "sink0";
static constexpr const char *STANDARD_PROFILE = "STANDARD_ISP";
static constexpr const char *AI_PROFILE = "AI_ISP";
static constexpr int DISPLAY_WIDTH = 1280;
static constexpr int DISPLAY_HEIGHT = 800;
static constexpr int DISPLAY_FPS = 30;

struct AppConfig
{
    int timeout;
    bool show_fps;
    int initial_profile;
    std::string config_path;
    std::string output_path;
};

struct CaptureState
{
    enum Profile
    {
        STANDARD = 0,
        AI = 1,
    };

    enum Feedback
    {
        IDLE = 0,
        SAVING = 1,
        SAVED = 2,
        FAILED = -1,
    };

    static constexpr int X0 = 1137;
    static constexpr int Y0 = 34;
    static constexpr int X1 = 1245;
    static constexpr int Y1 = 78;
    static constexpr int TIMER_Y0 = 34;
    static constexpr int TIMER_Y1 = 78;
    static constexpr int TIMER_X0 = 35;
    static constexpr int TIMER_WIDTH = 60;
    static constexpr int TIMER_GAP = 8;
    static constexpr int PROFILE_Y0 = 34;
    static constexpr int PROFILE_Y1 = 78;
    static constexpr int PROFILE_WIDTH = 156;
    static constexpr int PROFILE_GAP = 8;
    static constexpr int PROFILE_X0 = (DISPLAY_WIDTH - (2 * PROFILE_WIDTH + PROFILE_GAP)) / 2;

    std::atomic<int> timer_seconds{3};
    std::atomic<bool> countdown_active{false};
    std::atomic<long long> deadline_ns{0};
    std::atomic<int> feedback{IDLE};
    std::atomic<int> feedback_frames{0};
    std::atomic<int> flash_frames{0};
    std::atomic<int> current_profile{STANDARD};
    std::atomic<int> requested_profile{-1};
    std::atomic<bool> switching_profile{false};

    static long long now_ns()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    void handle_touch(int x, int y)
    {
        if (y >= PROFILE_Y0 && y <= PROFILE_Y1)
        {
            for (int profile = STANDARD; profile <= AI; ++profile)
            {
                const int x0 = PROFILE_X0 + profile * (PROFILE_WIDTH + PROFILE_GAP);
                if (x >= x0 && x <= x0 + PROFILE_WIDTH)
                {
                    if (!switching_profile.load() && profile != current_profile.load())
                        requested_profile = profile;
                    return;
                }
            }
        }
        if (y >= TIMER_Y0 && y <= TIMER_Y1)
        {
            constexpr int values[] = {3, 5, 10};
            for (int index = 0; index < 3; ++index)
            {
                const int x0 = TIMER_X0 + index * (TIMER_WIDTH + TIMER_GAP);
                if (x >= x0 && x <= x0 + TIMER_WIDTH)
                {
                    timer_seconds = values[index];
                    return;
                }
            }
        }
        if (x >= X0 && x <= X1 && y >= Y0 && y <= Y1)
        {
            deadline_ns = now_ns() + static_cast<long long>(timer_seconds.load()) * 1000000000LL;
            countdown_active.store(true, std::memory_order_release);
            feedback = IDLE;
            feedback_frames = 0;
        }
    }

    bool capture_due(long long now)
    {
        if (!countdown_active.load(std::memory_order_acquire) || now < deadline_ns.load())
            return false;
        return countdown_active.exchange(false);
    }
};

class PhotoSaver
{
  public:
    PhotoSaver(std::string output_path, CaptureState &state)
        : m_output_path(std::move(output_path)), m_state(state)
    {
    }

    ~PhotoSaver() { stop(); }

    void start() { m_thread = std::thread(&PhotoSaver::run, this); }

    void submit(const cv::Mat &frame, int profile)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pending = frame.clone();
            m_pending_profile = profile;
            m_has_pending = true;
        }
        m_cv.notify_one();
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_quit = true;
        }
        m_cv.notify_one();
        if (m_thread.joinable())
            m_thread.join();
    }

  private:
    void run()
    {
        while (true)
        {
            cv::Mat frame;
            int profile = CaptureState::STANDARD;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] { return m_quit || m_has_pending; });
                if (m_quit && !m_has_pending)
                    return;
                frame = std::move(m_pending);
                profile = m_pending_profile;
                m_has_pending = false;
            }

            const size_t extension = m_output_path.rfind(".png");
            const std::string suffix = profile == CaptureState::AI ? "-ai" : "-standard";
            const std::string output = extension == std::string::npos
                                           ? m_output_path + suffix + ".png"
                                           : m_output_path.substr(0, extension) + suffix + m_output_path.substr(extension);
            const std::string temporary = "/tmp/.astrial-isp-new.png";
            const std::vector<int> parameters{cv::IMWRITE_PNG_COMPRESSION, 3};
            bool saved = false;
            try
            {
                saved = cv::imwrite(temporary, frame, parameters) &&
                        std::rename(temporary.c_str(), output.c_str()) == 0;
            }
            catch (const cv::Exception &error)
            {
                std::cerr << "[photo] " << error.what() << std::endl;
            }

            m_state.feedback = saved ? CaptureState::SAVED : CaptureState::FAILED;
            m_state.feedback_frames = DISPLAY_FPS * 2;
            if (saved)
                std::cout << "[photo] saved " << output << std::endl;
            else
                std::cerr << "[photo] failed to save " << output << std::endl;
        }
    }

    std::string m_output_path;
    CaptureState &m_state;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    cv::Mat m_pending;
    int m_pending_profile = CaptureState::STANDARD;
    bool m_has_pending = false;
    bool m_quit = false;
    std::thread m_thread;
};

class TouchPhotoReader
{
  public:
    ~TouchPhotoReader()
    {
        stop();
        if (m_fd >= 0)
            ::close(m_fd);
    }

    bool open(CaptureState &state)
    {
        m_state = &state;
        m_path = find_device();
        if (m_path.empty())
        {
            std::cerr << "no touch device found under /dev/input" << std::endl;
            return false;
        }
        m_fd = ::open(m_path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (m_fd < 0)
        {
            std::cerr << "cannot open " << m_path << ": " << std::strerror(errno) << std::endl;
            return false;
        }

        char name[128] = "unknown";
        ::ioctl(m_fd, EVIOCGNAME(sizeof(name)), name);
        m_name = name;

        unsigned long abs_bits[(ABS_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {};
        ::ioctl(m_fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits);
        m_multitouch = test_bit(ABS_MT_POSITION_X, abs_bits) && test_bit(ABS_MT_POSITION_Y, abs_bits);
        const int code_x = m_multitouch ? ABS_MT_POSITION_X : ABS_X;
        const int code_y = m_multitouch ? ABS_MT_POSITION_Y : ABS_Y;
        if (::ioctl(m_fd, EVIOCGABS(code_x), &m_abs_x) < 0 ||
            ::ioctl(m_fd, EVIOCGABS(code_y), &m_abs_y) < 0)
        {
            std::cerr << "cannot read the axis ranges of " << m_path << std::endl;
            return false;
        }
        return true;
    }

    void start() { m_thread = std::thread(&TouchPhotoReader::run, this); }

    void stop()
    {
        m_quit = true;
        if (m_thread.joinable())
            m_thread.join();
    }

    const std::string &name() const { return m_name; }
    const std::string &path() const { return m_path; }

  private:
    struct Slot
    {
        bool active = false;
        bool fresh = false;
        int x = 0;
        int y = 0;
    };

    static bool test_bit(int bit, const unsigned long *bits)
    {
        const int width = 8 * sizeof(long);
        return (bits[bit / width] >> (bit % width)) & 1UL;
    }

    static std::string find_device()
    {
        std::vector<int> numbers;
        if (DIR *dir = ::opendir("/dev/input"))
        {
            while (dirent *entry = ::readdir(dir))
            {
                int number;
                if (std::sscanf(entry->d_name, "event%d", &number) == 1)
                    numbers.push_back(number);
            }
            ::closedir(dir);
        }
        std::sort(numbers.begin(), numbers.end());
        for (int number : numbers)
        {
            const std::string path = "/dev/input/event" + std::to_string(number);
            const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0)
                continue;
            unsigned long abs_bits[(ABS_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {};
            ::ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits);
            ::close(fd);
            if (test_bit(ABS_MT_POSITION_X, abs_bits) || test_bit(ABS_X, abs_bits))
                return path;
        }
        return "";
    }

    int scale(int value, const input_absinfo &axis, int size) const
    {
        const int span = std::max(1, axis.maximum - axis.minimum);
        return std::clamp(static_cast<int>(std::lround(double(value - axis.minimum) * (size - 1) / span)),
                          0, size - 1);
    }

    void handle(const input_event &event)
    {
        Slot &slot = m_slots[m_slot];
        if (event.type == EV_ABS)
        {
            if (m_multitouch)
            {
                if (event.code == ABS_MT_SLOT)
                    m_slot = std::clamp(event.value, 0, MAX_SLOTS - 1);
                else if (event.code == ABS_MT_TRACKING_ID)
                {
                    slot.active = event.value >= 0;
                    slot.fresh = slot.active;
                }
                else if (event.code == ABS_MT_POSITION_X)
                    slot.x = event.value;
                else if (event.code == ABS_MT_POSITION_Y)
                    slot.y = event.value;
            }
            else if (event.code == ABS_X)
                m_slots[0].x = event.value;
            else if (event.code == ABS_Y)
                m_slots[0].y = event.value;
        }
        else if (event.type == EV_KEY && !m_multitouch && event.code == BTN_TOUCH)
        {
            m_slots[0].active = event.value != 0;
            m_slots[0].fresh = m_slots[0].active;
        }
        else if (event.type == EV_SYN && event.code == SYN_REPORT)
        {
            for (Slot &current : m_slots)
            {
                if (!current.active || !current.fresh)
                    continue;
                const int x = scale(current.x, m_abs_x, DISPLAY_WIDTH);
                const int y = scale(current.y, m_abs_y, DISPLAY_HEIGHT);
                m_state->handle_touch(x, y);
                current.fresh = false;
            }
        }
    }

    void run()
    {
        input_event events[64];
        while (!m_quit)
        {
            pollfd pfd{m_fd, POLLIN, 0};
            if (::poll(&pfd, 1, 100) <= 0)
                continue;
            const ssize_t count = ::read(m_fd, events, sizeof(events));
            if (count < 0)
            {
                if (errno == EAGAIN || errno == EINTR)
                    continue;
                std::cerr << "reading " << m_path << " failed: " << std::strerror(errno) << std::endl;
                return;
            }
            for (ssize_t i = 0; i < count / static_cast<ssize_t>(sizeof(input_event)); ++i)
                handle(events[i]);
        }
    }

    static constexpr int MAX_SLOTS = 10;
    CaptureState *m_state = nullptr;
    int m_fd = -1;
    std::string m_path;
    std::string m_name;
    bool m_multitouch = false;
    input_absinfo m_abs_x{};
    input_absinfo m_abs_y{};
    Slot m_slots[MAX_SLOTS];
    int m_slot = 0;
    std::thread m_thread;
    std::atomic<bool> m_quit{false};
};

struct InputPipeline
{
    GstElement *pipeline = nullptr;
    GstElement *vision = nullptr;
    GstElement *appsink = nullptr;
};

struct OutputPipeline
{
    GstElement *pipeline = nullptr;
    GstElement *appsrc = nullptr;
    GstElement *fps_text = nullptr;
    std::chrono::steady_clock::time_point last_fps_log{};
};

struct BridgeContext
{
    GstElement *appsrc = nullptr;
};

struct PhotoProbeData
{
    CaptureState &state;
    PhotoSaver &saver;
};

static std::mutex g_stop_mutex;
static std::condition_variable g_stop_cv;
static bool g_stop_requested = false;
static std::atomic<bool> g_gstreamer_error{false};
static std::atomic<bool> g_reconfiguring{false};

static void request_stop()
{
    {
        std::lock_guard<std::mutex> lock(g_stop_mutex);
        g_stop_requested = true;
    }
    g_stop_cv.notify_all();
}

static GstBusSyncReply bus_sync_callback(GstBus *, GstMessage *message, gpointer)
{
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR)
    {
        GError *error = nullptr;
        gchar *debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        std::cerr << "GStreamer error from " << GST_OBJECT_NAME(message->src) << ": "
                  << (error ? error->message : "unknown") << std::endl;
        if (debug)
            std::cerr << "  Debug: " << debug << std::endl;
        g_clear_error(&error);
        g_free(debug);
        if (!g_reconfiguring.load())
        {
            g_gstreamer_error = true;
            request_stop();
        }
    }
    else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS)
    {
        request_stop();
    }
    return GST_BUS_PASS;
}

static void add_bus_handler(GstElement *gst_pipeline)
{
    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(gst_pipeline));
    gst_bus_set_sync_handler(bus, bus_sync_callback, nullptr, nullptr);
    gst_object_unref(bus);
}

static void on_fps_measurement(GstElement *, gdouble fps, gdouble drop_rate, gdouble average_fps,
                               gpointer user_data)
{
    auto *output = static_cast<OutputPipeline *>(user_data);
    if (output && output->fps_text)
    {
        gchar *text = g_strdup_printf("FPS %.1f", fps);
        g_object_set(output->fps_text, "text", text, NULL);
        g_free(text);
    }
    const auto now = std::chrono::steady_clock::now();
    if (!output || output->last_fps_log.time_since_epoch().count() == 0 ||
        now - output->last_fps_log >= std::chrono::seconds(10))
    {
        std::cout << "[display] fps=" << fps << " average=" << average_fps
                  << " dropped/s=" << drop_rate << std::endl;
        if (output)
            output->last_fps_log = now;
    }
}

static GstPadProbeReturn draw_button_and_capture(GstPad *, GstPadProbeInfo *info, gpointer user_data)
{
    auto &probe = *static_cast<PhotoProbeData *>(user_data);
    GstBuffer *buffer = gst_pad_probe_info_get_buffer(info);
    if (!buffer)
        return GST_PAD_PROBE_OK;
    buffer = gst_buffer_make_writable(buffer);
    GST_PAD_PROBE_INFO_DATA(info) = buffer;

    GstMapInfo map{};
    if (!gst_buffer_map(buffer, &map, GST_MAP_READWRITE))
        return GST_PAD_PROBE_OK;
    GstVideoMeta *meta = gst_buffer_get_video_meta(buffer);
    const size_t stride = meta && meta->stride[0] > 0 ? static_cast<size_t>(meta->stride[0])
                                                       : static_cast<size_t>(DISPLAY_WIDTH) * 3;
    if (map.size >= stride * DISPLAY_HEIGHT)
    {
        cv::Mat frame(DISPLAY_HEIGHT, DISPLAY_WIDTH, CV_8UC3, map.data, stride);
        const long long now = CaptureState::now_ns();
        if (probe.state.capture_due(now))
        {
            probe.saver.submit(frame, probe.state.current_profile.load());
            probe.state.feedback = CaptureState::SAVING;
            probe.state.feedback_frames = DISPLAY_FPS * 2;
            probe.state.flash_frames = 6;
        }

        const int selected_timer = probe.state.timer_seconds.load();
        constexpr int timer_values[] = {3, 5, 10};
        for (int index = 0; index < 3; ++index)
        {
            const int x0 = CaptureState::TIMER_X0 + index * (CaptureState::TIMER_WIDTH + CaptureState::TIMER_GAP);
            const bool selected = selected_timer == timer_values[index];
            const cv::Scalar fill = selected ? cv::Scalar(45, 145, 55) : cv::Scalar(30, 32, 34);
            cv::rectangle(frame, {x0, CaptureState::TIMER_Y0},
                          {x0 + CaptureState::TIMER_WIDTH, CaptureState::TIMER_Y1}, fill,
                          cv::FILLED, cv::LINE_AA);
            cv::rectangle(frame, {x0, CaptureState::TIMER_Y0},
                          {x0 + CaptureState::TIMER_WIDTH, CaptureState::TIMER_Y1},
                          cv::Scalar(255, 255, 255), selected ? 2 : 1, cv::LINE_AA);
            const std::string label = std::to_string(timer_values[index]) + "s";
            int timer_baseline = 0;
            const cv::Size timer_text = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX, 0.55, 1,
                                                        &timer_baseline);
            cv::putText(frame, label,
                        {x0 + (CaptureState::TIMER_WIDTH - timer_text.width) / 2,
                         CaptureState::TIMER_Y0 + (CaptureState::TIMER_Y1 - CaptureState::TIMER_Y0 +
                                                   timer_text.height) /
                                                      2},
                        cv::FONT_HERSHEY_SIMPLEX, 0.55, cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
        }

        const int current_profile = probe.state.current_profile.load();
        const int requested_profile = probe.state.requested_profile.load();
        const bool switching_profile = probe.state.switching_profile.load();
        constexpr const char *profile_labels[] = {"STANDARD ISP", "AI-ISP"};
        for (int profile = CaptureState::STANDARD; profile <= CaptureState::AI; ++profile)
        {
            const int x0 = CaptureState::PROFILE_X0 +
                           profile * (CaptureState::PROFILE_WIDTH + CaptureState::PROFILE_GAP);
            const bool selected = profile == current_profile;
            const bool pending = switching_profile && profile == requested_profile;
            const cv::Scalar fill = pending ? cv::Scalar(45, 105, 175)
                                            : selected ? cv::Scalar(45, 145, 55) : cv::Scalar(30, 32, 34);
            cv::rectangle(frame, {x0, CaptureState::PROFILE_Y0},
                          {x0 + CaptureState::PROFILE_WIDTH, CaptureState::PROFILE_Y1}, fill,
                          cv::FILLED, cv::LINE_AA);
            cv::rectangle(frame, {x0, CaptureState::PROFILE_Y0},
                          {x0 + CaptureState::PROFILE_WIDTH, CaptureState::PROFILE_Y1},
                          cv::Scalar(255, 255, 255), selected ? 2 : 1, cv::LINE_AA);
            int profile_baseline = 0;
            const cv::Size profile_text = cv::getTextSize(profile_labels[profile], cv::FONT_HERSHEY_SIMPLEX,
                                                          0.52, 1, &profile_baseline);
            cv::putText(frame, profile_labels[profile],
                        {x0 + (CaptureState::PROFILE_WIDTH - profile_text.width) / 2,
                         CaptureState::PROFILE_Y0 +
                             (CaptureState::PROFILE_Y1 - CaptureState::PROFILE_Y0 + profile_text.height) / 2},
                        cv::FONT_HERSHEY_SIMPLEX, 0.52, cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
        }

        if (probe.state.countdown_active.load(std::memory_order_acquire))
        {
            const long long remaining_ns = std::max(0LL, probe.state.deadline_ns.load() - now);
            const int remaining = std::max(1, static_cast<int>((remaining_ns + 999999999LL) / 1000000000LL));
            const cv::Point center(DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2);
            cv::circle(frame, center, 80, cv::Scalar(25, 27, 29), cv::FILLED, cv::LINE_AA);
            cv::circle(frame, center, 80, cv::Scalar(255, 255, 255), 3, cv::LINE_AA);
            const std::string countdown = std::to_string(remaining);
            int countdown_baseline = 0;
            const cv::Size countdown_text = cv::getTextSize(countdown, cv::FONT_HERSHEY_SIMPLEX, 2.7, 6,
                                                            &countdown_baseline);
            cv::putText(frame, countdown,
                        {center.x - countdown_text.width / 2, center.y + countdown_text.height / 2},
                        cv::FONT_HERSHEY_SIMPLEX, 2.7, cv::Scalar(255, 255, 255), 6, cv::LINE_AA);
        }

        int feedback = probe.state.feedback.load();
        int remaining = probe.state.feedback_frames.load();
        if (remaining > 0)
            probe.state.feedback_frames.fetch_sub(1);
        else
        {
            feedback = CaptureState::IDLE;
            probe.state.feedback = feedback;
        }

        cv::Scalar fill(30, 32, 34);
        const char *label = "PHOTO";
        if (feedback == CaptureState::SAVING)
        {
            fill = cv::Scalar(70, 95, 35);
            label = "SAVING";
        }
        else if (feedback == CaptureState::SAVED)
        {
            fill = cv::Scalar(45, 145, 55);
            label = "SAVED";
        }
        else if (feedback == CaptureState::FAILED)
        {
            fill = cv::Scalar(45, 45, 190);
            label = "ERROR";
        }

        cv::rectangle(frame, {CaptureState::X0, CaptureState::Y0},
                      {CaptureState::X1, CaptureState::Y1}, fill, cv::FILLED, cv::LINE_AA);
        cv::rectangle(frame, {CaptureState::X0, CaptureState::Y0},
                      {CaptureState::X1, CaptureState::Y1}, cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
        int baseline = 0;
        const cv::Size text = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX, 0.55, 1, &baseline);
        const int text_x = CaptureState::X0 + (CaptureState::X1 - CaptureState::X0 - text.width) / 2;
        const int text_y = CaptureState::Y0 + (CaptureState::Y1 - CaptureState::Y0 + text.height) / 2;
        cv::putText(frame, label, {text_x, text_y}, cv::FONT_HERSHEY_SIMPLEX, 0.55,
                    cv::Scalar(255, 255, 255), 1, cv::LINE_AA);

        const int flash = probe.state.flash_frames.load();
        if (flash > 0)
        {
            probe.state.flash_frames.fetch_sub(1);
            const double alpha = 0.18 + 0.08 * flash;
            cv::Mat white(frame.size(), frame.type(), cv::Scalar(255, 255, 255));
            cv::addWeighted(frame, 1.0 - alpha, white, alpha, 0.0, frame);
        }
    }
    gst_buffer_unmap(buffer, &map);
    return GST_PAD_PROBE_OK;
}

static bool parse_arguments(int argc, char **argv, AppConfig &config, bool &help_requested)
{
    cxxopts::Options options("dsi_ai_isp_app",
                             "Astrial H15 touch comparison between the standard ISP and AI-ISP.");
    options.add_options()
        ("h,help", "Show this help")
        ("t,duration", "Seconds to run, or inf", cxxopts::value<std::string>()->default_value("inf"))
        ("show-fps", "Show a small FPS counter on DSI",
         cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
        ("profile", "Initial profile: standard or ai",
         cxxopts::value<std::string>()->default_value("standard"))
        ("o,output", "PNG base path; -standard or -ai is added before .png",
         cxxopts::value<std::string>()->default_value(DEFAULT_OUTPUT))
        ("c,config-file-path", "gsthailovision Media Library configuration",
         cxxopts::value<std::string>()->default_value(DEFAULT_MEDIALIB_CONFIG));

    auto result = options.parse(argc, argv);
    if (result.count("help"))
    {
        std::cout << options.help({""}) << std::endl;
        help_requested = true;
        return false;
    }
    if (!result.unmatched().empty())
    {
        for (const auto &argument : result.unmatched())
            std::cerr << "Unrecognized argument: " << argument << std::endl;
        return false;
    }

    const std::string duration = result["duration"].as<std::string>();
    if (duration == "inf" || duration == "n")
        config.timeout = std::numeric_limits<int>::max();
    else
    {
        try
        {
            config.timeout = std::stoi(duration);
        }
        catch (const std::exception &)
        {
            std::cerr << "duration must be a number of seconds, or inf" << std::endl;
            return false;
        }
    }
    config.show_fps = result["show-fps"].as<bool>();
    const std::string profile = result["profile"].as<std::string>();
    if (profile == "standard")
        config.initial_profile = CaptureState::STANDARD;
    else if (profile == "ai")
        config.initial_profile = CaptureState::AI;
    else
    {
        std::cerr << "profile must be standard or ai" << std::endl;
        return false;
    }
    config.output_path = result["output"].as<std::string>();
    config.config_path = result["config-file-path"].as<std::string>();
    if (config.timeout <= 0 || config.output_path.empty())
        return false;
    std::ifstream config_file(config.config_path);
    if (!config_file.good())
    {
        std::cerr << config.config_path << " is missing" << std::endl;
        return false;
    }
    return true;
}

static bool wait_for_state(GstElement *pipeline, GstState state, GstClockTime timeout)
{
    if (gst_element_set_state(pipeline, state) == GST_STATE_CHANGE_FAILURE)
        return false;
    const GstStateChangeReturn result = gst_element_get_state(pipeline, nullptr, nullptr, timeout);
    return result == GST_STATE_CHANGE_SUCCESS || result == GST_STATE_CHANGE_NO_PREROLL;
}

static bool write_runtime_config(const std::string &source_path, int profile)
{
    std::ifstream input(source_path);
    if (!input.good())
        return false;
    std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const std::string key = "\"default_profile\"";
    const size_t key_position = contents.find(key);
    const size_t colon = key_position == std::string::npos ? std::string::npos : contents.find(':', key_position);
    const size_t value_start = colon == std::string::npos ? std::string::npos : contents.find('"', colon);
    const size_t value_end = value_start == std::string::npos ? std::string::npos : contents.find('"', value_start + 1);
    if (value_end == std::string::npos)
    {
        std::cerr << "Cannot find default_profile in " << source_path << std::endl;
        return false;
    }
    const char *profile_name = profile == CaptureState::AI ? AI_PROFILE : STANDARD_PROFILE;
    contents.replace(value_start + 1, value_end - value_start - 1, profile_name);

    std::ofstream output(RUNTIME_MEDIALIB_CONFIG, std::ios::trunc);
    output << contents;
    return output.good();
}

static GstFlowReturn forward_sample(GstAppSink *sink, gpointer user_data)
{
    auto *bridge = static_cast<BridgeContext *>(user_data);
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample || !bridge || !bridge->appsrc)
    {
        if (sample)
            gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    GstBuffer *source = gst_sample_get_buffer(sample);
    GstBuffer *copy = source ? gst_buffer_copy_deep(source) : nullptr;
    if (!copy)
    {
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }
    GST_BUFFER_PTS(copy) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DTS(copy) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DURATION(copy) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_OFFSET(copy) = GST_BUFFER_OFFSET_NONE;
    GST_BUFFER_OFFSET_END(copy) = GST_BUFFER_OFFSET_NONE;
    GST_BUFFER_FLAGS(copy) = 0;
    const GstFlowReturn result = gst_app_src_push_buffer(GST_APP_SRC(bridge->appsrc), copy);
    gst_sample_unref(sample);
    return result;
}

static bool build_output_pipeline(const AppConfig &config, OutputPipeline &output, PhotoProbeData &probe)
{
    output.pipeline = gst_pipeline_new("astrial-ai-isp-display");
    output.appsrc = gst_element_factory_make("appsrc", "display_source");
    GstElement *queue = gst_element_factory_make("queue", "display_queue");
    GstElement *convert = gst_element_factory_make("videoconvert", "display_convert");
    GstElement *capsfilter = gst_element_factory_make("capsfilter", "display_caps");
    output.fps_text = config.show_fps ? gst_element_factory_make("textoverlay", "fps_text") : nullptr;
    GstElement *fpsdisplay = gst_element_factory_make("fpsdisplaysink", "display_fps");
    GstElement *kmssink = gst_element_factory_make("kmssink", "dsi_sink");
    if (!output.pipeline || !output.appsrc || !queue || !convert || !capsfilter || !fpsdisplay || !kmssink ||
        (config.show_fps && !output.fps_text))
        return false;

    GstCaps *source_caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "NV12",
                                              "width", G_TYPE_INT, DISPLAY_WIDTH,
                                              "height", G_TYPE_INT, DISPLAY_HEIGHT,
                                              "framerate", GST_TYPE_FRACTION, DISPLAY_FPS, 1,
                                              "pixel-aspect-ratio", GST_TYPE_FRACTION, 11, 10, NULL);
    g_object_set(output.appsrc, "is-live", TRUE, "format", GST_FORMAT_TIME, "do-timestamp", TRUE,
                 "caps", source_caps, NULL);
    gst_caps_unref(source_caps);
    g_object_set(queue, "leaky", 2, "max-size-buffers", 1, "max-size-bytes", 0,
                 "max-size-time", static_cast<guint64>(0), NULL);
    g_object_set(convert, "n-threads", 4, NULL);
    GstCaps *display_caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGR",
                                               "width", G_TYPE_INT, DISPLAY_WIDTH,
                                               "height", G_TYPE_INT, DISPLAY_HEIGHT,
                                               "framerate", GST_TYPE_FRACTION, DISPLAY_FPS, 1, NULL);
    g_object_set(capsfilter, "caps", display_caps, NULL);
    gst_caps_unref(display_caps);

    if (output.fps_text)
    {
        g_object_set(output.fps_text, "text", "FPS --", "font-desc", "Sans 12", "auto-resize", FALSE,
                     "draw-shadow", TRUE, "draw-outline", TRUE, "shaded-background", TRUE,
                     "wait-text", FALSE, "xpad", 8, "ypad", 8, NULL);
        gst_util_set_object_arg(G_OBJECT(output.fps_text), "halignment", "left");
        gst_util_set_object_arg(G_OBJECT(output.fps_text), "valignment", "top");
    }
    g_object_set(kmssink, "driver-name", "hailo-drm", "force-modesetting", TRUE,
                 "can-scale", FALSE, "sync", FALSE, NULL);
    g_object_set(fpsdisplay, "video-sink", kmssink, "text-overlay", FALSE, "sync", FALSE,
                 "fps-update-interval", config.show_fps ? 1000 : 10000,
                 "signal-fps-measurements", TRUE, NULL);
    output.last_fps_log = std::chrono::steady_clock::now();
    g_signal_connect(fpsdisplay, "fps-measurements", G_CALLBACK(on_fps_measurement), &output);

    GstPad *sink_pad = gst_element_get_static_pad(fpsdisplay, "sink");
    gst_pad_add_probe(sink_pad, GST_PAD_PROBE_TYPE_BUFFER, draw_button_and_capture, &probe, nullptr);
    gst_object_unref(sink_pad);

    gst_bin_add_many(GST_BIN(output.pipeline), output.appsrc, queue, convert, capsfilter, NULL);
    if (output.fps_text)
        gst_bin_add(GST_BIN(output.pipeline), output.fps_text);
    gst_bin_add(GST_BIN(output.pipeline), fpsdisplay);
    return output.fps_text
               ? gst_element_link_many(output.appsrc, queue, convert, capsfilter, output.fps_text, fpsdisplay, NULL)
               : gst_element_link_many(output.appsrc, queue, convert, capsfilter, fpsdisplay, NULL);
}

static bool build_input_pipeline(const AppConfig &config, int profile, InputPipeline &input,
                                 BridgeContext &bridge)
{
    if (!write_runtime_config(config.config_path, profile))
        return false;
    input.pipeline = gst_pipeline_new("astrial-ai-isp-camera");
    input.vision = gst_element_factory_make("gsthailovision", "vision");
    GstElement *queue = gst_element_factory_make("queue", "camera_queue");
    input.appsink = gst_element_factory_make("appsink", "camera_sink");
    if (!input.pipeline || !input.vision || !queue || !input.appsink)
        return false;

    g_object_set(input.vision, "config-path", RUNTIME_MEDIALIB_CONFIG, NULL);
    g_object_set(queue, "leaky", 2, "max-size-buffers", 2, "max-size-bytes", 0,
                 "max-size-time", static_cast<guint64>(0), NULL);
    g_object_set(input.appsink, "emit-signals", TRUE, "max-buffers", 2, "drop", TRUE,
                 "sync", FALSE, "wait-on-eos", FALSE, NULL);
    g_signal_connect(input.appsink, "new-sample", G_CALLBACK(forward_sample), &bridge);

    gst_bin_add_many(GST_BIN(input.pipeline), input.vision, queue, input.appsink, NULL);
    if (!gst_element_link(queue, input.appsink))
        return false;
    GstPad *vision_src = gst_element_request_pad_simple(input.vision, STREAM_ID);
    GstPad *queue_sink = gst_element_get_static_pad(queue, "sink");
    const bool linked = vision_src && queue_sink && gst_pad_link(vision_src, queue_sink) == GST_PAD_LINK_OK;
    if (vision_src)
        gst_object_unref(vision_src);
    if (queue_sink)
        gst_object_unref(queue_sink);
    return linked;
}

static void destroy_input_pipeline(InputPipeline &input)
{
    if (input.pipeline)
    {
        gst_element_set_state(input.pipeline, GST_STATE_NULL);
        gst_object_unref(input.pipeline);
    }
    input = {};
}

static void destroy_output_pipeline(OutputPipeline &output)
{
    if (output.pipeline)
    {
        gst_element_set_state(output.pipeline, GST_STATE_NULL);
        gst_object_unref(output.pipeline);
    }
    output = {};
}

static void run_control_loop(const AppConfig &config, InputPipeline &input, BridgeContext &bridge,
                             CaptureState &state, int timeout)
{
    const auto started = std::chrono::steady_clock::now();
    while (true)
    {
        {
            std::unique_lock<std::mutex> lock(g_stop_mutex);
            g_stop_cv.wait_for(lock, std::chrono::milliseconds(50), [] { return g_stop_requested; });
            if (g_stop_requested)
                return;
        }
        if (std::chrono::steady_clock::now() - started >= std::chrono::seconds(timeout))
            return;

        const int requested = state.requested_profile.load();
        if (requested < CaptureState::STANDARD || requested > CaptureState::AI ||
            requested == state.current_profile.load())
            continue;

        state.switching_profile = true;
        const char *name = requested == CaptureState::AI ? AI_PROFILE : STANDARD_PROFILE;
        std::cout << "[profile] switching to " << name << std::endl;
        g_reconfiguring = true;
        destroy_input_pipeline(input);
        bool switched = build_input_pipeline(config, requested, input, bridge);
        if (switched)
            add_bus_handler(input.pipeline);
        if (switched)
            switched = wait_for_state(input.pipeline, GST_STATE_PLAYING, 20 * GST_SECOND);
        g_reconfiguring = false;
        if (switched && !g_gstreamer_error.load())
        {
            state.current_profile = requested;
            std::cout << "[profile] active " << name << std::endl;
        }
        else
        {
            std::cerr << "[profile] failed to activate " << name << std::endl;
            destroy_input_pipeline(input);
            request_stop();
        }
        state.requested_profile = -1;
        state.switching_profile = false;
    }
}

int main(int argc, char **argv)
{
    const bool wants_help = std::any_of(argv + 1, argv + argc, [](const char *argument) {
        return std::strcmp(argument, "-h") == 0 || std::strcmp(argument, "--help") == 0;
    });
    if (!wants_help && !std::getenv("SYSELE_LAUNCHED"))
    {
        std::cerr << "dsi_ai_isp_app: start it with the dsi_ai_isp command" << std::endl;
        return 2;
    }

    AppConfig config{};
    bool help_requested = false;
    if (!parse_arguments(argc, argv, config, help_requested))
        return help_requested ? 0 : 1;

    signal_utils::SignalHandler signal_handler(false);
    signal_handler.register_signal_handler([]([[maybe_unused]] int signal) { request_stop(); });

    CaptureState capture;
    PhotoSaver saver(config.output_path, capture);
    TouchPhotoReader touch;
    if (!touch.open(capture))
        return 1;

    gst_init(&argc, &argv);
    InputPipeline input;
    OutputPipeline output;
    BridgeContext bridge;
    PhotoProbeData probe{capture, saver};
    capture.current_profile = config.initial_profile;
    if (!build_output_pipeline(config, output, probe))
        return 1;
    bridge.appsrc = output.appsrc;
    if (!build_input_pipeline(config, config.initial_profile, input, bridge))
        return 1;
    add_bus_handler(input.pipeline);
    add_bus_handler(output.pipeline);

    saver.start();
    touch.start();
    if (gst_element_set_state(output.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE ||
        !wait_for_state(input.pipeline, GST_STATE_PLAYING, 20 * GST_SECOND) ||
        gst_element_get_state(output.pipeline, nullptr, nullptr, 10 * GST_SECOND) == GST_STATE_CHANGE_FAILURE)
    {
        std::cerr << "Failed to start the DSI AI-ISP application" << std::endl;
        touch.stop();
        destroy_input_pipeline(input);
        destroy_output_pipeline(output);
        saver.stop();
        return 1;
    }

    std::cout << "Touch: " << touch.name() << " on " << touch.path() << std::endl;
    std::cout << "Touch STANDARD ISP or AI-ISP to change the active profile" << std::endl;
    std::cout << "Touch PHOTO to save a clean frame using the active profile" << std::endl;
    run_control_loop(config, input, bridge, capture, config.timeout);
    touch.stop();
    destroy_input_pipeline(input);
    destroy_output_pipeline(output);
    saver.stop();

    const int exit_code = g_gstreamer_error ? 1 : 0;
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(exit_code);
}
