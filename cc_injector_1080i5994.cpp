// cc_injector_pts_stable.cpp
// 1080i59.94 CC1 injection with stable timestamps + no redraw

#include <iostream>
#include <vector>
#include <string>
#include <cstring>

// POSIX
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/frame.h>
}

// ======================================================================================
// 608 helpers
// ======================================================================================

static inline uint8_t parity(uint8_t c)
{
    c &= 0x7F;
    return (__builtin_popcount(c) & 1) ? c : (c | 0x80);
}

static inline uint8_t clean(uint8_t c)
{
    c &= 0x7F;
    return (c < 0x20) ? ' ' : c;
}

// ======================================================================================
// Caption state (SEND ONCE)
// ======================================================================================

struct CCEncoder {
    std::vector<std::pair<uint8_t,uint8_t>> current;
    size_t index = 0;
    bool initialized = false;
    bool active = false;
};

static void set_caption(CCEncoder& enc, const std::string& text)
{
    enc.current.clear();
    enc.index = 0;
    enc.active = true;

    if (!enc.initialized) {
        enc.current.emplace_back(0x14, 0x20); // RCL
        enc.current.emplace_back(0x14, 0x25); // RU2
        enc.current.emplace_back(0x11, 0x5C); // row 15
        enc.initialized = true;
    }

    enc.current.emplace_back(0x14, 0x2D); // CR

    std::string s = text.substr(0, 32);

    for (size_t i = 0; i < s.size(); i += 2) {
        uint8_t a = clean(s[i]);
        uint8_t b = (i + 1 < s.size()) ? clean(s[i+1]) : ' ';
        enc.current.emplace_back(a, b);
    }

    std::cerr << "[cc] set: " << text << "\n";
}

// ======================================================================================
// CC emission (ONE-SHOT + then padding)
// ======================================================================================

static void build_cc(std::vector<uint8_t>& out, CCEncoder& enc)
{
    out.clear();

    const int PAIRS_PER_FRAME = 3;

    for (int i = 0; i < PAIRS_PER_FRAME; i++) {

        uint8_t a = 0x80;
        uint8_t b = 0x80;

        if (enc.active && !enc.current.empty()) {

            auto& p = enc.current[enc.index];
            a = p.first;
            b = p.second;

            enc.index++;

            if (enc.index >= enc.current.size()) {
                enc.active = false; // ✅ stop after one pass
            }
        }

        uint8_t p1 = parity(a);
        uint8_t p2 = parity(b);

        // Field 1
        out.push_back(0x04);
        out.push_back(p1);
        out.push_back(p2);

        // Field 2
        out.push_back(0x05);
        out.push_back(p1);
        out.push_back(p2);
    }
}

// ======================================================================================
// UDP
// ======================================================================================

static int open_udp(int port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(fd, (sockaddr*)&addr, sizeof(addr));
    fcntl(fd, F_SETFL, O_NONBLOCK);

    std::cerr << "[cc] UDP :" << port << "\n";
    return fd;
}

static bool recv_line(int fd, std::string& out)
{
    char buf[512];
    int n = recv(fd, buf, sizeof(buf)-1, 0);
    if (n <= 0) return false;

    buf[n] = 0;
    out = std::string(buf).substr(0, 32);
    return true;
}

// ======================================================================================
// MAIN
// ======================================================================================

int main(int argc, char** argv)
{
    const char* inUrl  = (argc > 1) ? argv[1] : "udp://127.0.0.1:5000";
    const char* outUrl = (argc > 2) ? argv[2] : "output.ts";

    avformat_network_init();

    AVFormatContext* ifmt = nullptr;
    avformat_open_input(&ifmt, inUrl, nullptr, nullptr);
    avformat_find_stream_info(ifmt, nullptr);

    int vIdx = av_find_best_stream(ifmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);

    // decoder
    const AVCodec* dec = avcodec_find_decoder(ifmt->streams[vIdx]->codecpar->codec_id);
    AVCodecContext* decCtx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(decCtx, ifmt->streams[vIdx]->codecpar);
    avcodec_open2(decCtx, dec, nullptr);

    // encoder
    const AVCodec* enc = avcodec_find_encoder_by_name("libx264");
    AVCodecContext* encCtx = avcodec_alloc_context3(enc);

    AVRational fr = {30000,1001};

    encCtx->width = 1920;
    encCtx->height = 1080;
    encCtx->pix_fmt = AV_PIX_FMT_YUV420P;

    encCtx->time_base = av_inv_q(fr);
    encCtx->framerate = fr;

    encCtx->gop_size = 30;
    encCtx->max_b_frames = 0;

    encCtx->flags |= AV_CODEC_FLAG_INTERLACED_DCT | AV_CODEC_FLAG_INTERLACED_ME;
    encCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    encCtx->flags2 |= AV_CODEC_FLAG2_LOCAL_HEADER;

    encCtx->field_order = AV_FIELD_TT;

    av_opt_set(encCtx->priv_data, "a53cc", "1", 0);
    av_opt_set(encCtx->priv_data, "x264opts", "tff=1", 0);

    // ✅ critical stability options
    av_opt_set(encCtx->priv_data, "repeat-headers", "1", 0);
    av_opt_set(encCtx->priv_data, "nal-hrd", "vbr", 0);
    av_opt_set(encCtx->priv_data, "tune", "zerolatency", 0);

    avcodec_open2(encCtx, enc, nullptr);

    // muxer
    AVFormatContext* ofmt = nullptr;
    avformat_alloc_output_context2(&ofmt, nullptr, "mpegts", outUrl);

    AVStream* vout = avformat_new_stream(ofmt, enc);
    avcodec_parameters_from_context(vout->codecpar, encCtx);

    // ✅ critical: match time base
    vout->time_base = encCtx->time_base;

    avio_open(&ofmt->pb, outUrl, AVIO_FLAG_WRITE);
    avformat_write_header(ofmt, nullptr);

    AVPacket* pkt = av_packet_alloc();
    AVPacket* opkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    int udp = open_udp(54001);

    CCEncoder ccEnc{};
    std::string caption = "HELLO WORLD";
    set_caption(ccEnc, caption);

    // ✅ global PTS counter
    int64_t pts = 0;

    while (av_read_frame(ifmt, pkt) >= 0) {

        if (pkt->stream_index == vIdx) {

            avcodec_send_packet(decCtx, pkt);

            while (avcodec_receive_frame(decCtx, frame) == 0) {

                // ✅ force clean timestamps
                frame->pts = pts++;
                frame->pkt_duration = 1;

                frame->interlaced_frame = 1;
                frame->top_field_first = 1;

                std::string line;
                if (recv_line(udp, line)) {
                    caption = line;
                    set_caption(ccEnc, caption);
                }

                std::vector<uint8_t> cc;
                build_cc(cc, ccEnc);

                AVFrameSideData* sd =
                    av_frame_new_side_data(frame, AV_FRAME_DATA_A53_CC, cc.size());

                memcpy(sd->data, cc.data(), cc.size());

                avcodec_send_frame(encCtx, frame);

                while (avcodec_receive_packet(encCtx, opkt) == 0) {
                    av_packet_rescale_ts(opkt, encCtx->time_base, vout->time_base);
                    opkt->stream_index = vout->index;
                    av_interleaved_write_frame(ofmt, opkt);
                    av_packet_unref(opkt);
                }

                av_frame_unref(frame);
            }
        }

        av_packet_unref(pkt);
    }

    av_write_trailer(ofmt);
    return 0;
}