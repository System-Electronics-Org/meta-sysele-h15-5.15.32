// SPDX-License-Identifier: MIT
/*
 * Astrial H15: camera -> YOLOv8n -> overlay -> DSI.
 *
 * The camera side mirrors /usr/bin/dsi_demo: hailofrontendbinsrc reads
 * /opt/sysele/var/frontend_dsi_30.json. The output is raw BGR sent directly
 * to kmssink; there is no encoder, RTP or UDP path.
 */

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <cxxopts/cxxopts.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <opencv2/imgproc.hpp>

#include "media_library/dma_memory_allocator.hpp"
#include "media_library/signal_utils.hpp"
#include "hailo_postprocess_tools/objects/hailo_common.hpp"
#include "hailo_analytics/analytics/ai_models_config.hpp"
#include "hailo_analytics/analytics/tiling.hpp"
#include "hailo_analytics/pipeline/core/buffer.hpp"
#include "hailo_analytics/pipeline/core/pipeline.hpp"
#include "hailo_analytics/pipeline/core/pipeline_builder.hpp"
#include "hailo_analytics/pipeline/core/stage.hpp"
#include "hailo_analytics/pipeline/overlay/overlay_stage.hpp"
#include "hailo_analytics/pipeline/sources/gst_source_stage.hpp"

namespace pipeline = hailo_analytics::pipeline;

static constexpr const char *DEFAULT_FRONTEND_CONFIG = "/opt/sysele/var/frontend_dsi_30.json";
static constexpr const char *STREAM_ID = "sink0";
static constexpr const char *TILING_PIPELINE = "tiling_detection_pipeline";
static constexpr int DISPLAY_WIDTH = 1280;
static constexpr int DISPLAY_HEIGHT = 800;
static constexpr int DEFAULT_DISPLAY_FPS = 30;

enum class FaceEffect
{
    NONE,
    GAUSSIAN,
    HAILO_BOX,
    PIXELATION,
};

static const char *face_effect_name(FaceEffect effect)
{
    switch (effect)
    {
    case FaceEffect::GAUSSIAN:
        return "gaussian";
    case FaceEffect::HAILO_BOX:
        return "box";
    case FaceEffect::PIXELATION:
        return "pixel";
    default:
        return "n";
    }
}

struct AppConfig
{
    int timeout;
    int framerate;
    int inference_interval;
    FaceEffect face_effect;
    int privacy_strength;
    bool show_fps;
    std::string config_path;
};

struct InputPipeline
{
    GstElement *pipeline = nullptr;
    GstElement *frontend = nullptr;
    GstElement *appsink = nullptr;
};

struct OutputPipeline
{
    GstElement *pipeline = nullptr;
    GstElement *appsrc = nullptr;
    GstElement *fps_text = nullptr;
    std::chrono::steady_clock::time_point last_fps_log{};
};

static std::mutex g_stop_mutex;
static std::condition_variable g_stop_cv;
static bool g_stop_requested = false;
static std::atomic<bool> g_gstreamer_error{false};

static void request_stop()
{
    {
        std::lock_guard<std::mutex> lock(g_stop_mutex);
        g_stop_requested = true;
    }
    g_stop_cv.notify_all();
}

static void wait_for_stop_or_timeout(int seconds)
{
    std::unique_lock<std::mutex> lock(g_stop_mutex);
    g_stop_cv.wait_for(lock, std::chrono::seconds(seconds), [] { return g_stop_requested; });
}

static GstBusSyncReply bus_sync_callback(GstBus *, GstMessage *message, gpointer)
{
    switch (GST_MESSAGE_TYPE(message))
    {
    case GST_MESSAGE_ERROR: {
        GError *error = nullptr;
        gchar *debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        std::cerr << "GStreamer error from " << GST_OBJECT_NAME(message->src) << ": "
                  << (error ? error->message : "unknown") << std::endl;
        if (debug)
            std::cerr << "  Debug: " << debug << std::endl;
        g_clear_error(&error);
        g_free(debug);
        g_gstreamer_error = true;
        request_stop();
        break;
    }
    case GST_MESSAGE_EOS:
        std::cout << "End of stream from " << GST_OBJECT_NAME(message->src) << std::endl;
        request_stop();
        break;
    default:
        break;
    }
    return GST_BUS_PASS;
}

static void add_bus_handler(GstElement *gst_pipeline)
{
    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(gst_pipeline));
    gst_bus_set_sync_handler(bus, bus_sync_callback, nullptr, nullptr);
    gst_object_unref(bus);
}

static void on_fps_measurement(GstElement *, gdouble fps, gdouble drop_rate, gdouble average_fps, gpointer user_data)
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

static bool parse_arguments(int argc, char **argv, AppConfig &config, bool &help_requested)
{
    cxxopts::Options options("dsi_detection_app",
                             "Astrial H15 YOLOv8n detection with direct DSI output.\n"
                             "This is the executable behind the dsi_detection command, which prepares\n"
                             "the camera and the logs before starting it. Run: dsi_detection --help");
    options.add_options()
        ("h,help", "Show this help")
        ("t,duration", "Time to run in seconds, or inf to run until stopped",
         cxxopts::value<std::string>()->default_value("inf"))
        ("f,fps", "Camera and display frame rate",
         cxxopts::value<int>()->default_value(std::to_string(DEFAULT_DISPLAY_FPS)))
        ("i,inference-interval", "Run inference every N accepted frames",
         cxxopts::value<int>()->default_value("2"))
        ("e,face-effect", "Face effect: n, y/gaussian, box, or pixel",
         cxxopts::value<std::string>()->default_value("n"))
        ("face-blur", "Legacy alias for --face-effect", cxxopts::value<std::string>())
        ("s,privacy-strength", "Privacy effect strength from 1 to 5",
         cxxopts::value<int>()->default_value("3"))
        ("show-fps", "Show a small FPS counter on DSI",
         cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
        ("c,config-file-path", "hailofrontendbinsrc JSON configuration",
         cxxopts::value<std::string>()->default_value(DEFAULT_FRONTEND_CONFIG));

    // The names this executable answered to before it spoke the same language as
    // the command. Kept working, kept out of the help.
    options.add_options("compatibility")
        ("timeout", "Former name of --duration", cxxopts::value<std::string>())
        ("framerate", "Former name of --fps", cxxopts::value<int>());

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

    const std::string timeout =
        result.count("timeout") ? result["timeout"].as<std::string>() : result["duration"].as<std::string>();
    if (timeout == "inf" || timeout == "n")
        config.timeout = std::numeric_limits<int>::max();
    else
    {
        try
        {
            config.timeout = std::stoi(timeout);
        }
        catch (const std::exception &)
        {
            std::cerr << "timeout must be a number of seconds, or inf" << std::endl;
            return false;
        }
    }
    config.framerate = result.count("framerate") ? result["framerate"].as<int>() : result["fps"].as<int>();
    config.inference_interval = result["inference-interval"].as<int>();
    const std::string face_blur = result.count("face-blur")
                                      ? result["face-blur"].as<std::string>()
                                      : result["face-effect"].as<std::string>();
    if (face_blur == "n")
        config.face_effect = FaceEffect::NONE;
    else if (face_blur == "y" || face_blur == "gaussian")
        config.face_effect = FaceEffect::GAUSSIAN;
    else if (face_blur == "box")
        config.face_effect = FaceEffect::HAILO_BOX;
    else if (face_blur == "pixel")
        config.face_effect = FaceEffect::PIXELATION;
    else
    {
        std::cerr << "face-effect must be n, y, gaussian, box, or pixel" << std::endl;
        return false;
    }
    config.privacy_strength = result["privacy-strength"].as<int>();
    config.show_fps = result["show-fps"].as<bool>();
    config.config_path = result["config-file-path"].as<std::string>();
    {
        std::ifstream config_file(config.config_path);
        if (!config_file.good())
        {
            std::cerr << config.config_path << " is missing: run dsi_config first, or pass another one with "
                      << "--config-file-path" << std::endl;
            return false;
        }
    }
    if (config.timeout <= 0)
    {
        std::cerr << "timeout must be greater than zero" << std::endl;
        return false;
    }
    if (config.framerate <= 0)
    {
        std::cerr << "framerate must be greater than zero" << std::endl;
        return false;
    }
    if (config.inference_interval <= 0)
    {
        std::cerr << "inference-interval must be greater than zero" << std::endl;
        return false;
    }
    if (config.privacy_strength < 1 || config.privacy_strength > 5)
    {
        std::cerr << "privacy-strength must be between 1 and 5" << std::endl;
        return false;
    }
    return true;
}

static bool build_input_pipeline(const AppConfig &config, InputPipeline &input)
{
    input.pipeline = gst_pipeline_new("astrial-analytics-input");
    input.frontend = gst_element_factory_make("hailofrontendbinsrc", "frontend");
    GstElement *queue = gst_element_factory_make("queue", "camera_queue");
    input.appsink = gst_element_factory_make("appsink", "appsink0");
    if (!input.pipeline || !input.frontend || !queue || !input.appsink)
    {
        std::cerr << "Failed to create input pipeline elements" << std::endl;
        return false;
    }

    g_object_set(input.frontend, "config-file-path", config.config_path.c_str(), NULL);
    g_object_set(queue, "leaky", 2, "max-size-buffers", 5, "max-size-bytes", 0,
                 "max-size-time", static_cast<guint64>(0), NULL);
    g_object_set(input.appsink, "emit-signals", FALSE, "max-buffers", 2, "drop", TRUE,
                 "sync", FALSE, "wait-on-eos", FALSE, NULL);

    gst_bin_add_many(GST_BIN(input.pipeline), input.frontend, queue, input.appsink, NULL);
    if (!gst_element_link(queue, input.appsink))
    {
        std::cerr << "Failed to link camera queue to appsink" << std::endl;
        return false;
    }

    GstPad *frontend_src = gst_element_request_pad_simple(input.frontend, "src_%u");
    GstPad *queue_sink = gst_element_get_static_pad(queue, "sink");
    if (!frontend_src || !queue_sink || gst_pad_link(frontend_src, queue_sink) != GST_PAD_LINK_OK)
    {
        std::cerr << "Failed to link hailofrontendbinsrc src_0 to camera queue" << std::endl;
        if (frontend_src)
            gst_object_unref(frontend_src);
        if (queue_sink)
            gst_object_unref(queue_sink);
        return false;
    }
    gst_object_unref(frontend_src);
    gst_object_unref(queue_sink);

    return true;
}

static bool build_output_pipeline(const AppConfig &config, OutputPipeline &output)
{
    output.pipeline = gst_pipeline_new("astrial-analytics-output");
    output.appsrc = gst_element_factory_make("appsrc", "appsrc0");
    GstElement *queue = gst_element_factory_make("queue", "display_queue");
    GstElement *convert = gst_element_factory_make("videoconvert", "display_convert");
    GstElement *capsfilter = gst_element_factory_make("capsfilter", "display_caps");
    output.fps_text = config.show_fps ? gst_element_factory_make("textoverlay", "fps_text") : nullptr;
    GstElement *fpsdisplay = gst_element_factory_make("fpsdisplaysink", "display_fps");
    GstElement *kmssink = gst_element_factory_make("kmssink", "dsi_sink");
    if (!output.pipeline || !output.appsrc || !queue || !convert || !capsfilter || !fpsdisplay || !kmssink ||
        (config.show_fps && !output.fps_text))
    {
        std::cerr << "Failed to create direct DSI output elements" << std::endl;
        return false;
    }

    g_object_set(output.appsrc, "is-live", TRUE, "format", GST_FORMAT_TIME, "do-timestamp", TRUE, NULL);
    g_object_set(queue, "leaky", 2, "max-size-buffers", 3, "max-size-bytes", 0,
                 "max-size-time", static_cast<guint64>(0), NULL);
    g_object_set(convert, "n-threads", 4, NULL);

    GstCaps *display_caps = gst_caps_new_simple(
        "video/x-raw", "format", G_TYPE_STRING, "BGR", "width", G_TYPE_INT, DISPLAY_WIDTH,
        "height", G_TYPE_INT, DISPLAY_HEIGHT, "framerate", GST_TYPE_FRACTION, config.framerate, 1, NULL);
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

    gst_bin_add_many(GST_BIN(output.pipeline), output.appsrc, queue, convert, capsfilter, NULL);
    if (output.fps_text)
        gst_bin_add(GST_BIN(output.pipeline), output.fps_text);
    gst_bin_add(GST_BIN(output.pipeline), fpsdisplay);

    const bool linked = output.fps_text
                            ? gst_element_link_many(output.appsrc, queue, convert, capsfilter, output.fps_text,
                                                    fpsdisplay, NULL)
                            : gst_element_link_many(output.appsrc, queue, convert, capsfilter, fpsdisplay, NULL);
    if (!linked)
    {
        std::cerr << "Failed to link appsrc -> BGR -> optional FPS overlay -> kmssink" << std::endl;
        return false;
    }
    return true;
}

class FaceEffectStage : public pipeline::ThreadedStage
{
  public:
    FaceEffectStage(std::string name, FaceEffect effect, int strength)
        : pipeline::ThreadedStage(std::move(name), 3, true, true), m_effect(effect), m_strength(strength)
    {
    }

    pipeline::AppStatus process(pipeline::BufferPtr data) override
    {
        HailoMediaLibraryBufferPtr source = data->get_buffer();
        if (!source || source->get_num_of_planes() < 2)
            return pipeline::AppStatus::PIPELINE_ERROR;

        void *y_ptr = source->get_plane_ptr(0);
        void *uv_ptr = source->get_plane_ptr(1);
        auto &allocator = DmaMemoryAllocator::get_instance();
        if (allocator.dmabuf_sync_start(y_ptr) != media_library_return::MEDIA_LIBRARY_SUCCESS)
            return pipeline::AppStatus::DMA_ERROR;
        if (allocator.dmabuf_sync_start(uv_ptr) != media_library_return::MEDIA_LIBRARY_SUCCESS)
        {
            allocator.dmabuf_sync_end(y_ptr);
            return pipeline::AppStatus::DMA_ERROR;
        }

        const int width = static_cast<int>(source->owner->get_width());
        const int height = static_cast<int>(source->owner->get_height());
        cv::Mat y_plane(height, width, CV_8UC1, y_ptr, source->get_plane_stride(0));
        cv::Mat uv_plane(height / 2, width / 2, CV_8UC2, uv_ptr, source->get_plane_stride(1));
        const size_t processed_faces = apply_to_faces(data->get_roi(), y_plane, uv_plane, width, height);

        const auto uv_sync = allocator.dmabuf_sync_end(uv_ptr);
        const auto y_sync = allocator.dmabuf_sync_end(y_ptr);
        if (uv_sync != media_library_return::MEDIA_LIBRARY_SUCCESS ||
            y_sync != media_library_return::MEDIA_LIBRARY_SUCCESS)
            return pipeline::AppStatus::DMA_ERROR;

        if (processed_faces > 0 && !m_reported_active)
        {
            std::cout << "[face-effect] " << face_effect_name(m_effect) << " strength " << m_strength
                      << " active on tracked face ROI(s)" << std::endl;
            m_reported_active = true;
        }
        send_to_subscribers(data);
        return pipeline::AppStatus::SUCCESS;
    }

  private:
    static int odd_kernel_at_most(int requested, int available)
    {
        int kernel = std::min(requested, available);
        if ((kernel & 1) == 0)
            --kernel;
        return kernel;
    }

    static void gaussian_rect(cv::Mat &image, const cv::Rect &rect, int requested_kernel)
    {
        const int kernel = odd_kernel_at_most(requested_kernel, std::min(rect.width, rect.height));
        if (kernel >= 3)
            cv::GaussianBlur(image(rect), image(rect), cv::Size(kernel, kernel), 0.0, 0.0, cv::BORDER_REPLICATE);
    }

    static void pixelate_rect(cv::Mat &image, const cv::Rect &rect, int block_size)
    {
        cv::Mat target = image(rect);
        const cv::Size reduced(std::max(1, target.cols / block_size), std::max(1, target.rows / block_size));
        cv::Mat pixels;
        cv::resize(target, pixels, reduced, 0.0, 0.0, cv::INTER_AREA);
        cv::resize(pixels, target, target.size(), 0.0, 0.0, cv::INTER_NEAREST);
    }

    size_t apply_to_faces(const HailoROIPtr &roi, cv::Mat &y_plane, cv::Mat &uv_plane,
                          int frame_width, int frame_height) const
    {
        size_t count = 0;
        for (const auto &detection : hailo_common::get_hailo_detections(roi))
        {
            if (detection->get_label() != "face")
            {
                count += apply_to_faces(detection, y_plane, uv_plane, frame_width, frame_height);
                continue;
            }

            const HailoBBox roi_bbox =
                hailo_common::create_flattened_bbox(roi->get_bbox(), roi->get_scaling_bbox());
            const HailoBBox bbox = detection->get_bbox();
            int x0 = static_cast<int>(((bbox.xmin() * roi_bbox.width()) + roi_bbox.xmin()) * frame_width);
            int y0 = static_cast<int>(((bbox.ymin() * roi_bbox.height()) + roi_bbox.ymin()) * frame_height);
            int x1 = static_cast<int>(((bbox.xmax() * roi_bbox.width()) + roi_bbox.xmin()) * frame_width);
            int y1 = static_cast<int>(((bbox.ymax() * roi_bbox.height()) + roi_bbox.ymin()) * frame_height);

            const int margin_x = std::max(2, (x1 - x0) / 10);
            const int margin_y = std::max(2, (y1 - y0) / 10);
            x0 = std::clamp(x0 - margin_x, 0, frame_width);
            y0 = std::clamp(y0 - margin_y, 0, frame_height);
            x1 = std::clamp(x1 + margin_x, 0, frame_width);
            y1 = std::clamp(y1 + margin_y, 0, frame_height);
            if (x1 - x0 < 3 || y1 - y0 < 3)
                continue;

            const cv::Rect y_rect(x0, y0, x1 - x0, y1 - y0);
            const int uv_x0 = x0 / 2;
            const int uv_y0 = y0 / 2;
            const int uv_x1 = std::min((x1 + 1) / 2, uv_plane.cols);
            const int uv_y1 = std::min((y1 + 1) / 2, uv_plane.rows);
            const bool valid_uv = uv_x1 - uv_x0 >= 3 && uv_y1 - uv_y0 >= 3;
            const cv::Rect uv_rect(uv_x0, uv_y0, std::max(0, uv_x1 - uv_x0), std::max(0, uv_y1 - uv_y0));

            static constexpr int BOX_KERNEL[5] = {5, 9, 13, 19, 25};
            static constexpr int GAUSSIAN_CAP[5] = {15, 31, 51, 71, 91};
            static constexpr int PIXEL_BLOCK[5] = {6, 9, 12, 18, 24};
            const int strength_index = m_strength - 1;
            if (m_effect == FaceEffect::HAILO_BOX)
            {
                const int kernel = BOX_KERNEL[strength_index];
                cv::blur(y_plane(y_rect), y_plane(y_rect), cv::Size(kernel, kernel), cv::Point(-1, -1),
                         cv::BORDER_REPLICATE);
            }
            else if (m_effect == FaceEffect::GAUSSIAN)
            {
                int kernel = std::clamp(std::min(y_rect.width, y_rect.height) / 3, 9,
                                        GAUSSIAN_CAP[strength_index]);
                if ((kernel & 1) == 0)
                    ++kernel;
                gaussian_rect(y_plane, y_rect, kernel);
                if (valid_uv)
                    gaussian_rect(uv_plane, uv_rect, std::max(3, kernel / 2));
            }
            else if (m_effect == FaceEffect::PIXELATION)
            {
                const int block = PIXEL_BLOCK[strength_index];
                pixelate_rect(y_plane, y_rect, block);
                if (valid_uv)
                    pixelate_rect(uv_plane, uv_rect, std::max(3, block / 2));
            }
            ++count;
        }
        return count;
    }

    FaceEffect m_effect;
    int m_strength;
    bool m_reported_active = false;
};

class ContiguousNv12GstSinkStage : public pipeline::ThreadedStage
{
  public:
    ContiguousNv12GstSinkStage(std::string name, GstElement *appsrc, int framerate)
        : pipeline::ThreadedStage(std::move(name), 2, true, true), m_appsrc(GST_APP_SRC(appsrc)),
          m_framerate(framerate)
    {
    }

    pipeline::AppStatus process(pipeline::BufferPtr data) override
    {
        HailoMediaLibraryBufferPtr source = data->get_buffer();
        if (!source || source->get_num_of_planes() < 2)
        {
            std::cerr << "[analytics] invalid NV12 buffer" << std::endl;
            return pipeline::AppStatus::PIPELINE_ERROR;
        }

        const guint width = source->owner->get_width();
        const guint height = source->owner->get_height();
        const gsize y_size = source->get_plane_size(0);
        const gsize uv_size = source->get_plane_size(1);
        const gsize total_size = y_size + uv_size;

        if (!m_caps_set)
        {
            GstCaps *caps = gst_caps_new_simple(
                "video/x-raw", "format", G_TYPE_STRING, "NV12", "width", G_TYPE_INT, width,
                "height", G_TYPE_INT, height, "framerate", GST_TYPE_FRACTION, m_framerate, 1,
                "pixel-aspect-ratio", GST_TYPE_FRACTION, 11, 10, NULL);
            gst_app_src_set_caps(m_appsrc, caps);
            gst_caps_unref(caps);
            m_caps_set = true;
            std::cout << "[analytics] output caps " << width << "x" << height
                      << " NV12, Y=" << y_size << " UV=" << uv_size
                      << " strides=" << source->get_plane_stride(0) << "/" << source->get_plane_stride(1)
                      << std::endl;
        }

        GstBuffer *buffer = gst_buffer_new_allocate(nullptr, total_size, nullptr);
        if (!buffer)
            return pipeline::AppStatus::PIPELINE_ERROR;

        GstMapInfo map{};
        if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE))
        {
            gst_buffer_unref(buffer);
            return pipeline::AppStatus::PIPELINE_ERROR;
        }
        std::memcpy(map.data, source->get_plane_ptr(0), y_size);
        std::memcpy(map.data + y_size, source->get_plane_ptr(1), uv_size);
        gst_buffer_unmap(buffer, &map);

        gsize offsets[GST_VIDEO_MAX_PLANES]{};
        gint strides[GST_VIDEO_MAX_PLANES]{};
        offsets[0] = 0;
        offsets[1] = y_size;
        strides[0] = static_cast<gint>(source->get_plane_stride(0));
        strides[1] = static_cast<gint>(source->get_plane_stride(1));
        gst_buffer_add_video_meta_full(buffer, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_NV12,
                                       width, height, 2, offsets, strides);

        const GstFlowReturn result = gst_app_src_push_buffer(m_appsrc, buffer);
        if (result != GST_FLOW_OK)
        {
            std::cerr << "[analytics] appsrc push failed: " << static_cast<int>(result) << std::endl;
            return pipeline::AppStatus::PIPELINE_ERROR;
        }

        return pipeline::AppStatus::SUCCESS;
    }

  private:
    GstAppSrc *m_appsrc;
    int m_framerate;
    bool m_caps_set = false;
};

static pipeline::PipelinePtr build_analytics_pipeline(GstElement *appsink, GstElement *appsrc, const AppConfig &config)
{
    auto source = pipeline::sources::GstSourceStageBuild::create().set_stage_name("gst_source").buildptr();
    source->add_appsink(STREAM_ID, appsink);

    hailo_analytics::analytics::tiling::tiling_detection_config_t detection_config;
    hailo_analytics::analytics::ai_models::apply_to(
        hailo_analytics::analytics::ai_models::YOLOV8N, detection_config.detection_config);
    detection_config.tiling_config.crop_every_x_frames = static_cast<size_t>(config.inference_interval);
    detection_config.tracker_config.enabled = true;
    detection_config.tracker_config.enable_kalman_filter = true;
    detection_config.tracker_config.labels_map = {
        {1, "person"}, {2, "vehicle"}, {3, "face"}, {4, "license_plate"}};
    auto detection_result = hailo_analytics::analytics::tiling::generate_tiling_detection_pipeline(
        TILING_PIPELINE, detection_config);
    if (!detection_result.has_value())
    {
        std::cerr << "Failed to create YOLOv8n tiling pipeline" << std::endl;
        return nullptr;
    }

    auto overlay = pipeline::overlay::OverlayStageBuild::create()
                       .set_stage_name("overlay_stage")
                       .set_queue_size(3)
                       .buildptr();
    auto dsi_output = std::make_shared<ContiguousNv12GstSinkStage>("dsi_output", appsrc, config.framerate);

    pipeline::PipelineBuilder builder;
    builder.add_stage(source, pipeline::StageType::SOURCE)
        .add_stage(detection_result.value())
        .add_stage(overlay)
        .add_stage(dsi_output, pipeline::StageType::SINK)
        .connect_frontend("gst_source", STREAM_ID, TILING_PIPELINE);

    if (config.face_effect != FaceEffect::NONE)
    {
        auto face_effect =
            std::make_shared<FaceEffectStage>("face_effect_stage", config.face_effect, config.privacy_strength);
        builder.add_stage(face_effect)
            .connect(TILING_PIPELINE, "face_effect_stage")
            .connect("face_effect_stage", "overlay_stage");
    }
    else
    {
        builder.connect(TILING_PIPELINE, "overlay_stage");
    }
    return builder.connect("overlay_stage", "dsi_output").build("astrial_detection_dsi_pipeline");
}

static bool start_pipelines(InputPipeline &input, OutputPipeline &output, pipeline::PipelinePtr &analytics)
{
    std::cout << "Starting direct DSI output pipeline..." << std::endl;
    if (gst_element_set_state(output.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
        return false;

    std::cout << "Starting YOLOv8n analytics pipeline..." << std::endl;
    if (analytics->start() != pipeline::AppStatus::SUCCESS)
    {
        gst_element_set_state(output.pipeline, GST_STATE_NULL);
        return false;
    }

    std::cout << "Starting camera frontend from dsi_demo configuration..." << std::endl;
    if (gst_element_set_state(input.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    {
        analytics->stop();
        gst_element_set_state(output.pipeline, GST_STATE_NULL);
        return false;
    }
    return true;
}

static void stop_and_cleanup(InputPipeline &input, OutputPipeline &output, pipeline::PipelinePtr &analytics)
{
    std::cout << "Stopping camera..." << std::endl;
    gst_element_set_state(input.pipeline, GST_STATE_NULL);
    std::cout << "Stopping analytics..." << std::endl;
    analytics->stop();
    std::cout << "Stopping DSI output..." << std::endl;
    gst_element_set_state(output.pipeline, GST_STATE_NULL);
    gst_object_unref(input.pipeline);
    gst_object_unref(output.pipeline);
}

int main(int argc, char **argv)
{
    const bool wants_help = std::any_of(argv + 1, argv + argc, [](const char *argument) {
        return std::strcmp(argument, "-h") == 0 || std::strcmp(argument, "--help") == 0;
    });
    if (!wants_help && !std::getenv("SYSELE_LAUNCHED"))
    {
        std::cerr << "dsi_detection_app: this is the executable behind the dsi_detection command, which\n"
                     "                   prepares the camera and keeps the logs in logs/ next to it.\n"
                     "                   Start it with ./run from this directory, or with dsi_detection.\n"
                     "                   To start it anyway: SYSELE_LAUNCHED=1 ./dsi_detection_app\n";
        return 2;
    }

    AppConfig config{};
    bool help_requested = false;
    if (!parse_arguments(argc, argv, config, help_requested))
        return help_requested ? 0 : 1;

    signal_utils::SignalHandler signal_handler(false);
    signal_handler.register_signal_handler([]([[maybe_unused]] int signal) {
        std::cout << "Stop requested" << std::endl;
        request_stop();
    });

    gst_init(&argc, &argv);

    InputPipeline input;
    OutputPipeline output;
    if (!build_input_pipeline(config, input) || !build_output_pipeline(config, output))
        return 1;

    add_bus_handler(input.pipeline);
    add_bus_handler(output.pipeline);

    auto analytics = build_analytics_pipeline(input.appsink, output.appsrc, config);
    if (!analytics)
        return 1;

    if (!start_pipelines(input, output, analytics))
    {
        std::cerr << "Failed to start the direct DSI demo" << std::endl;
        gst_object_unref(input.pipeline);
        gst_object_unref(output.pipeline);
        return 1;
    }

    std::cout << "Running detection on DSI at " << config.framerate << " fps, inference every "
              << config.inference_interval << " frames, face effect="
              << face_effect_name(config.face_effect) << ", privacy strength=" << config.privacy_strength
              << ", on-screen FPS=" << (config.show_fps ? "on" : "off") << ", "
              << (config.timeout == std::numeric_limits<int>::max()
                      ? std::string("until stopped")
                      : "for " + std::to_string(config.timeout) + " seconds")
              << std::endl;
    wait_for_stop_or_timeout(config.timeout);
    stop_and_cleanup(input, output, analytics);

    // Hailo 1.12's reference application aborts while destroying its retained
    // GstSourceStage/appsink graph (a joinable internal thread reaches static
    // teardown). All pipeline threads and GStreamer elements are already
    // stopped above, so exit without invoking that faulty library destructor.
    const int exit_code = g_gstreamer_error ? 1 : 0;
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(exit_code);
}
