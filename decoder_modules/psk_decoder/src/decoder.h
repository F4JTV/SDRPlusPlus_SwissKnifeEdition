#pragma once
#include <signal_path/vfo_manager.h>

// Sideband / demodulation applied to the VFO before the FLDIGI engine runs.
// This mirrors how a real transceiver feeds audio into FLDIGI.
enum VfoMode {
    VFO_MODE_USB = 0,
    VFO_MODE_LSB = 1,
    VFO_MODE_NFM = 2
};

// Common interface for every FLDIGI mode decoder (BPSK first, then QPSK,
// RTTY, MFSK, ... can be added by implementing this interface).
class Decoder {
public:
    virtual ~Decoder() {}

    // Draw the mode-specific part of the menu.
    virtual void showMenu() {}

    // Re-bind to a (possibly new) VFO.
    virtual void setVFO(VFOManager::VFO* vfo) = 0;

    // Change the VFO demodulation mode (USB/LSB/NFM).
    virtual void setVfoMode(VfoMode mode) = 0;

    // Set the audio passband centre frequency (the "cursor" in FLDIGI).
    virtual void setAFFreq(double freq) = 0;

    // Squelch (gates the IQ stream on average power before demodulation).
    virtual void  setSquelchEnabled(bool en) = 0;
    virtual void  setSquelchLevel(float dB) = 0;
    virtual bool  getSquelchEnabled() const = 0;
    virtual float getSquelchLevel()   const = 0;

    // Auto-tune (one-shot AF freq search across the audio passband).
    // beginAutoTune() starts a ~2.5s scan; the caller should poll
    // isAutoTuning() and autoTuneProgress() each UI frame. When
    // autoTuneReady() returns true, autoTuneResult() returns the new AF
    // freq (and clears the ready flag); the decoder ALSO applies it
    // internally via setAFFreq().
    virtual void  beginAutoTune() = 0;
    virtual void  cancelAutoTune() = 0;
    virtual bool  isAutoTuning() const = 0;
    virtual float autoTuneProgress() const = 0;
    virtual bool  autoTuneReady() const = 0;
    virtual float autoTuneResult() = 0;

    // GUI band display. The decoder maintains a smoothed live magnitude
    // spectrum of the audio passband; the UI uses it to show the operator
    // where their signal sits.
    virtual int    getBandSpectrum(float* out, int n) const = 0;
    virtual double getBandFlo() const = 0;
    virtual double getBandFhi() const = 0;
    virtual double getSignalBandwidth() const = 0;  // expected width of the PSK signal around AF
    virtual float  getAFFreq() const = 0;

    virtual void start() = 0;
    virtual void stop() = 0;
};
