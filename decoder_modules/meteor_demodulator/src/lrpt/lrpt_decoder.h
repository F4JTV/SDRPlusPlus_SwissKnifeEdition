#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <functional>

#include "codings/correlator.h"
#include "codings/rotation.h"
#include "codings/randomization.h"
#include "codings/nrzm.h"
#include "codings/reedsolomon.h"
#include "codings/viterbi/viterbi27.h"
#include "codings/viterbi/viterbi_1_2.h"
#include "codings/viterbi/utils.h"
#include "codings/deframing/bpsk_ccsds_deframer.h"
#include "deint.h"

#include "ccsds/vcdu.h"
#include "ccsds/demuxer.h"
#include "msumr/lrpt_msumr_reader.h"
#include "msumr/simple_image.h"

/*
 * Full Meteor LRPT decoder, ported from SatDump's meteor_support plugin.
 *
 * Two pipelines, selectable at runtime:
 *
 *  MODE_LEGACY  (old Meteor-M2 / M2-2, non-interleaved QPSK)
 *     Correlator -> rotate_soft -> Viterbi27 -> (NRZM) -> derand -> RS -> CADU
 *
 *  MODE_M2X     (Meteor-M2-3 / M2-4, OQPSK + interleaved)
 *     DintSampleReader (raw + PHASE_90 copy)
 *       -> 2x DeinterleaverReader (dbdexter conv. deinterleaver)
 *       -> 2x Viterbi1_2 (phase/IQ search, keep best lock)
 *       -> (NRZM bits) -> BPSK_CCSDS_Deframer -> derand -> RS -> CADU
 *
 *  CADU -> VCDU (keep VCID 5) -> AOS demuxer (882, insert zone)
 *       -> MSU-MR LRPT reader -> per-channel image + RGB composite
 *
 * Soft symbols are pushed in (int8 I/Q). A worker thread blocks reading them
 * from a FIFO, exactly mirroring SatDump's pull-based read_data(), so the
 * (pull-based) deinterleaver/Viterbi code ports almost verbatim.
 */
class LRPTDecoder {
public:
    enum Mode { MODE_LEGACY = 0, MODE_M2X = 1, MODE_M2X_INT = 2 };

    static constexpr int FRAME_SIZE = 1024;
    static constexpr int ENCODED_FRAME_SIZE = 1024 * 8 * 2; // 16384
    static constexpr int BUFFER_SIZE = 8192;

    explicit LRPTDecoder(Mode mode = MODE_M2X, bool diff_decode = true)
        : d_mode(mode), d_diff_decode(diff_decode) {
        fifo.reserve(ENCODED_FRAME_SIZE * 4);
        start();
    }

    ~LRPTDecoder() { stop(); }

    // Feed soft QPSK symbols (interleaved I,Q int8 soft values).
    void pushSymbols(const int8_t* data, int count) {
        {
            std::lock_guard<std::mutex> l(fifoMtx);
            fifo.insert(fifo.end(), data, data + count);
        }
        fifoCv.notify_one();
    }

    // Stats
    bool     isLocked()   { return locked.load(); }
    float    getBER()     { return ber.load(); }
    int      getRSAvg()   { return rsAvg.load(); }
    uint64_t getCADUs()   { return caduCount.load(); }
    uint64_t getPackets() { return packetCount.load(); }
    uint64_t getFrames()  { return frameCount.load(); }   // deframer/Viterbi frames, pre-RS

    void setRsBypass(bool v) { rsBypass.store(v); }
    bool getRsBypass()       { return rsBypass.load(); }

    // Per-APID packet counts (index 0..6 = APID 64..70). For diagnostics.
    void getApidPackets(uint64_t out[7]) {
        for (int i = 0; i < 7; i++) out[i] = apidPackets[i].load();
    }

    void reset() {
        stop();
        { std::lock_guard<std::mutex> l(fifoMtx); fifo.clear(); fifoHead = 0; }
        caduCount = 0; packetCount = 0; frameCount = 0; rsAvg = -1; locked = false; ber = 10.0f;
        start();
    }

    void setMode(Mode m) {
        if (m == d_mode) return;
        stop(); d_mode = m;
        { std::lock_guard<std::mutex> l(fifoMtx); fifo.clear(); fifoHead = 0; }
        start();
    }
    Mode getMode() { return d_mode; }

    void setDiffDecode(bool v) {
        if (v == d_diff_decode) return;
        stop(); d_diff_decode = v; start();
    }
    bool getDiffDecode() { return d_diff_decode; }

    std::vector<int> activeChannels() {
        std::lock_guard<std::mutex> l(readerMtx);
        std::vector<int> out;
        if (!reader) return out;
        for (int c = 0; c < 6; c++)
            if (reader->getChannel(c).size() > 0) out.push_back(c);
        return out;
    }

    meteorimg::SimpleImage getChannelImage(int channel) {
        std::lock_guard<std::mutex> l(readerMtx);
        if (!reader) return meteorimg::SimpleImage();
        return reader->getChannel(channel);
    }

    bool getComposite(int rCh, int gCh, int bCh, std::vector<uint8_t>& rgb, int& width, int& height) {
        std::lock_guard<std::mutex> l(readerMtx);
        if (!reader) return false;
        meteorimg::SimpleImage r = reader->getChannel(rCh);
        meteorimg::SimpleImage g = reader->getChannel(gCh);
        meteorimg::SimpleImage b = reader->getChannel(bCh);
        size_t w = 0, h = 0;
        for (auto* im : {&r, &g, &b}) {
            if (im->width() > w) w = im->width();
            if (im->height() > h) h = im->height();
        }
        if (w == 0 || h == 0) return false;
        width = (int)w; height = (int)h;
        rgb.assign(w * h * 3, 0);
        auto sample = [](meteorimg::SimpleImage& im, size_t idx) -> uint8_t {
            return (idx < im.size()) ? im.get(idx) : 0;
        };
        for (size_t i = 0; i < w * h; i++) {
            rgb[i * 3 + 0] = sample(r, i);
            rgb[i * 3 + 1] = sample(g, i);
            rgb[i * 3 + 2] = sample(b, i);
        }
        return true;
    }

private:
    // ---- lifecycle ----
    void start() {
        stopWorker = false;
        {
            std::lock_guard<std::mutex> l(readerMtx);
            demuxer = std::make_unique<ccsds::ccsds_aos::Demuxer>(882, true);
            // M2-x reader for M2-3 / M2-4 (affects timestamp parsing); legacy for old M2.
            reader  = std::make_unique<meteor::msumr::lrpt::MSUMRReader>(d_mode != MODE_LEGACY);
            for (int i = 0; i < 7; i++) apidPackets[i] = 0;
        }
        worker = std::thread(&LRPTDecoder::workerLoop, this);
    }

    void stop() {
        stopWorker = true;
        fifoCv.notify_all();
        if (worker.joinable()) worker.join();
    }

    // ---- blocking soft-symbol source (mirrors SatDump read_data) ----
    // Returns true on success (len bytes filled), false if stopping.
    bool readData(int8_t* out, size_t len) {
        std::unique_lock<std::mutex> lk(fifoMtx);
        fifoCv.wait(lk, [&] { return stopWorker.load() || (fifo.size() - fifoHead) >= len; });
        if (stopWorker.load()) return false;
        memcpy(out, fifo.data() + fifoHead, len);
        fifoHead += len;
        if (fifoHead > (size_t)(1 << 20)) { // compact occasionally
            fifo.erase(fifo.begin(), fifo.begin() + fifoHead);
            fifoHead = 0;
        }
        return true;
    }

    void workerLoop() {
        if (d_mode == MODE_M2X_INT) runM2x(true);
        else if (d_mode == MODE_M2X) runM2x(false);
        else runLegacy();
    }

    // ============ LEGACY (non-interleaved QPSK, Viterbi27) ============
    void runLegacy() {
        Correlator correlator(QPSK, d_diff_decode ? 0xfc4ef4fd0cc2df89 : 0xfca2b63db00d9794);
        reedsolomon::ReedSolomon rs(reedsolomon::RS223);
        viterbi::Viterbi27 viterbi(ENCODED_FRAME_SIZE / 2, viterbi::CCSDS_R2_K7_POLYS);
        diff::NRZMDiff nrzm;

        std::vector<int8_t> buffer(ENCODED_FRAME_SIZE);
        uint8_t frameBuffer[FRAME_SIZE];
        int errors[4];

        while (!stopWorker.load()) {
            if (!readData(buffer.data(), ENCODED_FRAME_SIZE)) break;

            phase_t phase; bool swap; int cor = 0;
            int pos = correlator.correlate(buffer.data(), phase, swap, cor, ENCODED_FRAME_SIZE);

            locked = (pos == 0);

            if (pos != 0 && pos < ENCODED_FRAME_SIZE) {
                memmove(buffer.data(), buffer.data() + pos, ENCODED_FRAME_SIZE - pos);
                if (!readData(buffer.data() + ENCODED_FRAME_SIZE - pos, pos)) break;
            }

            rotate_soft(buffer.data(), ENCODED_FRAME_SIZE, phase, swap);
            viterbi.work(buffer.data(), frameBuffer);
            ber = viterbi.ber();

            if (d_diff_decode) nrzm.decode(frameBuffer, FRAME_SIZE);

            derand_ccsds(&frameBuffer[4], FRAME_SIZE - 4);

            if (frameBuffer[9] == 0xFF)
                for (int i = 0; i < FRAME_SIZE; i++) frameBuffer[i] ^= 0xFF;

            rs.decode_interlaved(&frameBuffer[4], false, 4, errors);
            rsAvg = (errors[0] + errors[1] + errors[2] + errors[3]) / 4;
            frameCount++; // a frame reached the RS stage

            bool rsOk = (errors[0] >= 0 && errors[1] >= 0 && errors[2] >= 0 && errors[3] >= 0);
            if (rsOk || rsBypass.load()) {
                uint8_t cadu[1024];
                cadu[0] = 0x1d; cadu[1] = 0xcf; cadu[2] = 0xfc; cadu[3] = 0x1d;
                memcpy(&cadu[4], &frameBuffer[4], FRAME_SIZE - 4);
                caduCount++;
                handleCADU(cadu);
            }
        }
    }

    // Helper: two-stream sample reader (raw + PHASE_90), pulling from readData.
    struct DintSampleReader {
        std::function<bool(int8_t*, size_t)> input_function; // true = ok, false = stop
        bool iserror = false;
        std::vector<int8_t> buffer1, buffer2;

        void read_more() {
            buffer1.resize(buffer1.size() + 8192);
            iserror = iserror || !input_function(&buffer1[buffer1.size() - 8192], 8192);
            buffer2.resize(buffer2.size() + 8192);
            memcpy(&buffer2[buffer2.size() - 8192], &buffer1[buffer1.size() - 8192], 8192);
            rotate_soft(&buffer2[buffer2.size() - 8192], 8192, PHASE_90, false);
        }
        int read1(int8_t* buf, size_t len) {
            while (buffer1.size() < len && !iserror) read_more();
            if (iserror) return 0;
            memcpy(buf, buffer1.data(), len);
            buffer1.erase(buffer1.begin(), buffer1.begin() + len);
            return (int)len;
        }
        int read2(int8_t* buf, size_t len) {
            while (buffer2.size() < len && !iserror) read_more();
            if (iserror) return 0;
            memcpy(buf, buffer2.data(), len);
            buffer2.erase(buffer2.begin(), buffer2.begin() + len);
            return (int)len;
        }
    };

    // ============ M2-X (OQPSK, Viterbi1_2) ============
    // interleaved == false : non-interleaved 72k (Meteor-M2-3 typical)
    // interleaved == true  : interleaved 80k    (Meteor-M2-4 / alt)
    void runM2x(bool interleaved) {
        const float ber_thresold = 0.170f;
        const int   outsync_after = 5;

        std::vector<phase_t> phases = {PHASE_0, PHASE_90};
        viterbi::Viterbi1_2 vit1(ber_thresold, outsync_after, BUFFER_SIZE, phases, true);
        viterbi::Viterbi1_2 vit2(ber_thresold, outsync_after, BUFFER_SIZE, phases, true);
        meteor::DeinterleaverReader deint1, deint2;
        deframing::BPSK_CCSDS_Deframer deframer(8192);
        reedsolomon::ReedSolomon rs(reedsolomon::RS223);
        diff::NRZMDiff diffd;

        std::vector<int8_t> _buffer(ENCODED_FRAME_SIZE + INTER_MARKER_STRIDE);
        std::vector<int8_t> _buffer2(ENCODED_FRAME_SIZE + INTER_MARKER_STRIDE);
        int8_t* buffer  = &_buffer[INTER_MARKER_STRIDE];
        int8_t* buffer2 = &_buffer2[INTER_MARKER_STRIDE];

        std::vector<uint8_t> viterbi_out(BUFFER_SIZE * 2);
        std::vector<uint8_t> viterbi_out2(BUFFER_SIZE * 2);
        std::vector<uint8_t> frame_buffer(BUFFER_SIZE * 2);
        int errors[4];

        DintSampleReader file_reader;
        file_reader.input_function = [this](int8_t* buf, size_t len) -> bool {
            return readData(buf, len);
        };

        while (!stopWorker.load()) {
            int vitout = 0;
            uint8_t* vout = viterbi_out.data();

            if (interleaved) {
                // Deinterleave two phase candidates, keep whichever Viterbi locks best
                int r1 = deint1.read_samples(
                    [&file_reader](int8_t* buf, size_t len) -> int { return (bool)file_reader.read1(buf, len); },
                    buffer, 8192);
                int r2 = deint2.read_samples(
                    [&file_reader](int8_t* buf, size_t len) -> int { return (bool)file_reader.read2(buf, len); },
                    buffer2, 8192);
                if (r1 || r2 || file_reader.iserror) break;

                int vitout1 = vit1.work(buffer,  BUFFER_SIZE, viterbi_out.data());
                int vitout2 = vit2.work(buffer2, BUFFER_SIZE, viterbi_out2.data());
                if (vit2.getState() > vit1.getState()) {
                    vitout = vitout2; ber = vit2.ber(); locked = vit2.getState() > 0;
                    vout = viterbi_out2.data();
                } else {
                    vitout = vitout1; ber = vit1.ber(); locked = vit1.getState() > 0;
                }
            }
            else {
                // Non-interleaved: read straight, single Viterbi1_2 (does its own phase/IQ search)
                if (!readData(buffer, BUFFER_SIZE)) break;
                vitout = vit1.work(buffer, BUFFER_SIZE, viterbi_out.data());
                ber = vit1.ber();
                locked = vit1.getState() > 0;
            }

            if (d_diff_decode) diffd.decode_bits(vout, vitout);

            int frames = deframer.work(vout, vitout, frame_buffer.data());
            frameCount += frames; // frames the deframer emitted (pre-RS)

            for (int i = 0; i < frames; i++) {
                uint8_t* cadu = &frame_buffer[i * 1024];
                derand_ccsds(&cadu[4], 1020);
                rs.decode_interlaved(&cadu[4], false, 4, errors);
                rsAvg = (errors[0] + errors[1] + errors[2] + errors[3]) / 4;
                bool rsOk = (errors[0] >= 0 && errors[1] >= 0 && errors[2] >= 0 && errors[3] >= 0);
                if (rsOk || rsBypass.load()) {
                    caduCount++;
                    handleCADU(cadu);
                }
            }
        }
    }

    void handleCADU(uint8_t* c) {
        ccsds::ccsds_aos::VCDU vcdu = ccsds::ccsds_aos::parseVCDU(c);
        if (vcdu.vcid != 5) return; // MSU-MR imagery only
        std::lock_guard<std::mutex> l(readerMtx);
        if (!demuxer || !reader) return;
        std::vector<ccsds::CCSDSPacket> pkts = demuxer->work(c);
        for (ccsds::CCSDSPacket& pkt : pkts) {
            packetCount++;
            int a = (int)pkt.header.apid - 64;
            if (a >= 0 && a < 7) apidPackets[a]++;
            reader->work(pkt);
        }
    }

    Mode d_mode;
    bool d_diff_decode;

    // FIFO of soft symbols
    std::vector<int8_t> fifo;
    size_t fifoHead = 0;
    std::mutex fifoMtx;
    std::condition_variable fifoCv;
    std::atomic<bool> stopWorker{false};
    std::thread worker;

    // Shared decode output
    std::mutex readerMtx;
    std::unique_ptr<ccsds::ccsds_aos::Demuxer> demuxer;
    std::unique_ptr<meteor::msumr::lrpt::MSUMRReader> reader;

    // Stats (atomics, written by worker)
    std::atomic<bool>     locked{false};
    std::atomic<float>    ber{10.0f};
    std::atomic<int>      rsAvg{-1};
    std::atomic<uint64_t> caduCount{0};
    std::atomic<uint64_t> packetCount{0};
    std::atomic<uint64_t> frameCount{0};
    std::atomic<bool>     rsBypass{false};
    std::atomic<uint64_t> apidPackets[7];
};
