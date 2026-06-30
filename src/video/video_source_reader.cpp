#include "video/video_source_reader.hpp"

#include "common/utils.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <iostream>
#include <stdexcept>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#ifdef _WIN32
#include <libavdevice/avdevice.h>
#endif
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace {

std::string ffmpeg_error(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

void check_ffmpeg(int result, const char *operation) {
    if (result < 0) {
        throw std::runtime_error(std::string(operation) + " failed: " +
                                 ffmpeg_error(result));
    }
}

#ifndef _WIN32
int camera_ioctl(int fd, unsigned long request, void *argument) {
    int result = 0;
    do {
        result = ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);
    return result;
}

void check_camera(int result, const std::string &operation) {
    if (result < 0) {
        throw std::runtime_error(
            operation + " failed: " + std::strerror(errno));
    }
}
#endif

} // namespace

VideoSourceReader::VideoSourceReader(SenderConfig config)
    : config_(std::move(config)) {
    open();
}

VideoSourceReader::~VideoSourceReader() {
    close();
}

bool VideoSourceReader::next_frame(EncodedVideoFrame &frame,
                                   const VideoProfile &profile,
                                   bool force_keyframe) {
    if (!encoder_configured_ || encoder_profile_ != profile) {
        configure_encoder(profile);
        force_keyframe = true;
    }

    while (next_decoded_frame()) {
        if (!should_output_decoded_frame(profile.fps)) {
            if (config_.input == SenderInput::File) {
                av_frame_unref(decoded_frame_);
            }
            continue;
        }
        const int64_t source_ready_monotonic_us = monotonic_us();

        sws_context_ = sws_getCachedContext(
            sws_context_,
            decoded_frame_->width,
            decoded_frame_->height,
            static_cast<AVPixelFormat>(decoded_frame_->format),
            profile.width,
            profile.height,
            AV_PIX_FMT_YUV420P,
            SWS_BILINEAR,
            nullptr,
            nullptr,
            nullptr);
        if (!sws_context_) {
            throw std::runtime_error("sws_getCachedContext failed");
        }

        check_ffmpeg(av_frame_make_writable(scaled_frame_),
                     "av_frame_make_writable");
        sws_scale(sws_context_,
                  decoded_frame_->data,
                  decoded_frame_->linesize,
                  0,
                  decoded_frame_->height,
                  scaled_frame_->data,
                  scaled_frame_->linesize);
        if (config_.input == SenderInput::File) {
            av_frame_unref(decoded_frame_);
        }

        scaled_frame_->pts = encoder_pts_++;
        scaled_frame_->pict_type =
            force_keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;

        check_ffmpeg(avcodec_send_frame(encoder_context_, scaled_frame_),
                     "avcodec_send_frame");

        AVPacket *packet = av_packet_alloc();
        if (!packet) {
            throw std::runtime_error("av_packet_alloc failed");
        }
        const int receive_result =
            avcodec_receive_packet(encoder_context_, packet);
        if (receive_result == AVERROR(EAGAIN)) {
            av_packet_free(&packet);
            continue;
        }
        if (receive_result < 0) {
            const std::string error = ffmpeg_error(receive_result);
            av_packet_free(&packet);
            throw std::runtime_error("avcodec_receive_packet failed: " + error);
        }

        frame.data.resize(static_cast<std::size_t>(packet->size));
        std::memcpy(frame.data.data(), packet->data,
                    static_cast<std::size_t>(packet->size));
        frame.duration_90khz =
            static_cast<uint32_t>(std::max(1, 90000 / profile.fps));
        frame.encoder_epoch = epoch_;
        frame.keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
        frame.encoded_at_unix_us = unix_time_us();
        const int64_t encoded_monotonic_us = monotonic_us();
        const uint64_t source_to_encoded_us =
            encoded_monotonic_us >= source_ready_monotonic_us
                ? static_cast<uint64_t>(
                      encoded_monotonic_us - source_ready_monotonic_us)
                : 0;
        frame.source_to_encoded_us = static_cast<uint32_t>(
            std::min<uint64_t>(
                source_to_encoded_us,
                std::numeric_limits<uint32_t>::max()));
        av_packet_free(&packet);
        return true;
    }
    return false;
}

void VideoSourceReader::reset() {
    close();
    open();
}

bool VideoSourceReader::is_live() const {
    return config_.input == SenderInput::Camera;
}

void VideoSourceReader::open() {
    if (config_.input == SenderInput::Camera) {
        open_camera();
    } else {
        open_file();
    }
}

void VideoSourceReader::open_file() {
    int result =
        avformat_open_input(&format_context_, config_.video_file.c_str(),
                            nullptr, nullptr);
    if (result < 0) {
        throw std::runtime_error(
            "cannot open video file '" + config_.video_file + "': " +
                                 ffmpeg_error(result));
    }

    check_ffmpeg(avformat_find_stream_info(format_context_, nullptr),
                 "avformat_find_stream_info");

    video_stream_index_ =
        av_find_best_stream(format_context_, AVMEDIA_TYPE_VIDEO,
                            -1, -1, nullptr, 0);
    if (video_stream_index_ < 0) {
        throw std::runtime_error("video file has no video stream");
    }

    AVStream *stream = format_context_->streams[video_stream_index_];
    stream_time_base_num_ = stream->time_base.num;
    stream_time_base_den_ = stream->time_base.den;

    const AVCodec *decoder =
        avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) {
        throw std::runtime_error("input video decoder not found");
    }
    decoder_context_ = avcodec_alloc_context3(decoder);
    if (!decoder_context_) {
        throw std::runtime_error("avcodec_alloc_context3 decoder failed");
    }
    check_ffmpeg(avcodec_parameters_to_context(
                     decoder_context_, stream->codecpar),
                 "avcodec_parameters_to_context");
    decoder_context_->thread_count = 2;
    check_ffmpeg(avcodec_open2(decoder_context_, decoder, nullptr),
                 "avcodec_open2 decoder");

    decoded_frame_ = av_frame_alloc();
    if (!decoded_frame_) {
        throw std::runtime_error("av_frame_alloc decoder failed");
    }

    input_eof_ = false;
    decoder_flushed_ = false;
    next_output_source_seconds_ = -1.0;
    std::cerr << "video_input=file path=" << config_.video_file
              << std::endl;
}

void VideoSourceReader::open_camera() {
#ifdef _WIN32
    avdevice_register_all();

    const AVInputFormat *input_format = av_find_input_format("dshow");
    if (!input_format) {
        throw std::runtime_error("FFmpeg DirectShow input is not available");
    }

    const std::string device = "video=" + config_.camera_device;
    AVDictionary *options = nullptr;
    av_dict_set(&options, "video_size",
                (std::to_string(config_.camera_width) + "x" +
                 std::to_string(config_.camera_height)).c_str(),
                0);
    av_dict_set(&options, "framerate",
                std::to_string(config_.camera_fps).c_str(), 0);

    int result = avformat_open_input(&format_context_, device.c_str(),
                                     input_format, &options);
    av_dict_free(&options);
    if (result < 0) {
        throw std::runtime_error(
            "cannot open DirectShow camera '" + config_.camera_device +
            "': " + ffmpeg_error(result) +
            ". List devices with: ffmpeg -list_devices true -f dshow -i dummy");
    }

    check_ffmpeg(avformat_find_stream_info(format_context_, nullptr),
                 "avformat_find_stream_info camera");

    video_stream_index_ =
        av_find_best_stream(format_context_, AVMEDIA_TYPE_VIDEO,
                            -1, -1, nullptr, 0);
    if (video_stream_index_ < 0) {
        throw std::runtime_error("DirectShow camera has no video stream");
    }

    AVStream *stream = format_context_->streams[video_stream_index_];
    stream_time_base_num_ = stream->time_base.num;
    stream_time_base_den_ = stream->time_base.den;

    const AVCodec *decoder =
        avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) {
        throw std::runtime_error("camera video decoder not found");
    }
    decoder_context_ = avcodec_alloc_context3(decoder);
    if (!decoder_context_) {
        throw std::runtime_error("avcodec_alloc_context3 camera decoder failed");
    }
    check_ffmpeg(avcodec_parameters_to_context(
                     decoder_context_, stream->codecpar),
                 "avcodec_parameters_to_context camera");
    decoder_context_->thread_count = 2;
    check_ffmpeg(avcodec_open2(decoder_context_, decoder, nullptr),
                 "avcodec_open2 camera decoder");

    decoded_frame_ = av_frame_alloc();
    if (!decoded_frame_) {
        throw std::runtime_error("av_frame_alloc camera failed");
    }

    input_eof_ = false;
    decoder_flushed_ = false;
    camera_streaming_ = true;
    next_output_source_seconds_ = -1.0;
    std::cerr << "video_input=camera backend=dshow device="
              << config_.camera_device
              << " requested_resolution="
              << config_.camera_width << "x"
              << config_.camera_height
              << " requested_fps=" << config_.camera_fps
              << std::endl;
#else
    camera_fd_ = ::open(config_.camera_device.c_str(),
                        O_RDWR | O_NONBLOCK);
    if (camera_fd_ < 0) {
        throw std::runtime_error(
            "cannot open camera '" + config_.camera_device + "': " +
            std::strerror(errno));
    }

    try {
        v4l2_capability capability{};
        check_camera(camera_ioctl(camera_fd_, VIDIOC_QUERYCAP, &capability),
                     "VIDIOC_QUERYCAP");
        if ((capability.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0 ||
            (capability.capabilities & V4L2_CAP_STREAMING) == 0) {
            throw std::runtime_error(
                "camera does not support streaming video capture");
        }

        v4l2_format format{};
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.fmt.pix.width =
            static_cast<uint32_t>(config_.camera_width);
        format.fmt.pix.height =
            static_cast<uint32_t>(config_.camera_height);
        format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        format.fmt.pix.field = V4L2_FIELD_ANY;
        check_camera(camera_ioctl(camera_fd_, VIDIOC_S_FMT, &format),
                     "VIDIOC_S_FMT");
        if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
            throw std::runtime_error(
                "camera did not accept the yuyv422 pixel format");
        }
        config_.camera_width = static_cast<int>(format.fmt.pix.width);
        config_.camera_height = static_cast<int>(format.fmt.pix.height);
        camera_bytes_per_line_ = static_cast<int>(
            format.fmt.pix.bytesperline != 0
                ? format.fmt.pix.bytesperline
                : format.fmt.pix.width * 2U);

        v4l2_streamparm parameters{};
        parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parameters.parm.capture.timeperframe.numerator = 1;
        parameters.parm.capture.timeperframe.denominator =
            static_cast<uint32_t>(config_.camera_fps);
        check_camera(camera_ioctl(camera_fd_, VIDIOC_S_PARM, &parameters),
                     "VIDIOC_S_PARM");
        const auto &time_per_frame =
            parameters.parm.capture.timeperframe;
        if (time_per_frame.numerator != 0) {
            config_.camera_fps = static_cast<int>(
                time_per_frame.denominator /
                time_per_frame.numerator);
        }

        v4l2_requestbuffers request{};
        request.count = 4;
        request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        request.memory = V4L2_MEMORY_MMAP;
        check_camera(camera_ioctl(camera_fd_, VIDIOC_REQBUFS, &request),
                     "VIDIOC_REQBUFS");
        if (request.count < 2) {
            throw std::runtime_error(
                "camera returned too few streaming buffers");
        }

        camera_buffers_.resize(request.count);
        for (uint32_t index = 0; index < request.count; ++index) {
            v4l2_buffer buffer{};
            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            buffer.index = index;
            check_camera(camera_ioctl(camera_fd_, VIDIOC_QUERYBUF, &buffer),
                         "VIDIOC_QUERYBUF");
            void *data = mmap(nullptr, buffer.length,
                              PROT_READ | PROT_WRITE, MAP_SHARED,
                              camera_fd_, buffer.m.offset);
            if (data == MAP_FAILED) {
                throw std::runtime_error(
                    "camera mmap failed: " +
                    std::string(std::strerror(errno)));
            }
            camera_buffers_[index] = {data, buffer.length};
            check_camera(camera_ioctl(camera_fd_, VIDIOC_QBUF, &buffer),
                         "VIDIOC_QBUF");
        }

        decoded_frame_ = av_frame_alloc();
        if (!decoded_frame_) {
            throw std::runtime_error("av_frame_alloc camera failed");
        }
        decoded_frame_->format = AV_PIX_FMT_YUYV422;
        decoded_frame_->width = config_.camera_width;
        decoded_frame_->height = config_.camera_height;
        check_ffmpeg(av_frame_get_buffer(decoded_frame_, 32),
                     "av_frame_get_buffer camera");

        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        check_camera(camera_ioctl(camera_fd_, VIDIOC_STREAMON, &type),
                     "VIDIOC_STREAMON");
        camera_streaming_ = true;
        next_output_source_seconds_ = -1.0;
        std::cerr << "video_input=camera device="
                  << config_.camera_device
                  << " format=yuyv422 resolution="
                  << config_.camera_width << "x"
                  << config_.camera_height
                  << " fps=" << config_.camera_fps
                  << std::endl;
    } catch (...) {
        close_camera();
        av_frame_free(&decoded_frame_);
        throw;
    }
#endif
}

void VideoSourceReader::close() {
    close_encoder();
    sws_freeContext(sws_context_);
    sws_context_ = nullptr;
    av_frame_free(&decoded_frame_);
    avcodec_free_context(&decoder_context_);
    if (format_context_) {
        avformat_close_input(&format_context_);
    }
    close_camera();
    video_stream_index_ = -1;
    input_eof_ = false;
    decoder_flushed_ = false;
    next_output_source_seconds_ = -1.0;
}

void VideoSourceReader::close_camera() {
#ifndef _WIN32
    if (camera_streaming_ && camera_fd_ >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        camera_ioctl(camera_fd_, VIDIOC_STREAMOFF, &type);
    }
    camera_streaming_ = false;
    for (const auto &buffer : camera_buffers_) {
        if (buffer.data && buffer.data != MAP_FAILED) {
            munmap(buffer.data, buffer.length);
        }
    }
    camera_buffers_.clear();
    if (camera_fd_ >= 0) {
        ::close(camera_fd_);
        camera_fd_ = -1;
    }
#else
    camera_streaming_ = false;
#endif
}

void VideoSourceReader::close_encoder() {
    av_frame_free(&scaled_frame_);
    avcodec_free_context(&encoder_context_);
    encoder_configured_ = false;
}

void VideoSourceReader::configure_encoder(const VideoProfile &profile) {
    close_encoder();

    const AVCodec *encoder = avcodec_find_encoder_by_name("libx264");
    if (!encoder) {
        throw std::runtime_error("FFmpeg libx264 encoder not found");
    }

    encoder_context_ = avcodec_alloc_context3(encoder);
    if (!encoder_context_) {
        throw std::runtime_error("avcodec_alloc_context3 encoder failed");
    }

    encoder_context_->codec_type = AVMEDIA_TYPE_VIDEO;
    encoder_context_->codec_id = AV_CODEC_ID_H264;
    encoder_context_->width = profile.width;
    encoder_context_->height = profile.height;
    encoder_context_->pix_fmt = AV_PIX_FMT_YUV420P;
    encoder_context_->time_base = AVRational{1, profile.fps};
    encoder_context_->framerate = AVRational{profile.fps, 1};
    encoder_context_->bit_rate =
        static_cast<int64_t>(profile.bitrate_kbps) * 950;
    encoder_context_->rc_max_rate =
        static_cast<int64_t>(profile.bitrate_kbps) * 1000;
    encoder_context_->rc_buffer_size =
        static_cast<int>(encoder_context_->bit_rate / 2);
    const int keyint =
        profile.all_intra ? 1 : std::max(1, profile.gop_frames);
    encoder_context_->gop_size = keyint;
    encoder_context_->max_b_frames = 0;
    encoder_context_->refs = 1;
    encoder_context_->thread_count = 2;

    av_opt_set(encoder_context_->priv_data, "preset", "veryfast", 0);
    av_opt_set(encoder_context_->priv_data, "tune", "zerolatency", 0);
    av_opt_set(encoder_context_->priv_data, "profile", "baseline", 0);

    const std::string x264_params =
        "repeat-headers=1:scenecut=0:keyint=" +
        std::to_string(keyint) +
        ":min-keyint=" + std::to_string(keyint) +
        ":bframes=0:ref=1:force-cfr=1";
    av_opt_set(encoder_context_->priv_data, "x264-params",
               x264_params.c_str(), 0);

    check_ffmpeg(avcodec_open2(encoder_context_, encoder, nullptr),
                 "avcodec_open2 encoder");

    scaled_frame_ = av_frame_alloc();
    if (!scaled_frame_) {
        throw std::runtime_error("av_frame_alloc encoder failed");
    }
    scaled_frame_->format = AV_PIX_FMT_YUV420P;
    scaled_frame_->width = profile.width;
    scaled_frame_->height = profile.height;
    check_ffmpeg(av_frame_get_buffer(scaled_frame_, 32),
                 "av_frame_get_buffer");

    encoder_profile_ = profile;
    encoder_configured_ = true;
    encoder_pts_ = 0;
    next_output_source_seconds_ = -1.0;
    ++epoch_;
    if (epoch_ == 0) {
        epoch_ = 1;
    }
}

bool VideoSourceReader::next_decoded_frame() {
    return config_.input == SenderInput::Camera
        ? next_camera_frame()
        : next_file_frame();
}

bool VideoSourceReader::next_file_frame() {
    while (true) {
        av_frame_unref(decoded_frame_);
        const int receive_result =
            avcodec_receive_frame(decoder_context_, decoded_frame_);
        if (receive_result == 0) {
            return true;
        }
        if (receive_result == AVERROR_EOF) {
            return false;
        }
        if (receive_result != AVERROR(EAGAIN)) {
            throw std::runtime_error("avcodec_receive_frame failed: " +
                                     ffmpeg_error(receive_result));
        }

        if (input_eof_) {
            if (!decoder_flushed_) {
                check_ffmpeg(avcodec_send_packet(decoder_context_, nullptr),
                             "avcodec_send_packet flush");
                decoder_flushed_ = true;
                continue;
            }
            return false;
        }

        AVPacket *packet = av_packet_alloc();
        if (!packet) {
            throw std::runtime_error("av_packet_alloc failed");
        }
        int read_result = 0;
        do {
            read_result = av_read_frame(format_context_, packet);
        } while (read_result >= 0 &&
                 packet->stream_index != video_stream_index_ &&
                 (av_packet_unref(packet), true));

        if (read_result < 0) {
            input_eof_ = true;
            av_packet_free(&packet);
            continue;
        }

        const int send_result =
            avcodec_send_packet(decoder_context_, packet);
        av_packet_free(&packet);
        if (send_result < 0 && send_result != AVERROR(EAGAIN)) {
            throw std::runtime_error("avcodec_send_packet failed: " +
                                     ffmpeg_error(send_result));
        }
    }
}
bool VideoSourceReader::next_camera_frame() {
#ifdef _WIN32
    return next_file_frame();
#else
    while (!g_stop_requested.load()) {
        pollfd descriptor{};
        descriptor.fd = camera_fd_;
        descriptor.events = POLLIN;
        const int poll_result = poll(&descriptor, 1, 500);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(
                "camera poll failed: " +
                std::string(std::strerror(errno)));
        }
        if (poll_result == 0) {
            continue;
        }

        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (camera_ioctl(camera_fd_, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN) {
                continue;
            }
            check_camera(-1, "VIDIOC_DQBUF");
        }
        if (buffer.index >= camera_buffers_.size()) {
            throw std::runtime_error(
                "camera returned an invalid buffer index");
        }

        check_ffmpeg(av_frame_make_writable(decoded_frame_),
                     "av_frame_make_writable camera");
        const auto *source = static_cast<const uint8_t *>(
            camera_buffers_[buffer.index].data);
        const int row_bytes = config_.camera_width * 2;
        const std::size_t required = static_cast<std::size_t>(
            camera_bytes_per_line_) * config_.camera_height;
        if (buffer.bytesused < required) {
            camera_ioctl(camera_fd_, VIDIOC_QBUF, &buffer);
            throw std::runtime_error(
                "camera returned a truncated frame");
        }
        for (int row = 0; row < config_.camera_height; ++row) {
            std::memcpy(
                decoded_frame_->data[0] +
                    row * decoded_frame_->linesize[0],
                source + row * camera_bytes_per_line_,
                static_cast<std::size_t>(row_bytes));
        }
        check_camera(camera_ioctl(camera_fd_, VIDIOC_QBUF, &buffer),
                     "VIDIOC_QBUF");
        decoded_frame_->pts = monotonic_us();
        return true;
    }
    return false;
#endif
}

bool VideoSourceReader::should_output_decoded_frame(int fps) {
    double source_seconds = 0.0;
    if (config_.input == SenderInput::Camera) {
        source_seconds =
            static_cast<double>(monotonic_us()) / 1'000'000.0;
    } else if (decoded_frame_->best_effort_timestamp != AV_NOPTS_VALUE &&
        stream_time_base_den_ > 0) {
        source_seconds =
            static_cast<double>(decoded_frame_->best_effort_timestamp) *
            static_cast<double>(stream_time_base_num_) /
            static_cast<double>(stream_time_base_den_);
    } else if (next_output_source_seconds_ >= 0.0) {
        source_seconds = next_output_source_seconds_;
    }

    if (next_output_source_seconds_ < 0.0) {
        next_output_source_seconds_ = source_seconds;
    }
    if (source_seconds + 0.0001 < next_output_source_seconds_) {
        return false;
    }
    next_output_source_seconds_ += 1.0 / static_cast<double>(fps);
    return true;
}
