
// cc_injector.cpp
// Build (Ubuntu): g++ -std=c++17 cc_injector.cpp $(pkg-config --cflags --libs libavformat libavcodec libavutil libswresample) -o cc_injector

#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <cerrno>

// POSIX UDP socket (non-blocking)
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/frame.h>
#include <libavutil/channel_layout.h> // legacy + new API header
}

// ======================================================================================
// CEA-608 helpers (text → A/53 cc_data triplets with odd parity, Field 1)
// ======================================================================================

static inline uint8_t cea608_parity(uint8_t c7)
{
    c7 &= 0x7F;
    uint8_t msb = (__builtin_popcount((unsigned)c7) & 1) ? 0x00 : 0x80;
    return c7 | msb;
}

// A/53 cc_data triplet (3 bytes). Use 0xFC for Field 1 (valid=1), 0xFD for Field 2.
static inline void push_cc_triplet(std::vector<uint8_t>& out, uint8_t a, uint8_t b)
{
    const uint8_t header = 0xFC; // Field 1, valid=1
    out.push_back(header);
    out.push_back(cea608_parity(a));
    out.push_back(cea608_parity(b));
}
static inline void push_pair(std::vector<uint8_t>& out, uint8_t a, uint8_t b) { push_cc_triplet(out,a,b); }

// Limit to 32 and send as 608 text pairs (space padded)
static inline void push_text(std::vector<uint8_t>& out, const std::string& s)
{
    size_t len = std::min<size_t>(s.size(), 32);
    for (size_t i = 0; i < len; i += 2) {
        uint8_t c1 = (uint8_t)s[i];
        uint8_t c2 = (i + 1 < len) ? (uint8_t)s[i + 1] : (uint8_t)' ';
        push_pair(out, c1, c2);
    }
}

// PAC builder for rows 1..15 (white/no underline/indent 0)
static bool build_pac_for_row(uint8_t row, uint8_t& b1, uint8_t& b2, bool underline=false, uint8_t attr=0)
{
    static const int ccrowtab[16] = {
        11,11, 1, 2,
         3, 4,12,13,
        14,15, 5, 6,
         7, 8, 9,10
    };
    if (row < 1 || row > 15) return false;
    int idx=-1; for(int i=0;i<16;++i){ if(ccrowtab[i]==(int)row){ idx=i; break; } }
    if (idx < 0) return false;

    int row_lsb = idx & 1;
    int row_hi3 = (idx >> 1) & 7;

    b1 = (uint8_t)(0x10 | row_hi3);
    b2 = (uint8_t)(0x40 | (row_lsb<<5) | ((attr & 0x0F)<<1) | (underline?1:0));
    return true;
}

// RU2 (roll-up 2)
struct RollUp2State { bool started=false; };

// Roll-up with CR (roll) + PAC + text
static void build_ru2_update_cc(std::vector<uint8_t>& out, RollUp2State& st, const std::string& new_line)
{
    out.clear();
    push_pair(out, 0x14, 0x25);        // RU2
    if (st.started) push_pair(out, 0x14, 0x2D); // CR (roll)
    uint8_t p1=0,p2=0; if (build_pac_for_row(15,p1,p2)) push_pair(out,p1,p2);
    push_text(out, new_line);
    st.started = true;
}

// Repaint only: RU2 once, then PAC + text (no CR)
static void build_ru2_repaint_no_roll(std::vector<uint8_t>& out, RollUp2State& st, const std::string& line)
{
    out.clear();
    if (!st.started) push_pair(out, 0x14, 0x25); // RU2 on first use
    uint8_t p1=0,p2=0; if (build_pac_for_row(15,p1,p2)) push_pair(out,p1,p2);
    push_text(out, line);
    st.started = true;
}

// Pop-on (optional)
static void build_popon_cc(std::vector<uint8_t>& out, const std::string& line)
{
    out.clear();
    push_pair(out, 0x14, 0x20); // RCL
    uint8_t p1=0,p2=0; if (build_pac_for_row(15,p1,p2)) push_pair(out,p1,p2);
    push_text(out, line);
    push_pair(out, 0x14, 0x2F); // EOC
}

// ======================================================================================
// UDP caption input (non-blocking) + logging
// ======================================================================================

struct CaptionInput {
    int fd = -1;                 // UDP socket
    std::string host;
    uint16_t port = 0;
    bool enabled = false;
};

static bool set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

// Bind udp://host:port; empty host → 127.0.0.1
static bool open_udp_listener(CaptionInput& ci, const std::string& host, uint16_t port) {
    ci.fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ci.fd < 0) return false;
    int reuse=1; setsockopt(ci.fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{}; addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    std::string h = host.empty() ? std::string("127.0.0.1") : host;
    if (inet_aton(h.c_str(), &addr.sin_addr) == 0) { close(ci.fd); ci.fd=-1; return false; }

    if (bind(ci.fd, (sockaddr*)&addr, sizeof(addr)) != 0) { close(ci.fd); ci.fd=-1; return false; }
    if (!set_nonblock(ci.fd)) { close(ci.fd); ci.fd=-1; return false; }

    ci.host = h; ci.port = port; ci.enabled = true;
    std::cerr << "[cc] Listening for captions on udp://" << ci.host << ":" << ci.port << "\n";
    return true;
}

static inline void ltrim_inplace(std::string& s) {
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    if (i) s.erase(0, i);
}
static inline void rtrim_inplace(std::string& s) {
    size_t i = s.size();
    while (i > 0 && s[i-1] == ' ') --i;
    if (i < s.size()) s.erase(i);
}
static inline void trim_inplace(std::string& s) {
    ltrim_inplace(s); rtrim_inplace(s);
}

// Drain UDP and return the last non-empty line (sanitized, <=32 chars). Logs each final line.
static bool udp_get_latest_line_and_log(int fd, std::string& out_line) {
    if (fd < 0) return false;
    bool got = false;
    char buf[2048];
    for (;;) {
        sockaddr_in src{}; socklen_t slen = sizeof(src);
        ssize_t n = recvfrom(fd, buf, sizeof(buf)-1, 0, (sockaddr*)&src, &slen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            else break;
        }
        buf[n] = '\0';
        std::string s(buf, (size_t)n);

        // Normalize CR->LF, split by LF, take last non-empty segment
        for (char& ch : s) if (ch == '\r') ch = '\n';
        std::string last;
        size_t start = 0;
        while (true) {
            size_t pos = s.find('\n', start);
            std::string seg = (pos == std::string::npos) ? s.substr(start) : s.substr(start, pos - start);
            if (!seg.empty()) last = seg;
            if (pos == std::string::npos) break;
            start = pos + 1;
        }
        if (last.empty()) continue;

        // Sanitize to printable ASCII and clamp to 32
        std::string t; t.reserve(32);
        for (char c : last) {
            unsigned char uc = (unsigned char)c;
            if (uc >= 0x20 && uc <= 0x7E) t.push_back((char)uc);
            else if (uc == '\t') t.push_back(' ');
            else break; // stop at control
            if (t.size() >= 32) break;
        }
        trim_inplace(t);
        if (!t.empty()) {
            out_line = t;
            got = true;
            std::cerr << "[cc] recv: \"" << out_line << "\"\n";
        }
    }
    return got;
}

// ======================================================================================
// CLI parsing
// ======================================================================================

static bool parse_cc_udp_arg(const char* s, std::string& host, uint16_t& port) {
    if (!s) return false;
    const char* eq = std::strchr(s, '=');
    if (!eq) return false;
    std::string v(eq+1);
    auto colon = v.rfind(':');
    if (colon == std::string::npos) return false;
    host = v.substr(0, colon);
    std::string ps = v.substr(colon+1);
    int p = std::atoi(ps.c_str());
    if (p <= 0 || p > 65535) return false;
    port = (uint16_t)p;
    return true;
}

static bool parse_venc_arg(const char* s, std::string& enc_name) {
    if (!s) return false;
    const char* eq = std::strchr(s, '=');
    if (!eq) return false;
    enc_name = std::string(eq+1);
    return !enc_name.empty();
}

static bool parse_int_arg(const char* s, const char* key, int& val) {
    if (!s) return false;
    const size_t klen = std::strlen(key);
    if (std::strncmp(s, key, klen) != 0) return false;
    const char* eq = s + klen;
    if (*eq != '=') return false;
    val = std::atoi(eq+1);
    return true;
}

// ======================================================================================
// Audio layout helpers (FFmpeg version-guarded)
// ======================================================================================

static void set_audio_layout_from_decoder(AVCodecContext* aencCtx, const AVCodecContext* adecCtx) {
#if LIBAVCODEC_VERSION_MAJOR >= 59
    if (adecCtx->ch_layout.nb_channels > 0) {
        av_channel_layout_copy(&aencCtx->ch_layout, &adecCtx->ch_layout);
    } else {
        av_channel_layout_default(&aencCtx->ch_layout, 2); // stereo
    }
#else
    aencCtx->channels       = adecCtx->channels ? adecCtx->channels : 2;
    aencCtx->channel_layout = adecCtx->channel_layout ? adecCtx->channel_layout : AV_CH_LAYOUT_STEREO;
#endif
}

// ======================================================================================
// Main
// ======================================================================================

int main(int argc, char** argv)
{
    av_log_set_level(AV_LOG_ERROR);

    // Defaults so `./cc_injector` runs without args
    const char* defaultIn  = "udp://127.0.0.1:5000?timeout=5000000&fifo_size=1000000&overrun_nonfatal=1";
    const char* defaultOut = "output.ts";

    const char* inUrl  = (argc > 1) ? argv[1] : defaultIn;
    const char* outUrl = (argc > 2) ? argv[2] : defaultOut;

    // Flags
    bool use_external_udp_captions = false;
    std::string cc_host; uint16_t cc_port = 0;

    // Defaults: prefer libx264 (SEI/GA94 path), bootstrap on, linger 750ms
    std::string venc_name = "libx264";
    int bootstrap_enable = 1;
    int linger_ms = 750;

    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--cc-udp=", 9) == 0) {
            if (!parse_cc_udp_arg(argv[i], cc_host, cc_port)) {
                std::cerr << "Invalid --cc-udp format. Use --cc-udp=HOST:PORT (e.g. --cc-udp=127.0.0.1:54001)\n";
                return 1;
            }
            use_external_udp_captions = true;
        } else if (std::strncmp(argv[i], "--venc=", 7) == 0) {
            parse_venc_arg(argv[i], venc_name);
        } else if (parse_int_arg(argv[i], "--bootstrap", bootstrap_enable)) {
            // parsed
        } else if (parse_int_arg(argv[i], "--linger_ms", linger_ms)) {
            // parsed
        }
    }

    // Open input
    AVFormatContext* ifmt = nullptr;
    if (avformat_open_input(&ifmt, inUrl, nullptr, nullptr) < 0) {
        std::cerr << "open input failed: " << inUrl << "\n"; return 1;
    }
    if (avformat_find_stream_info(ifmt, nullptr) < 0) {
        std::cerr << "find_stream_info failed\n"; return 1;
    }

    int vIdx = av_find_best_stream(ifmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    int aIdx = av_find_best_stream(ifmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (vIdx < 0) { std::cerr << "no video stream found\n"; return 1; }

    // Video decoder
    const AVCodec* vdec = avcodec_find_decoder(ifmt->streams[vIdx]->codecpar->codec_id);
    if (!vdec) { std::cerr << "video decoder not found\n"; return 1; }
    AVCodecContext* vdecCtx = avcodec_alloc_context3(vdec);
    avcodec_parameters_to_context(vdecCtx, ifmt->streams[vIdx]->codecpar);
    if (avcodec_open2(vdecCtx, vdec, nullptr) < 0) { std::cerr << "open vdec failed\n"; return 1; }

    // Choose video encoder
    const AVCodec* venc = nullptr;
    if (!venc_name.empty())
        venc = avcodec_find_encoder_by_name(venc_name.c_str());
    if (!venc) {
        if (venc_name == "libx264") {
            venc = avcodec_find_encoder(AV_CODEC_ID_H264);
            if (!venc) { std::cerr << "H.264 encoder not found\n"; return 1; }
        } else if (venc_name == "mpeg2video") {
            venc = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
            if (!venc) { std::cerr << "MPEG-2 encoder not found\n"; return 1; }
        } else {
            std::cerr << "Unknown encoder: " << venc_name << "\n";
            return 1;
        }
    }

    AVCodecContext* vencCtx = avcodec_alloc_context3(venc);
    AVRational in_rate = ifmt->streams[vIdx]->r_frame_rate.num ? ifmt->streams[vIdx]->r_frame_rate
                                                               : av_make_q(30,1);

    vencCtx->width   = vdecCtx->width  ? vdecCtx->width  : 1280;
    vencCtx->height  = vdecCtx->height ? vdecCtx->height : 720;
    vencCtx->pix_fmt = vdecCtx->pix_fmt == AV_PIX_FMT_NONE ? AV_PIX_FMT_YUV420P : (AVPixelFormat)vdecCtx->pix_fmt;
    vencCtx->time_base = av_inv_q(in_rate);
    vencCtx->framerate = in_rate;
    vencCtx->gop_size  = 30;
    vencCtx->max_b_frames = 0;

    // Encourage A/53 captions in libx26x wrappers (no-op if option absent)
    av_opt_set(vencCtx->priv_data, "a53cc", "1", 0);

    if (avcodec_open2(vencCtx, venc, nullptr) < 0) { std::cerr << "open venc failed\n"; return 1; }

    // Output muxer (MPEG-TS)
    AVFormatContext* ofmt = nullptr;
    if (avformat_alloc_output_context2(&ofmt, nullptr, "mpegts", outUrl) < 0 || !ofmt) {
        std::cerr << "alloc output failed\n"; return 1;
    }
    AVStream* vout = avformat_new_stream(ofmt, venc);
    if (!vout) { std::cerr << "new vout failed\n"; return 1; }
    if (avcodec_parameters_from_context(vout->codecpar, vencCtx) < 0) { std::cerr << "copy v params failed\n"; return 1; }
    vout->time_base = vencCtx->time_base;

    // Optional audio: decode -> encode AAC -> mux
    AVCodecContext* adecCtx = nullptr;
    AVCodecContext* aencCtx = nullptr;
    AVStream*       aout    = nullptr;

    if (aIdx >= 0) {
        const AVCodec* adec = avcodec_find_decoder(ifmt->streams[aIdx]->codecpar->codec_id);
        if (adec) {
            adecCtx = avcodec_alloc_context3(adec);
            avcodec_parameters_to_context(adecCtx, ifmt->streams[aIdx]->codecpar);
            if (avcodec_open2(adecCtx, adec, nullptr) < 0) { avcodec_free_context(&adecCtx); adecCtx = nullptr; }
        }

        const AVCodec* aenc = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (adecCtx && aenc) {
            aencCtx = avcodec_alloc_context3(aenc);

#if LIBAVCODEC_VERSION_MAJOR >= 59
            if (adecCtx->sample_rate > 0) aencCtx->sample_rate = adecCtx->sample_rate; else aencCtx->sample_rate = 48000;
            aencCtx->time_base = AVRational{1, aencCtx->sample_rate};
            if (aenc->sample_fmts) aencCtx->sample_fmt = aenc->sample_fmts[0]; else aencCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
            set_audio_layout_from_decoder(aencCtx, adecCtx);
#else
            aencCtx->channels       = adecCtx->channels ? adecCtx->channels : 2;
            aencCtx->channel_layout = adecCtx->channel_layout ? adecCtx->channel_layout : AV_CH_LAYOUT_STEREO;
            aencCtx->sample_rate    = adecCtx->sample_rate ? adecCtx->sample_rate : 48000;
            aencCtx->sample_fmt     = aenc->sample_fmts ? aenc->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
            aencCtx->time_base      = AVRational{1, aencCtx->sample_rate};
#endif

            if (avcodec_open2(aencCtx, aenc, nullptr) == 0) {
                aout = avformat_new_stream(ofmt, aenc);
                if (!aout || avcodec_parameters_from_context(aout->codecpar, aencCtx) < 0) {
                    if (aout) aout->codecpar = nullptr;
                    avcodec_free_context(&aencCtx); aencCtx = nullptr;
                } else {
                    aout->time_base = aencCtx->time_base;
                }
            } else {
                avcodec_free_context(&aencCtx); aencCtx = nullptr;
            }
        }
    }

    if (!(ofmt->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&ofmt->pb, outUrl, AVIO_FLAG_WRITE) < 0) { std::cerr << "open output failed: " << outUrl << "\n"; return 1; }
    }
    if (avformat_write_header(ofmt, nullptr) < 0) { std::cerr << "write header failed\n"; return 1; }

    AVPacket* ipkt = av_packet_alloc();
    AVPacket* opkt = av_packet_alloc();
    AVFrame*  vfrm = av_frame_alloc();
    AVFrame*  afrm = av_frame_alloc();

    // Caption state
    const bool USE_ROLLUP = true;
    RollUp2State ru2{};
    bool caption_pending = false;
    std::string current_caption;

    // NEW: track last two distinct captions to avoid duplicate lines in RU2
    std::string prev_caption;  // top line (previous)
    std::string curr_caption;  // bottom line (current)

    // Linger last caption so player can latch on
    std::string last_caption;
    int64_t last_caption_expire_pts = AV_NOPTS_VALUE;

    // Bootstrap caption (helps players expose CC track immediately)
    bool bootstrap_pending = (bootstrap_enable != 0);
    std::string bootstrap_caption = "CC ONLINE";
    int64_t bootstrap_expire_pts = AV_NOPTS_VALUE;

    // External UDP listener
    CaptionInput caprx{};
    if (use_external_udp_captions) {
        if (!open_udp_listener(caprx, cc_host, cc_port)) {
            std::cerr << "Failed to open UDP caption listener; continuing without external captions.\n";
            use_external_udp_captions = false;
        }
    }

    while (av_read_frame(ifmt, ipkt) >= 0) {
        if (ipkt->stream_index == vIdx) {
            if (avcodec_send_packet(vdecCtx, ipkt) == 0) {
                while (avcodec_receive_frame(vdecCtx, vfrm) == 0) {
                    // Rescale PTS to encoder tb
                    AVRational src = ifmt->streams[vIdx]->time_base;
                    AVRational dst = vencCtx->time_base;
                    if (vfrm->pts != AV_NOPTS_VALUE)
                        vfrm->pts = av_rescale_q(vfrm->pts, src, dst);

                    // Poll UDP (non-blocking) and log
                    if (use_external_udp_captions && caprx.enabled) {
                        std::string latest;
                        if (udp_get_latest_line_and_log(caprx.fd, latest)) {
                            if (!latest.empty()) {
                                current_caption = latest;
                                caption_pending = true;

                                // (Re)set linger
                                int64_t linger = (int64_t)((linger_ms / 1000.0) * (vencCtx->time_base.den / (double)vencCtx->time_base.num));
                                last_caption = current_caption;
                                last_caption_expire_pts = (vfrm->pts == AV_NOPTS_VALUE) ? linger : vfrm->pts + linger;
                            }
                        }
                    }

                    // Bootstrap immediately at start (for ~1s)
                    if (bootstrap_pending) {
                        int64_t linger = (int64_t)(1.0 * vencCtx->time_base.den / (double)vencCtx->time_base.num);
                        bootstrap_expire_pts = (vfrm->pts == AV_NOPTS_VALUE) ? linger : vfrm->pts + linger;
                        bootstrap_pending = false;

                        last_caption = bootstrap_caption;
                        last_caption_expire_pts = bootstrap_expire_pts;
                        current_caption = bootstrap_caption;
                        caption_pending = true; // force immediate injection
                    } else if (bootstrap_enable &&
                               vfrm->pts != AV_NOPTS_VALUE &&
                               vfrm->pts < bootstrap_expire_pts &&
                               !caption_pending) {
                        current_caption = bootstrap_caption;
                        caption_pending = true; // keep bootstrap alive during window
                    }

                    // Remove any previous A/53 on this frame
                    av_frame_remove_side_data(vfrm, AV_FRAME_DATA_A53_CC);

                    // -------------------- Build CC buffer with "distinct-roll" logic --------------------
                    std::vector<uint8_t> cc;
                    bool do_inject      = false;
                    bool do_repaint     = false;  // repaint bottom w/o CR
                    bool do_roll        = false;  // roll (CR), then paint bottom

                    // NEW UDP/bootstrap line just arrived
                    if (caption_pending && !current_caption.empty()) {
                        caption_pending = false; // consume the event

                        // First-time bootstrap: if nothing on screen yet, paint bottom only
                        if (!ru2.started && curr_caption.empty()) {
                            curr_caption = current_caption;
                            do_repaint   = true;   // RU2 (once) + PAC + text
                            do_inject    = true;
                        } else {
                            // Only roll when the new line is DISTINCT from the current bottom line
                            if (current_caption != curr_caption) {
                                prev_caption = curr_caption;     // becomes top after CR
                                curr_caption = current_caption;  // becomes bottom after CR
                                do_roll   = true;                // send CR + new text
                                do_inject = true;
                            } else {
                                // Same text as bottom: repaint only (avoid duplicates on both rows)
                                do_repaint = true;
                                do_inject  = true;
                            }
                        }
                    }
                    // Linger window: repaint only (no CR)
                    else if (!curr_caption.empty() &&
                             vfrm->pts != AV_NOPTS_VALUE &&
                             vfrm->pts < last_caption_expire_pts) {
                        current_caption = curr_caption; // reaffirm bottom text
                        do_repaint  = true;
                        do_inject   = true;
                    }

                    if (do_inject) {
                        if (USE_ROLLUP) {
                            if (do_roll)      build_ru2_update_cc(cc, ru2, current_caption);     // includes CR
                            else              build_ru2_repaint_no_roll(cc, ru2, current_caption);
                        } else {
                            build_popon_cc(cc, current_caption);
                        }
                    }

                    // Attach CC side-data (and log)
                    if (!cc.empty()) {
                        AVFrameSideData* sd = av_frame_new_side_data(vfrm, AV_FRAME_DATA_A53_CC, cc.size());
                        if (sd) {
                            std::memcpy(sd->data, cc.data(), cc.size());
                            std::cerr << "[cc] inject len=" << cc.size()
                                      << (do_roll ? " (roll)" : " (repaint)")
                                      << " pts=" << vfrm->pts << "\n";
                        }
                    }

                    // Encode -> mux
                    if (avcodec_send_frame(vencCtx, vfrm) < 0) break;
                    while (avcodec_receive_packet(vencCtx, opkt) == 0) {
                        av_packet_rescale_ts(opkt, vencCtx->time_base, vout->time_base);
                        opkt->stream_index = vout->index;
                        av_interleaved_write_frame(ofmt, opkt);
                        av_packet_unref(opkt);
                    }
                    av_frame_unref(vfrm);
                }
            }
        } else if (aIdx >= 0 && ipkt->stream_index == aIdx && adecCtx && aencCtx && aout) {
            if (avcodec_send_packet(adecCtx, ipkt) == 0) {
                while (avcodec_receive_frame(adecCtx, afrm) == 0) {
                    if (avcodec_send_frame(aencCtx, afrm) < 0) break;
                    while (avcodec_receive_packet(aencCtx, opkt) == 0) {
                        av_packet_rescale_ts(opkt, aencCtx->time_base, aout->time_base);
                        opkt->stream_index = aout->index;
                        av_interleaved_write_frame(ofmt, opkt);
                        av_packet_unref(opkt);
                    }
                    av_frame_unref(afrm);
                }
            }
        }
        av_packet_unref(ipkt);
    }

    // Flush video
    avcodec_send_frame(vencCtx, nullptr);
    while (avcodec_receive_packet(vencCtx, opkt) == 0) {
        av_packet_rescale_ts(opkt, vencCtx->time_base, vout->time_base);
        opkt->stream_index = vout->index;
        av_interleaved_write_frame(ofmt, opkt);
        av_packet_unref(opkt);
    }
    // Flush audio
    if (aencCtx && aout) {
        avcodec_send_frame(aencCtx, nullptr);
        while (avcodec_receive_packet(aencCtx, opkt) == 0) {
            av_packet_rescale_ts(opkt, aencCtx->time_base, aout->time_base);
            opkt->stream_index = aout->index;
            av_interleaved_write_frame(ofmt, opkt);
            av_packet_unref(opkt);
        }
    }

    av_write_trailer(ofmt);

    // cleanup
    av_frame_free(&vfrm); av_frame_free(&afrm);
    av_packet_free(&ipkt); av_packet_free(&opkt);
    if (adecCtx) avcodec_free_context(&adecCtx);
    if (aencCtx) avcodec_free_context(&aencCtx);
    avcodec_free_context(&vdecCtx);
    avcodec_free_context(&vencCtx);
    if (!(ofmt->oformat->flags & AVFMT_NOFILE)) avio_closep(&ofmt->pb);
    avformat_free_context(ofmt);
    avformat_close_input(&ifmt);

    // close UDP
    if (caprx.fd >= 0) close(caprx.fd);

    std::cout << "Done: " << outUrl << "\n";
    return 0;
}

