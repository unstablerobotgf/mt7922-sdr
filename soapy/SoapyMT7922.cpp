// SPDX-License-Identifier: MIT
// Copyright (c) 2026 unstablerobotgf
// SoapySDR source for the MT7922 in SDR mode (/dev/mt7921_sdr, mt7921-common sdr=1).
// Raw words: I in bits 29:16, Q in bits 13:0, 14-bit two's complement, bits 1:0 always zero.
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Registry.hpp>

#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <stdexcept>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include "mt7921_sdr_ioctl.h"
}

static const char *DEV_PATH = "/dev/mt7921_sdr";
static const double RATES[] = {40e6, 80e6, 160e6};	// 160 exceeds PCIe 2.0 x1: gapped
static const double FMIN = 2390e6, FMAX = 2500e6;	// 2.4 GHz band only

class MT7922 : public SoapySDR::Device {
public:
	MT7922()
	{
		fd = ::open(DEV_PATH, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			throw std::runtime_error(std::string("open ") + DEV_PATH + ": " + strerror(errno));
	}
	~MT7922() override
	{
		if (streaming)
			::ioctl(fd, MT7921_SDR_STOP);
		::close(fd);
	}

	std::string getDriverKey() const override { return "mt7922"; }
	std::string getHardwareKey() const override { return "MT7922 ICAP"; }
	size_t getNumChannels(const int dir) const override { return dir == SOAPY_SDR_RX ? 1 : 0; }

	std::vector<std::string> getStreamFormats(const int, const size_t) const override
	{
		return {SOAPY_SDR_CS16, SOAPY_SDR_CF32};
	}
	std::string getNativeStreamFormat(const int, const size_t, double &fullScale) const override
	{
		fullScale = 32768;
		return SOAPY_SDR_CS16;
	}

	SoapySDR::Stream *setupStream(const int dir, const std::string &format,
				      const std::vector<size_t> &, const SoapySDR::Kwargs &) override
	{
		if (dir != SOAPY_SDR_RX)
			throw std::runtime_error("mt7922: RX only");
		if (format != SOAPY_SDR_CS16 && format != SOAPY_SDR_CF32)
			throw std::runtime_error("mt7922: CS16 or CF32 only");
		cf32 = format == SOAPY_SDR_CF32;
		return reinterpret_cast<SoapySDR::Stream *>(this);
	}
	void closeStream(SoapySDR::Stream *) override { stop(); }
	size_t getStreamMTU(SoapySDR::Stream *) const override { return 2048 * 16; }

	int activateStream(SoapySDR::Stream *, const int, const long long, const size_t) override
	{
		std::lock_guard<std::mutex> lock(mu);
		return start() ? 0 : SOAPY_SDR_STREAM_ERROR;
	}
	int deactivateStream(SoapySDR::Stream *, const int, const long long) override
	{
		std::lock_guard<std::mutex> lock(mu);
		stop();
		return 0;
	}

	int readStream(SoapySDR::Stream *, void *const *buffs, const size_t numElems, int &flags,
		       long long &, const long timeoutUs) override
	{
		flags = 0;
		if (!streaming)
			return SOAPY_SDR_TIMEOUT;
		size_t want = std::min(numElems, getStreamMTU(nullptr));

		(void)timeoutUs;	// read() blocks until a packet lands (one every 51 us at 40 MSps)
		raw.resize(want);
		ssize_t n = ::read(fd, raw.data(), want * 4);
		if (n < 0)
			return errno == EINTR ? SOAPY_SDR_TIMEOUT : SOAPY_SDR_STREAM_ERROR;
		if (n == 0)
			return SOAPY_SDR_TIMEOUT;
		size_t got = n / 4;
		if (cf32) {
			float *o = static_cast<float *>(buffs[0]);
			for (size_t k = 0; k < got; k++) {
				o[2 * k] = sx(raw[k] >> 16) * (1.0f / 8192);
				o[2 * k + 1] = sx(raw[k]) * (1.0f / 8192);
			}
		} else {
			int16_t *o = static_cast<int16_t *>(buffs[0]);
			for (size_t k = 0; k < got; k++) {
				o[2 * k] = (int16_t)(sx(raw[k] >> 16) * 4);
				o[2 * k + 1] = (int16_t)(sx(raw[k]) * 4);
			}
		}
		return (int)got;
	}

	// Frequency: 2390..2500 MHz, retuned live (A-die writes only, the stream keeps running).
	std::vector<std::string> listFrequencies(const int, const size_t) const override { return {"RF"}; }
	void setFrequency(const int, const size_t, const std::string &, const double frequency,
			  const SoapySDR::Kwargs &) override
	{
		std::lock_guard<std::mutex> lock(mu);
		uint64_t hz = (uint64_t)std::llround(std::min(std::max(frequency, FMIN), FMAX));

		if (::ioctl(fd, MT7921_SDR_TUNE, &hz) < 0)
			SoapySDR::logf(SOAPY_SDR_ERROR, "mt7922: TUNE %llu Hz: %s", (unsigned long long)hz,
				       strerror(errno));
		else
			freq = (double)hz;
	}
	double getFrequency(const int, const size_t, const std::string &) const override { return freq; }
	SoapySDR::RangeList getFrequencyRange(const int, const size_t, const std::string &) const override
	{
		return {SoapySDR::Range(FMIN, FMAX)};
	}

	std::vector<double> listSampleRates(const int, const size_t) const override
	{
		return std::vector<double>(std::begin(RATES), std::end(RATES));
	}
	void setSampleRate(const int, const size_t, const double r) override
	{
		double best = RATES[0];
		for (double x : RATES)
			if (std::fabs(x - r) < std::fabs(best - r))
				best = x;
		reconfigure(best, freq);
	}
	double getSampleRate(const int, const size_t) const override { return rate; }
	std::vector<double> listBandwidths(const int, const size_t) const override
	{
		return std::vector<double>(std::begin(RATES), std::end(RATES));
	}
	double getBandwidth(const int, const size_t) const override { return rate; }

	// Gain: the PHY's AGC (automatic; it hunts even on pure noise, 400-1300 rms), or manual:
	// LNA 0..4 (~6 dB steps), LPF 0..9 (~2 dB), DIG -32..+31 (signed 6-bit, ~1 dB per step, measured).
	// A change while streaming costs a stop/start of a few ms in the kernel.
	std::vector<std::string> listGains(const int, const size_t) const override
	{
		return {"LNA", "LPF", "DIG"};
	}
	SoapySDR::Range getGainRange(const int, const size_t, const std::string &name) const override
	{
		if (name == "DIG")
			return SoapySDR::Range(-32, 31, 1);
		return SoapySDR::Range(0, name == "LNA" ? 4 : 9, 1);
	}
	SoapySDR::Range getGainRange(const int, const size_t) const override { return SoapySDR::Range(-32, 44, 1); }
	void setGain(const int, const size_t, const std::string &name, const double value) override
	{
		std::lock_guard<std::mutex> lock(mu);
		unsigned v = (unsigned)std::lround(std::max(0.0, value));
		long s = std::lround(value);

		if (name == "LNA")
			gain.lna = std::min(v, 4u);
		else if (name == "LPF")
			gain.lpf = std::min(v, 9u);
		else if (name == "DIG")
			gain.digital = (int32_t)std::min(std::max(s, -32L), 31L);
		if (gain.manual)
			applyGain();
	}
	double getGain(const int, const size_t, const std::string &name) const override
	{
		if (name == "DIG")
			return gain.digital;
		return name == "LNA" ? gain.lna : gain.lpf;
	}
	bool hasGainMode(const int, const size_t) const override { return true; }
	bool getGainMode(const int, const size_t) const override { return !gain.manual; }
	void setGainMode(const int, const size_t, const bool automatic) override
	{
		std::lock_guard<std::mutex> lock(mu);

		if (gain.manual == !automatic)
			return;
		gain.manual = !automatic;
		applyGain();
	}

	std::vector<std::string> listAntennas(const int, const size_t) const override { return {"RX"}; }
	std::string getAntenna(const int, const size_t) const override { return "RX"; }

private:
	static inline int sx(uint32_t v) { return (int)((v & 0x3fff) ^ 0x2000) - 0x2000; }

	void applyGain()
	{
		if (::ioctl(fd, MT7921_SDR_GAIN, &gain) < 0)
			SoapySDR::logf(SOAPY_SDR_ERROR, "mt7922: GAIN: %s", strerror(errno));
	}

	bool start()
	{
		struct mt7921_sdr_start st = {(uint32_t)(rate / 1e6), (uint32_t)std::lround(freq / 1e6)};
		uint64_t hz = (uint64_t)std::llround(freq);

		if (streaming)
			return true;
		if (::ioctl(fd, MT7921_SDR_START, &st) < 0) {
			SoapySDR::logf(SOAPY_SDR_ERROR, "mt7922: START %u MSps %u MHz: %s",
				       st.rate_msps, st.freq_mhz, strerror(errno));
			return false;
		}
		if (hz != (uint64_t)st.freq_mhz * 1000000)	/* START takes whole MHz */
			::ioctl(fd, MT7921_SDR_TUNE, &hz);
		streaming = true;
		return true;
	}
	void stop()
	{
		if (!streaming)
			return;
		::ioctl(fd, MT7921_SDR_STOP);
		uint32_t junk[2048];
		while (::read(fd, junk, sizeof(junk)) > 0)
			;	// drain what the ring still holds
		streaming = false;
	}
	// Retune or change rate: a stop/start gap of a few ms (the analog state is replayed).
	void reconfigure(double r, double f)
	{
		std::lock_guard<std::mutex> lock(mu);
		bool was = streaming;

		if (r == rate && f == freq)
			return;
		stop();
		rate = r;
		freq = f;
		if (was)
			start();
	}

	int fd = -1;
	bool streaming = false, cf32 = false;
	double rate = 40e6, freq = 2437e6;
	struct mt7921_sdr_gain gain = {0, 4, 9, 16};	// AGC; manual starts at LNA 4, LPF 9, DIG +16
	std::vector<uint32_t> raw;
	std::mutex mu;
};

static SoapySDR::KwargsList find(const SoapySDR::Kwargs &args)
{
	if (args.count("driver") && args.at("driver") != "mt7922")
		return {};
	if (::access(DEV_PATH, R_OK) != 0)
		return {};
	SoapySDR::Kwargs k;
	k["driver"] = "mt7922";
	k["label"] = "MT7922 (mt7921e SDR mode)";
	return {k};
}

static SoapySDR::Device *make(const SoapySDR::Kwargs &) { return new MT7922(); }

static SoapySDR::Registry reg("mt7922", &find, &make, SOAPY_SDR_ABI_VERSION);
