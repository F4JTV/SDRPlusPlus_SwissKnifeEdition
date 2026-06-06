#pragma once

#include <mutex>
#include <dsp/block.h>
#include <module.h>
#include <dsp/multirate/rational_resampler.h>
#include <dsp/demod/fm.h>
#include <dsp/window/blackman.h>
#include <signal_path/signal_path.h>
#include "decode/decoder.hpp"
#include "gpx.hpp"
#include "ptu.hpp"
#include "tcp_sender.h"

/* Display name, bandwidth, decoder */
typedef std::tuple<const char*, float, dsp::block*> sondespec_t;

class RadiosondeDecoderModule : public ModuleManager::Instance {
public:
	RadiosondeDecoderModule(std::string name);
	~RadiosondeDecoderModule();

	void postInit() override;
	void enable() override;
	void disable() override;
	bool isEnabled() override;

private:
	std::string name;
	/* Pre-built, stable ImGui widget IDs. These must outlive each ImGui call;
	 * building them on the fly with (std::string(a)+b).c_str() returns a
	 * pointer into a temporary that is destroyed at the end of the statement,
	 * which ImGui then reads later (use-after-free -> crash). */
	std::string idType, idTable, idGpxTrack, idGpxFname, idPtuLog, idPtuFname;
	bool enabled = true;
	bool gpxOutput = false, ptuOutput = false;
	char gpxFilename[2048];
	char ptuFilename[2048];
	VFOManager::VFO *vfo;
	float currentBandwidth = 0;
	dsp::demod::FM<float> fmDemod;
	dsp::multirate::RationalResampler<float> resampler;

	radiosonde::Decoder<RS41Decoder, rs41_decoder_init, rs41_decoder_deinit, rs41_decode> rs41decoder;
	radiosonde::Decoder<DFM09Decoder, dfm09_decoder_init, dfm09_decoder_deinit, dfm09_decode> dfm09decoder;
	radiosonde::Decoder<IMS100Decoder, ims100_decoder_init, ims100_decoder_deinit, ims100_decode> ims100decoder;
	radiosonde::Decoder<M10Decoder, m10_decoder_init, m10_decoder_deinit, m10_decode> m10decoder;
	radiosonde::Decoder<IMET4Decoder, imet4_decoder_init, imet4_decoder_deinit, imet4_decode> imet4decoder;
	radiosonde::Decoder<C50Decoder, c50_decoder_init, c50_decoder_deinit, c50_decode> c50decoder;
	radiosonde::Decoder<MRZN1Decoder, mrzn1_decoder_init, mrzn1_decoder_deinit, mrzn1_decode> mrzn1decoder;

	const sondespec_t supportedTypes[7] = {
		sondespec_t("RS41", 1e4, &rs41decoder),
		sondespec_t("DFM06/09", 1.5e4, &dfm09decoder),
		sondespec_t("iMS100/RS-11G", 2e4, &ims100decoder),
		sondespec_t("M10/M20", 5e4, &m10decoder),
		sondespec_t("iMet-4", 2e4, &imet4decoder),
		sondespec_t("SRS-C50", 2e4, &c50decoder),
		sondespec_t("MRZ-N1", 2e4, &mrzn1decoder),
	};
	int selectedType = -1;
	dsp::block *activeDecoder;

	SondeFullData lastData;
	std::mutex lastDataMtx;   /* protects lastData (written by decoder thread,
	                             read by GUI thread) */
	GPXWriter gpxWriter;
	PTUWriter ptuWriter;

	/* TCP output to the Django map collector (one JSON line per fix). */
	TcpLineSender tcpSender;
	bool tcpEnabled = false;
	char tcpHost[256] = "127.0.0.1";
	int  tcpPort = 10100;
	std::string idTcpEnable, idTcpHost, idTcpPort;

	static void menuHandler(void *ctx);
	static void sondeDataHandler(SondeFullData *data, void *ctx);
	static void onTypeSelected(void *ctx, int selection);
	static void onGPXOutputChanged(void *ctx);
	static void onPTUOutputChanged(void *ctx);
};
