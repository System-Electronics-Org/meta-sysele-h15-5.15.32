// SPDX-License-Identifier: MIT
/*
 * Astrial H15: touch test for the DSI panel.
 *
 * The screen starts black. Wherever a finger touches or slides, a red dot about
 * 1 cm across appears at full brightness and fades back to black within a few
 * seconds. Every finger is tracked on its own, and consecutive points are
 * joined, so a fast stroke leaves a continuous trail instead of separate dots.
 *
 * Touches are read straight from the evdev device. Frames reach the panel the
 * same way dsi_detection sends them: appsrc -> BGR -> kmssink on hailo-drm.
 */
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include <opencv2/imgproc.hpp>

#include <dirent.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

constexpr int DISPLAY_WIDTH = 1280;
constexpr int DISPLAY_HEIGHT = 800;
// The Waveshare 10.1" panel is 217 mm wide for 1280 pixels: about 5.9 px/mm.
constexpr double PIXELS_PER_MM = 1280.0 / 217.0;
constexpr int MAX_SLOTS = 10;
constexpr int LONG_BITS = 8 * sizeof(long);

std::atomic<bool> g_stop{false};

void on_signal(int)
{
    g_stop = true;
}

bool test_bit(int bit, const unsigned long *bits)
{
    return (bits[bit / LONG_BITS] >> (bit % LONG_BITS)) & 1UL;
}

struct Options
{
    std::string device; // empty: the first touch device found
    double fade_s = 3.0;
    double size_mm = 10.0;
    int fps = 30;
    double duration_s = 0; // 0: until stopped
};

struct Segment
{
    cv::Point from;
    cv::Point to;
};

// Reads one evdev touch device on its own thread and turns contacts into
// screen segments. Multi-touch devices are followed slot by slot; a device
// without multi-touch is followed through ABS_X, ABS_Y and BTN_TOUCH.
class TouchReader
{
  public:
    ~TouchReader()
    {
        stop();
        if (m_fd >= 0)
            ::close(m_fd);
    }

    bool open(const std::string &path)
    {
        m_fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (m_fd < 0)
        {
            std::fprintf(stderr, "dsi_touch_test: cannot open %s: %s\n", path.c_str(), std::strerror(errno));
            return false;
        }
        m_path = path;

        char name[128] = "unknown";
        ::ioctl(m_fd, EVIOCGNAME(sizeof(name)), name);
        m_name = name;

        unsigned long abs_bits[(ABS_MAX + LONG_BITS) / LONG_BITS] = {};
        ::ioctl(m_fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits);
        m_multitouch = test_bit(ABS_MT_POSITION_X, abs_bits) && test_bit(ABS_MT_POSITION_Y, abs_bits);
        if (!m_multitouch && !(test_bit(ABS_X, abs_bits) && test_bit(ABS_Y, abs_bits)))
        {
            std::fprintf(stderr, "dsi_touch_test: %s reports no touch coordinates\n", path.c_str());
            return false;
        }

        const int code_x = m_multitouch ? ABS_MT_POSITION_X : ABS_X;
        const int code_y = m_multitouch ? ABS_MT_POSITION_Y : ABS_Y;
        if (::ioctl(m_fd, EVIOCGABS(code_x), &m_abs_x) < 0 || ::ioctl(m_fd, EVIOCGABS(code_y), &m_abs_y) < 0)
        {
            std::fprintf(stderr, "dsi_touch_test: cannot read the axis ranges of %s\n", path.c_str());
            return false;
        }
        return true;
    }

    // The first device under /dev/input that reports touch coordinates.
    static std::string find_device()
    {
        std::vector<int> numbers;
        if (DIR *dir = ::opendir("/dev/input"))
        {
            while (dirent *entry = ::readdir(dir))
            {
                int n;
                if (std::sscanf(entry->d_name, "event%d", &n) == 1)
                    numbers.push_back(n);
            }
            ::closedir(dir);
        }
        std::sort(numbers.begin(), numbers.end());
        for (int n : numbers)
        {
            const std::string path = "/dev/input/event" + std::to_string(n);
            const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0)
                continue;
            unsigned long abs_bits[(ABS_MAX + LONG_BITS) / LONG_BITS] = {};
            ::ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits);
            ::close(fd);
            if (test_bit(ABS_MT_POSITION_X, abs_bits) || test_bit(ABS_X, abs_bits))
                return path;
        }
        return "";
    }

    void start()
    {
        m_thread = std::thread(&TouchReader::run, this);
    }

    void stop()
    {
        m_quit = true;
        if (m_thread.joinable())
            m_thread.join();
    }

    std::vector<Segment> take()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<Segment> out;
        out.swap(m_pending);
        return out;
    }

    bool failed() const
    {
        return m_failed;
    }

    const std::string &path() const
    {
        return m_path;
    }
    const std::string &name() const
    {
        return m_name;
    }
    bool multitouch() const
    {
        return m_multitouch;
    }
    const input_absinfo &abs_x() const
    {
        return m_abs_x;
    }
    const input_absinfo &abs_y() const
    {
        return m_abs_y;
    }

  private:
    struct Slot
    {
        bool active = false;
        bool moved = false;
        bool has_last = false;
        int x = 0;
        int y = 0;
        cv::Point last;
    };

    cv::Point to_screen(int x, int y) const
    {
        const auto scale = [](int v, const input_absinfo &a, int size) {
            const int span = std::max(1, a.maximum - a.minimum);
            const int p = static_cast<int>(std::lround(double(v - a.minimum) * (size - 1) / span));
            return std::clamp(p, 0, size - 1);
        };
        return {scale(x, m_abs_x, DISPLAY_WIDTH), scale(y, m_abs_y, DISPLAY_HEIGHT)};
    }

    void handle(const input_event &ev)
    {
        if (m_dropped)
        {
            // After SYN_DROPPED the state is unknown until the next report:
            // skip to it and start every contact again without a trail.
            if (ev.type == EV_SYN && ev.code == SYN_REPORT)
            {
                m_dropped = false;
                for (Slot &s : m_slots)
                    s.has_last = false;
            }
            return;
        }

        Slot &slot = m_slots[m_slot];
        switch (ev.type)
        {
        case EV_ABS:
            if (m_multitouch)
            {
                if (ev.code == ABS_MT_SLOT)
                    m_slot = std::clamp(ev.value, 0, MAX_SLOTS - 1);
                else if (ev.code == ABS_MT_TRACKING_ID)
                {
                    slot.active = ev.value >= 0;
                    slot.has_last = false;
                }
                else if (ev.code == ABS_MT_POSITION_X)
                {
                    slot.x = ev.value;
                    slot.moved = true;
                }
                else if (ev.code == ABS_MT_POSITION_Y)
                {
                    slot.y = ev.value;
                    slot.moved = true;
                }
            }
            else if (ev.code == ABS_X || ev.code == ABS_Y)
            {
                (ev.code == ABS_X ? m_slots[0].x : m_slots[0].y) = ev.value;
                m_slots[0].moved = true;
            }
            break;
        case EV_KEY:
            if (!m_multitouch && ev.code == BTN_TOUCH)
            {
                m_slots[0].active = ev.value != 0;
                m_slots[0].has_last = false;
            }
            break;
        case EV_SYN:
            if (ev.code == SYN_DROPPED)
                m_dropped = true;
            else if (ev.code == SYN_REPORT)
                report();
            break;
        default:
            break;
        }
    }

    void report()
    {
        std::vector<Segment> segments;
        for (Slot &s : m_slots)
        {
            if (!s.active || !s.moved)
                continue;
            const cv::Point p = to_screen(s.x, s.y);
            segments.push_back({s.has_last ? s.last : p, p});
            s.last = p;
            s.has_last = true;
            s.moved = false;
        }
        if (segments.empty())
            return;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending.insert(m_pending.end(), segments.begin(), segments.end());
    }

    void run()
    {
        input_event events[64];
        while (!m_quit && !g_stop)
        {
            pollfd pfd{m_fd, POLLIN, 0};
            if (::poll(&pfd, 1, 100) <= 0)
                continue;
            const ssize_t n = ::read(m_fd, events, sizeof(events));
            if (n < 0)
            {
                if (errno == EAGAIN || errno == EINTR)
                    continue;
                std::fprintf(stderr, "dsi_touch_test: reading %s failed: %s\n", m_path.c_str(), std::strerror(errno));
                m_failed = true;
                g_stop = true;
                return;
            }
            for (ssize_t i = 0; i < n / static_cast<ssize_t>(sizeof(input_event)); i++)
                handle(events[i]);
        }
    }

    int m_fd = -1;
    std::string m_path;
    std::string m_name;
    bool m_multitouch = false;
    input_absinfo m_abs_x{};
    input_absinfo m_abs_y{};
    Slot m_slots[MAX_SLOTS];
    int m_slot = 0;
    bool m_dropped = false;
    std::mutex m_mutex;
    std::vector<Segment> m_pending;
    std::thread m_thread;
    std::atomic<bool> m_quit{false};
    std::atomic<bool> m_failed{false};
};

// The display path of dsi_detection: appsrc -> queue -> videoconvert -> BGR
// -> kmssink on hailo-drm. The frames are BGR already, so videoconvert passes
// them through.
struct Display
{
    GstElement *pipeline = nullptr;
    GstElement *appsrc = nullptr;
    int par_n = 1;
    int par_d = 1;

    bool build(int fps)
    {
        pipeline = gst_pipeline_new("dsi-touch-test");
        appsrc = gst_element_factory_make("appsrc", "canvas");
        GstElement *queue = gst_element_factory_make("queue", "display_queue");
        GstElement *convert = gst_element_factory_make("videoconvert", "display_convert");
        GstElement *capsfilter = gst_element_factory_make("capsfilter", "display_caps");
        GstElement *kmssink = gst_element_factory_make("kmssink", "dsi_sink");
        if (!pipeline || !appsrc || !queue || !convert || !capsfilter || !kmssink)
        {
            std::fprintf(stderr, "dsi_touch_test: cannot create the display elements\n");
            return false;
        }

        g_object_set(appsrc, "is-live", TRUE, "format", GST_FORMAT_TIME, "do-timestamp", TRUE, NULL);
        g_object_set(queue, "leaky", 2, "max-size-buffers", 3, "max-size-bytes", 0,
                     "max-size-time", static_cast<guint64>(0), NULL);
        g_object_set(kmssink, "driver-name", "hailo-drm", "force-modesetting", TRUE, "can-scale", FALSE, "sync",
                     FALSE, NULL);

        gst_bin_add_many(GST_BIN(pipeline), appsrc, queue, convert, capsfilter, kmssink, NULL);
        if (!gst_element_link_many(appsrc, queue, convert, capsfilter, kmssink, NULL))
        {
            std::fprintf(stderr, "dsi_touch_test: cannot link appsrc -> BGR -> kmssink\n");
            return false;
        }

        // kmssink derives a pixel aspect ratio from the physical size the panel
        // driver reports, and refuses frames that declare another one, which
        // videoconvert cannot change. It knows the ratio once started, so the
        // pipeline is started first and the frames are described with its
        // value: nothing to update here if the driver's size changes.
        if (gst_element_set_state(pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE)
            return false;
        par_n = par_d = 1;
        GstPad *pad = gst_element_get_static_pad(kmssink, "sink");
        if (GstCaps *sink_caps = gst_pad_query_caps(pad, nullptr))
        {
            if (!gst_caps_is_empty(sink_caps))
                gst_structure_get_fraction(gst_caps_get_structure(sink_caps, 0), "pixel-aspect-ratio", &par_n,
                                           &par_d);
            gst_caps_unref(sink_caps);
        }
        gst_object_unref(pad);

        GstCaps *caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGR", "width", G_TYPE_INT,
                                            DISPLAY_WIDTH, "height", G_TYPE_INT, DISPLAY_HEIGHT, "framerate",
                                            GST_TYPE_FRACTION, fps, 1, "pixel-aspect-ratio", GST_TYPE_FRACTION,
                                            par_n, par_d, NULL);
        g_object_set(appsrc, "caps", caps, NULL);
        g_object_set(capsfilter, "caps", caps, NULL);
        gst_caps_unref(caps);
        return gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
    }

    bool push(const cv::Mat &frame)
    {
        const gsize size = frame.total() * frame.elemSize();
        GstBuffer *buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
        gst_buffer_fill(buffer, 0, frame.data, size);
        return gst_app_src_push_buffer(GST_APP_SRC(appsrc), buffer) == GST_FLOW_OK;
    }

    // An error posted by any element, printed; false if there was one.
    bool healthy()
    {
        GstBus *bus = gst_element_get_bus(pipeline);
        GstMessage *msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        gst_object_unref(bus);
        if (!msg)
            return true;
        GError *err = nullptr;
        gchar *debug = nullptr;
        gst_message_parse_error(msg, &err, &debug);
        std::fprintf(stderr, "dsi_touch_test: %s: %s\n", GST_OBJECT_NAME(msg->src), err ? err->message : "error");
        if (debug)
            std::fprintf(stderr, "dsi_touch_test:   %s\n", debug);
        g_clear_error(&err);
        g_free(debug);
        gst_message_unref(msg);
        return false;
    }

    // Let the sink see the end of the stream before the pipeline is torn down,
    // so the panel goes back to the console cleanly.
    void close()
    {
        if (!pipeline)
            return;
        gst_app_src_end_of_stream(GST_APP_SRC(appsrc));
        GstBus *bus = gst_element_get_bus(pipeline);
        GstMessage *msg = gst_bus_timed_pop_filtered(bus, 2 * GST_SECOND,
                                                     static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        if (msg)
            gst_message_unref(msg);
        gst_object_unref(bus);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        pipeline = nullptr;
    }
};

void usage()
{
    std::printf("Usage: dsi_touch_test [options]\n"
                "\n"
                "Paints a red dot under every finger on a black screen; the dots\n"
                "fade back to black. Stop it with Ctrl+C.\n"
                "\n"
                "Options:\n"
                "  -d, --device PATH     touch device (default: the first one found)\n"
                "  -f, --fade SEC        time for a dot to fade to black (default: 3)\n"
                "  -s, --size MM         dot diameter in millimetres (default: 10)\n"
                "  -r, --fps N           display frame rate, 1 to 60 (default: 30)\n"
                "  -t, --duration SEC    stop after SEC seconds (default: run until stopped)\n"
                "  -h, --help            show this help\n");
}

bool parse(int argc, char **argv, Options &opt)
{
    static const option longopts[] = {
        {"device", required_argument, nullptr, 'd'}, {"fade", required_argument, nullptr, 'f'},
        {"size", required_argument, nullptr, 's'},   {"fps", required_argument, nullptr, 'r'},
        {"duration", required_argument, nullptr, 't'}, {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };
    int c;
    while ((c = getopt_long(argc, argv, "d:f:s:r:t:h", longopts, nullptr)) != -1)
    {
        switch (c)
        {
        case 'd':
            opt.device = optarg;
            break;
        case 'f':
            opt.fade_s = std::atof(optarg);
            break;
        case 's':
            opt.size_mm = std::atof(optarg);
            break;
        case 'r':
            opt.fps = std::atoi(optarg);
            break;
        case 't':
            opt.duration_s = std::atof(optarg);
            break;
        case 'h':
            usage();
            std::exit(0);
        default:
            usage();
            return false;
        }
    }
    if (opt.fade_s <= 0 || opt.size_mm <= 0 || opt.fps < 1 || opt.fps > 60 || opt.duration_s < 0)
    {
        std::fprintf(stderr, "dsi_touch_test: fade and size must be positive, fps between 1 and 60\n");
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    Options opt;
    if (!parse(argc, argv, opt))
        return 2;

    struct sigaction sa = {};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    const std::string device = opt.device.empty() ? TouchReader::find_device() : opt.device;
    if (device.empty())
    {
        std::fprintf(stderr, "dsi_touch_test: no touch device found under /dev/input\n");
        return 1;
    }
    TouchReader touch;
    if (!touch.open(device))
        return 1;

    gst_init(&argc, &argv);
    Display display;
    if (!display.build(opt.fps))
    {
        display.close();
        return 1;
    }

    const int dot = std::max(2, static_cast<int>(std::lround(opt.size_mm * PIXELS_PER_MM)));
    std::printf("dsi_touch_test: %s on %s, %s, x %d..%d, y %d..%d\n", touch.name().c_str(), touch.path().c_str(),
                touch.multitouch() ? "multi-touch" : "single touch", touch.abs_x().minimum, touch.abs_x().maximum,
                touch.abs_y().minimum, touch.abs_y().maximum);
    std::printf("dsi_touch_test: dot %d px (%.0f mm), fade %.1f s, %d fps, pixel aspect %d/%d. Ctrl+C to stop.\n", dot,
                opt.size_mm, opt.fade_s, opt.fps, display.par_n, display.par_d);
    std::fflush(stdout);

    touch.start();

    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / opt.fps));
    const cv::Scalar red(0, 0, 255);
    cv::Mat canvas(DISPLAY_HEIGHT, DISPLAY_WIDTH, CV_8UC3, cv::Scalar::all(0));

    const auto start = clock::now();
    auto previous = start;
    auto next = start;
    auto last_touch = start;
    double fade_carry = 0;
    // While anything is lit the canvas fades and goes out every frame. Once it
    // is black again nothing changes, and nothing is sent until the next touch.
    bool lit = false;
    bool first = true;
    int status = 0;

    while (!g_stop)
    {
        const auto now = clock::now();
        if (opt.duration_s > 0 && std::chrono::duration<double>(now - start).count() >= opt.duration_s)
            break;
        const double dt = std::chrono::duration<double>(now - previous).count();
        previous = now;

        if (lit)
        {
            // Linear fade: full red reaches black in exactly fade_s seconds.
            fade_carry += 255.0 * dt / opt.fade_s;
            const int step = static_cast<int>(fade_carry);
            if (step > 0)
            {
                cv::subtract(canvas, cv::Scalar::all(step), canvas);
                fade_carry -= step;
            }
        }

        const std::vector<Segment> segments = touch.take();
        for (const Segment &s : segments)
        {
            cv::line(canvas, s.from, s.to, red, dot, cv::LINE_AA);
            cv::circle(canvas, s.to, dot / 2, red, cv::FILLED, cv::LINE_AA);
        }
        if (!segments.empty())
        {
            lit = true;
            last_touch = now;
        }

        bool send = first || lit;
        if (lit && std::chrono::duration<double>(now - last_touch).count() > opt.fade_s + 0.2)
        {
            canvas.setTo(cv::Scalar::all(0));
            lit = false;
            fade_carry = 0;
            send = true;
        }

        if (send && !display.push(canvas))
        {
            std::fprintf(stderr, "dsi_touch_test: the display pipeline refused a frame\n");
            status = 1;
            break;
        }
        first = false;

        if (!display.healthy())
        {
            status = 1;
            break;
        }

        next += period;
        if (next < clock::now())
            next = clock::now();
        std::this_thread::sleep_until(next);
    }

    touch.stop();
    display.close();
    if (touch.failed())
        status = 1;
    std::printf("dsi_touch_test: stopped\n");
    return status;
}
