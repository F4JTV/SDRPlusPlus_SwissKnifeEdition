#include <core.h>
#include <config.h>
#include <utils/flog.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <imgui.h>
#include <module.h>
#include <signal_path/signal_path.h>
#include <time.h>
#include "main.hpp"
#include "utils.hpp"


#define SNAP_INTERVAL 1000
#define UNCAL_COLOR IM_COL32(255,234,0,255)
#define OUT_SAMPLE_RATE 48000

SDRPP_MOD_INFO {
    /* Name:            */ "radiosonde_decoder",
    /* Description:     */ "Radiosonde decoder for SDR++",
    /* Author:          */ "dbdexter-dev",
    /* Version:         */ 0, 12, 0,
    /* Max instances    */ -1
};

ConfigManager config;

RadiosondeDecoderModule::RadiosondeDecoderModule(std::string name)
{
	float bw;
	bool created = false;
	int typeToSelect;
	std::string gpxPath, ptuPath;

	this->name = name;
	selectedType = -1;
	activeDecoder = NULL;

	/* Build stable widget IDs once. */
	idType      = "##_radiosonde_type_" + name;
	idTable     = "##radiosonde_data_" + name;
	idGpxTrack  = "GPX track##_gpx_track_" + name;
	idGpxFname  = "##_gpx_fname_" + name;
	idPtuLog    = "Log data##_ptu_log_" + name;
	idPtuFname  = "##_ptu_fname_" + name;
	idTcpEnable = "##_tcp_en_" + name;
	idTcpHost   = "##_tcp_host_" + name;
	idTcpPort   = "##_tcp_port_" + name;

	config.acquire();
	if (!config.conf.contains(name)) {
		config.conf[name]["gpxPath"] = getTempFile("radiosonde.gpx");
		config.conf[name]["ptuPath"] = getTempFile("radiosonde_ptu.csv");
		config.conf[name]["sondeType"] = 0;
		created = true;
	}
	/* TCP map output settings: host and port are persisted, but tcpEnabled is
	 * NOT — we never auto-open a TCP connection at SDR++ startup. The user
	 * must tick "Enable TCP" manually in each session. */
	if (!config.conf[name].contains("tcpHost"))    config.conf[name]["tcpHost"]    = "127.0.0.1";
	if (!config.conf[name].contains("tcpPort"))    config.conf[name]["tcpPort"]    = 10100;
	gpxPath = config.conf[name]["gpxPath"];
	ptuPath = config.conf[name]["ptuPath"];
	typeToSelect = config.conf[name]["sondeType"];
	{
		std::string h = config.conf[name]["tcpHost"];
		strncpy(tcpHost, h.c_str(), sizeof(tcpHost)-1);
		tcpHost[sizeof(tcpHost)-1] = 0;
	}
	tcpPort = config.conf[name]["tcpPort"];
	/* tcpEnabled stays at its default (false): no socket attempts on startup. */
	config.release(created);

	strncpy(gpxFilename, gpxPath.c_str(), sizeof(gpxFilename)-1);
	strncpy(ptuFilename, ptuPath.c_str(), sizeof(ptuFilename)-1);

	/* Clamp the saved type to a valid range before it is ever used (the menu
	 * handler indexes supportedTypes[selectedType], so it must be valid before
	 * the first GUI frame). */
	if (typeToSelect < 0 || typeToSelect >= IM_ARRAYSIZE(supportedTypes)) typeToSelect = 0;
	selectedType = typeToSelect;
	currentBandwidth = std::get<1>(supportedTypes[typeToSelect]);
	bw = currentBandwidth;

	/* Create the VFO exactly once. onTypeSelected() will only recreate it if
	 * the bandwidth actually changes, avoiding a redundant
	 * delete-then-create of a VFO with the same name during construction
	 * (which races with the GUI/waterfall thread and can crash).
	 *
	 * The VFO bandwidth is unlocked so the user can shrink it with the mouse
	 * in the waterfall, down to 1 kHz; the upper bound stays at the value
	 * recommended for the selected sonde type. */
	vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, bw, bw, 1000.0, bw, false);
	if (!vfo || !vfo->output) {
		/* Could not obtain a usable VFO (e.g. a name clash in the IQ
		 * front-end). Bail out cleanly instead of dereferencing NULL. */
		flog::error("[radiosonde] Failed to create VFO '{}'", name);
		if (vfo) { sigpath::vfoManager.deleteVFO(vfo); vfo = NULL; }
		enabled = false;
		gui::menu.registerEntry(name, menuHandler, this, this);
		return;
	}
	vfo->setSnapInterval(SNAP_INTERVAL);
	/* Listen to user-driven VFO resizes from the waterfall, so we can keep
	 * the FM demodulator and the resampler in sync with the new bandwidth. */
	onVfoBandwidthChangedHandler.handler = onVfoBandwidthChanged;
	onVfoBandwidthChangedHandler.ctx = this;
	vfo->wtfVFO->onUserChangedBandwidth.bindHandler(&onVfoBandwidthChangedHandler);

	fmDemod.init(vfo->output, bw, bw/2.0f, false);

	/* Resampler to 48kHz */
	resampler.init(&fmDemod.out, bw, OUT_SAMPLE_RATE);

	dfm09decoder.init(&resampler.out, OUT_SAMPLE_RATE, sondeDataHandler, this);
	c50decoder.init(&resampler.out, OUT_SAMPLE_RATE, sondeDataHandler, this);
	imet4decoder.init(&resampler.out, OUT_SAMPLE_RATE, sondeDataHandler, this);
	ims100decoder.init(&resampler.out, OUT_SAMPLE_RATE, sondeDataHandler, this);
	m10decoder.init(&resampler.out, OUT_SAMPLE_RATE, sondeDataHandler, this);
	mrzn1decoder.init(&resampler.out, OUT_SAMPLE_RATE, sondeDataHandler, this);
	rs41decoder.init(&resampler.out, OUT_SAMPLE_RATE, sondeDataHandler, this);

	fmDemod.start();
	resampler.start();

	/* Select the decoder for the saved type. The VFO already matches its
	 * bandwidth, so this will not recreate it. */
	onTypeSelected(this, typeToSelect);
	enabled = true;

	gui::menu.registerEntry(name, menuHandler, this, this);

	/* Configure the TCP map link with the saved host/port, but do NOT start
	 * the worker here: opening a socket at SDR++ startup is unwanted. The user
	 * must tick "Enable TCP" in the module menu to actually connect. */
	tcpSender.configure(tcpHost, tcpPort);
}

RadiosondeDecoderModule::~RadiosondeDecoderModule()
{
	tcpSender.stop();
	if (isEnabled()) disable();
	if (vfo) {
		sigpath::vfoManager.deleteVFO(vfo);
		vfo = NULL;
	}
	gui::menu.removeEntry(name);
}

void
RadiosondeDecoderModule::enable() {
	/* Make a new VFO, wire it into the DSP path, then start the appropriate decoder */
	onTypeSelected(this, selectedType);

	fmDemod.start();
	resampler.start();
	enabled = true;
}

void
RadiosondeDecoderModule::disable() {
	if (activeDecoder) activeDecoder->stop();
	activeDecoder = NULL;

	fmDemod.stop();
	resampler.stop();

	if (vfo) sigpath::vfoManager.deleteVFO(vfo);
	vfo = NULL;
	currentBandwidth = 0;

	gpxWriter.stopTrack();
	{
		std::lock_guard<std::mutex> lck(lastDataMtx);
		lastData.init();
	}
	enabled = false;
}

bool
RadiosondeDecoderModule::isEnabled() {
	return enabled;
}

void
RadiosondeDecoderModule::postInit() {
}

/* Private methods {{{*/
void
RadiosondeDecoderModule::menuHandler(void *ctx)
{
	RadiosondeDecoderModule *_this = (RadiosondeDecoderModule*)ctx;
	const ImVec2 wh = ImGui::GetContentRegionAvail();
	const float width = wh.x;
	char time[64];
	bool gpxStatusChanged, ptuStatusChanged;

	if (!_this->enabled) style::beginDisabled();

	/* Take a consistent snapshot of the data under lock; the decoder thread
	 * may overwrite lastData (including its std::string members) at any time,
	 * and reading it unlocked is a data race that can hand ImGui a pointer
	 * into a string being reallocated -> crash in the menu's later memcmp. */
	SondeFullData data;
	{
		std::lock_guard<std::mutex> lck(_this->lastDataMtx);
		data = _this->lastData;
	}

	/* Guard against an out-of-range selection (e.g. a brief window during
	 * construction) to avoid indexing supportedTypes out of bounds. */
	if (_this->selectedType < 0 || _this->selectedType >= IM_ARRAYSIZE(_this->supportedTypes)) {
		_this->selectedType = 0;
	}

	/* Type combobox {{{ */
	ImGui::LeftLabel("Type");
	ImGui::SetNextItemWidth(width - ImGui::GetCursorPosX());
	if (ImGui::BeginCombo(_this->idType.c_str(), std::get<0>(_this->supportedTypes[_this->selectedType]))) {
		for (int i=0; i<IM_ARRAYSIZE(_this->supportedTypes); i++) {
			const char *curItem = std::get<0>(_this->supportedTypes[i]);
			bool selected = _this->selectedType == i;

			if (ImGui::Selectable(curItem, selected)) {
				onTypeSelected(ctx, i);
			}
			if (selected) {
				ImGui::SetItemDefaultFocus();
			}
		}
		ImGui::EndCombo();
	}
	/* }}} */
	/* Sonde data display {{{ */
	ImGui::SetNextItemWidth(width);
	if (ImGui::BeginTable(_this->idTable.c_str(), 2, ImGuiTableFlags_SizingFixedFit)) {
		ImGui::TableNextColumn();
		ImGui::Text("Serial no.");
		if (_this->enabled) {
			ImGui::TableNextColumn();
			ImGui::Text("%s", data.serial.c_str());
		}

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Frame no.");
		if (_this->enabled) {
			ImGui::TableNextColumn();
			ImGui::Text("%d", data.seq);
		}

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Onboard time");
		if (_this->enabled) {
			if (strftime(time, sizeof(time), "%a %b %d %Y %H:%M:%S", gmtime(&data.time))) {
				ImGui::TableNextColumn();
				ImGui::Text("%s", time);
			}
		}

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text(" ");

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Latitude");
		if (_this->enabled) {
			ImGui::TableNextColumn();
			ImGui::Text("%8.5f%c", fabs(data.lat), (data.lat >= 0 ? 'N' : 'S'));
		}

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Longitude");
		if (_this->enabled) {
			ImGui::TableNextColumn();
			ImGui::Text("%8.5f%c", fabs(data.lon), (data.lon >= 0 ? 'E' : 'W'));
		}

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Altitude");
		if (_this->enabled) {
			ImGui::TableNextColumn();
			ImGui::Text("%.1fm", data.alt);
		}

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Speed");
		if (_this->enabled) {
			ImGui::TableNextColumn();
			ImGui::Text("%.1fm/s", data.spd);
		}

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Heading");
		if (_this->enabled) {
			ImGui::TableNextColumn();
			ImGui::Text("%.0f°", data.hdg);
		}

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Climb");
		if (_this->enabled) {
			ImGui::TableNextColumn();
			ImGui::Text("%.1fm/s", data.climb);
		}

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text(" ");

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Temperature");
		if (_this->enabled) {
			ImGui::TableNextColumn();
			if (!data.calibrated) ImGui::PushStyleColor(ImGuiCol_Text, UNCAL_COLOR);
			ImGui::Text("%.1f°C", data.temp);
			if (!data.calibrated) ImGui::PopStyleColor();
			if (!data.calibrated && ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Calibration data not yet complete (%.0f%%).", data.calib_percent);
			}
		}

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Humidity");
		if (_this->enabled) {
			ImGui::TableNextColumn();
			if (!data.calibrated) ImGui::PushStyleColor(ImGuiCol_Text, UNCAL_COLOR);
			ImGui::Text("%.1f%%", data.rh);
			if (!data.calibrated) ImGui::PopStyleColor();
			if (!data.calibrated && ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Calibration data not yet complete (%.0f%%).", data.calib_percent);
			}
		}

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Dew point");
		if (_this->enabled) {
			ImGui::TableNextColumn();
			if (!data.calibrated) ImGui::PushStyleColor(ImGuiCol_Text, UNCAL_COLOR);
			ImGui::Text("%.1f°C", data.dewpt);
			if (!data.calibrated) ImGui::PopStyleColor();
			if (!data.calibrated && ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Calibration data not yet complete (%.0f%%).", data.calib_percent);
			}
		}

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Pressure");
		if (_this->enabled) {
			ImGui::TableNextColumn();
			if (!data.calibrated) ImGui::PushStyleColor(ImGuiCol_Text, UNCAL_COLOR);
			ImGui::Text("%.1fhPa", data.pressure);
			if (!data.calibrated) ImGui::PopStyleColor();
			if (!data.calibrated && ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Calibration data not yet complete (%.0f%%).", data.calib_percent);
			}
		}

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Text("Aux. data");
		if (_this->enabled) {
			ImGui::TableNextColumn();
			ImGui::Text("%s", data.auxData.c_str());
		}

		ImGui::EndTable();
	}
	/* }}} */
	/* GPX output file {{{ */
	gpxStatusChanged = ImGui::Checkbox(_this->idGpxTrack.c_str(), &_this->gpxOutput);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(width - ImGui::GetCursorPosX());
	gpxStatusChanged |= ImGui::InputText(_this->idGpxFname.c_str(), _this->gpxFilename, sizeof(gpxFilename)-1,
	                                     ImGuiInputTextFlags_EnterReturnsTrue);
	if (gpxStatusChanged) onGPXOutputChanged(ctx);
	/* }}} */
	/* Log output file {{{ */
	ptuStatusChanged = ImGui::Checkbox(_this->idPtuLog.c_str(), &_this->ptuOutput);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(width - ImGui::GetCursorPosX());
	ptuStatusChanged |= ImGui::InputText(_this->idPtuFname.c_str(), _this->ptuFilename, sizeof(ptuFilename)-1,
	                                     ImGuiInputTextFlags_EnterReturnsTrue);
	if (ptuStatusChanged) onPTUOutputChanged(ctx);
	/* }}} */

	/* TCP map output {{{
	 * UI styled exactly after the AIS / ADS-B modules: section title, Host /
	 * Port inputs with FillWidth(), and a single "Enable TCP" checkbox with an
	 * inline status word next to it ("disabled" / "enabled" / "connected"). */
	ImGui::Separator();
	ImGui::TextUnformatted("TCP map output");

	ImGui::LeftLabel("Host");
	ImGui::FillWidth();
	bool tcpChanged = ImGui::InputText(_this->idTcpHost.c_str(), _this->tcpHost,
	                                   sizeof(_this->tcpHost));
	ImGui::LeftLabel("Port");
	ImGui::FillWidth();
	if (ImGui::InputInt(_this->idTcpPort.c_str(), &_this->tcpPort)) {
		if (_this->tcpPort < 0) _this->tcpPort = 0;
		if (_this->tcpPort > 65535) _this->tcpPort = 65535;
		tcpChanged = true;
	}
	if (tcpChanged) {
		_this->tcpSender.configure(_this->tcpHost, _this->tcpPort);
		config.acquire();
		config.conf[_this->name]["tcpHost"] = std::string(_this->tcpHost);
		config.conf[_this->name]["tcpPort"] = _this->tcpPort;
		config.release(true);
	}

	if (ImGui::Checkbox(("Enable TCP" + _this->idTcpEnable).c_str(), &_this->tcpEnabled)) {
		if (_this->tcpEnabled) {
			_this->tcpSender.configure(_this->tcpHost, _this->tcpPort);
			_this->tcpSender.start();
		} else {
			_this->tcpSender.stop();
		}
		/* tcpEnabled is intentionally NOT persisted: we never want the module
		 * to open a TCP connection automatically at SDR++ startup. */
	}
	ImGui::SameLine();
	if (!_this->tcpEnabled) {
		ImGui::TextDisabled("disabled");
	} else if (_this->tcpSender.isConnected()) {
		ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "connected");
	} else {
		ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "enabled");
	}
	/* }}} */

	if (!_this->enabled) style::endDisabled();
}

void
RadiosondeDecoderModule::sondeDataHandler(SondeFullData *data, void *ctx)
{
	RadiosondeDecoderModule *_this = (RadiosondeDecoderModule*)ctx;
	{
		std::lock_guard<std::mutex> lck(_this->lastDataMtx);
		_this->lastData = *data;
	}

	if (data->serial != "") {
		_this->gpxWriter.startTrack(data->serial.c_str());
	}
	_this->gpxWriter.addTrackPoint(data->time, data->lat, data->lon, data->alt, data->spd, data->hdg);
	_this->ptuWriter.addPoint(data);

	/* Send to the Django map server (one JSON line per fix), same schema as the
	 * AIS / ADS-B / APRS / DSD-FME modules:
	 *   {"name","date","time","lat","lon","type","speed","info"}
	 * Only emit positions that actually carry a fix. */
	if (_this->tcpEnabled && data->serial != "" &&
	    (data->lat != 0.0f || data->lon != 0.0f)) {
		/* UTC date/time of reception, like the other modules. */
		time_t now = ::time(NULL);
		struct tm tmv;
#ifdef _WIN32
		gmtime_s(&tmv, &now);
#else
		gmtime_r(&now, &tmv);
#endif
		char date[32], tstr[16];
		snprintf(date, sizeof(date), "%04d-%02d-%02d",
		         tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
		snprintf(tstr, sizeof(tstr), "%02d:%02d:%02d",
		         tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

		/* Minimal JSON escaping for the serial (it can be alphanumeric only in
		 * practice, but stay safe). */
		auto jesc = [](const std::string& s) {
			std::string o; o.reserve(s.size() + 2);
			for (char c : s) {
				if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back(c); }
				else if ((unsigned char)c < 0x20) { /* skip control chars */ }
				else o.push_back(c);
			}
			return o;
		};
		std::string name = jesc(data->serial);

		char line[512];
		snprintf(line, sizeof(line),
		         "{\"name\":\"%s\",\"date\":\"%s\",\"time\":\"%s\","
		         "\"lat\":%.6f,\"lon\":%.6f,\"type\":\"radiosonde\","
		         "\"speed\":%.1f,\"info\":\"alt=%.0f;hdg=%.0f;climb=%.1f;"
		         "temp=%.1f;rh=%.0f;p=%.1f\"}",
		         name.c_str(), date, tstr,
		         data->lat, data->lon,
		         data->spd,
		         data->alt, data->hdg, data->climb,
		         data->temp, data->rh, data->pressure);
		_this->tcpSender.send(line);
	}
}

void
RadiosondeDecoderModule::onGPXOutputChanged(void *ctx)
{
	RadiosondeDecoderModule *_this = (RadiosondeDecoderModule*)ctx;
	if (_this->gpxOutput) {
		_this->gpxOutput = _this->gpxWriter.init(_this->gpxFilename);
	} else {
		_this->gpxWriter.deinit();
	}

	if (_this->gpxOutput) {
		config.acquire();
		config.conf[_this->name]["gpxPath"] = _this->gpxFilename;
		config.release(true);
	}
}

void
RadiosondeDecoderModule::onPTUOutputChanged(void *ctx)
{
	RadiosondeDecoderModule *_this = (RadiosondeDecoderModule*)ctx;
	if (_this->ptuOutput) {
		_this->ptuOutput = _this->ptuWriter.init(_this->ptuFilename);
	} else {
		_this->ptuWriter.deinit();
	}
	if (_this->ptuOutput) {
		config.acquire();
		config.conf[_this->name]["ptuPath"] = _this->ptuFilename;
		config.release(true);
	}
}

void
RadiosondeDecoderModule::onTypeSelected(void *ctx, int selection)
{
	float bw;
	RadiosondeDecoderModule *_this = (RadiosondeDecoderModule*)ctx;

	/* Ensure that the selection is within bounds */
	if (selection >= (int)(sizeof(_this->supportedTypes)/sizeof(_this->supportedTypes[0]))) return;

	/* Spin down the currently active decoder first, so it can no longer write
	 * lastData, then reset it under lock. */
	if (_this->activeDecoder) _this->activeDecoder->stop();
	_this->activeDecoder = NULL;
	{
		std::lock_guard<std::mutex> lck(_this->lastDataMtx);
		_this->lastData.init();
	}

	/* If selection is negative, just stop here */
	if (selection < 0) return;
	_this->selectedType = selection;

	/* Save selection to config */
	config.acquire();
	config.conf[_this->name]["sondeType"] = selection;
	config.release(true);

	/* Get new bandwidth */
	bw = std::get<1>(_this->supportedTypes[selection]);

	/* Only recreate the VFO if the bandwidth actually changed. Recreating a
	 * VFO with the same name is unnecessary and races with the GUI thread
	 * that reads gui::waterfall.vfos. */
	if (!_this->vfo || bw != _this->currentBandwidth) {
		_this->fmDemod.stop();
		if (_this->vfo) sigpath::vfoManager.deleteVFO(_this->vfo);
		_this->vfo = sigpath::vfoManager.createVFO(_this->name, ImGui::WaterfallVFO::REF_CENTER, 0, bw, bw, 1000.0, bw, false);
		if (!_this->vfo || !_this->vfo->output) {
			flog::error("[radiosonde] Failed to (re)create VFO '{}'", _this->name);
			if (_this->vfo) { sigpath::vfoManager.deleteVFO(_this->vfo); _this->vfo = NULL; }
			_this->currentBandwidth = 0;
			return;
		}
		_this->vfo->setSnapInterval(SNAP_INTERVAL);
		/* Rebind the user-resize event handler on the new VFO. */
		_this->vfo->wtfVFO->onUserChangedBandwidth.bindHandler(&_this->onVfoBandwidthChangedHandler);
		_this->fmDemod.setInput(_this->vfo->output);
		_this->fmDemod.setBandwidth(bw/2.0f);
		_this->fmDemod.setSamplerate(bw);
		_this->fmDemod.start();
		_this->resampler.setInSamplerate(bw);
		_this->currentBandwidth = bw;
	}

	/* Spin up the appropriate decoder */
	_this->activeDecoder = std::get<2>(_this->supportedTypes[selection]);
	_this->activeDecoder->start();
}
/* }}} */

/* Handle user resizing the VFO with the mouse in the waterfall.
 *
 * The FM demodulator and the rational resampler are both reconfigured live
 * (their setBandwidth / setSamplerate / setInSamplerate methods are
 * thread-safe and handle restart internally). We do NOT recreate the VFO
 * here — the waterfall is already showing the new size, and recreating
 * would race with the GUI thread that just fired this event. */
void
RadiosondeDecoderModule::onVfoBandwidthChanged(double newBw, void *ctx)
{
	RadiosondeDecoderModule *_this = (RadiosondeDecoderModule*)ctx;
	if (newBw <= 0.0) return;
	if (newBw == _this->currentBandwidth) return;

	_this->fmDemod.setSamplerate(newBw);
	_this->fmDemod.setBandwidth(newBw / 2.0f);
	_this->resampler.setInSamplerate(newBw);
	_this->currentBandwidth = newBw;
}

/* Module exports {{{ */
MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/radiosonde_decoder_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
	return new RadiosondeDecoderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void *instance) {
	delete (RadiosondeDecoderModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}

/* }}} */
