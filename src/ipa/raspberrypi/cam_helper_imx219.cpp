/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2019, Raspberry Pi (Trading) Limited
 *
 * cam_helper_imx219.cpp - camera helper for imx219 sensor
 */

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * We have observed that the imx219 embedded data stream randomly returns junk
 * register values. Do not rely on embedded data until this has been resolved.
 */
#define ENABLE_EMBEDDED_DATA 0

#include "cam_helper.hpp"
#if ENABLE_EMBEDDED_DATA
#include "md_parser.hpp"
#endif

using namespace RPiController;

/*
 * We care about one gain register and a pair of exposure registers. Their I2C
 * addresses from the Sony IMX219 datasheet:
 */
constexpr uint32_t gainReg = 0x157;
constexpr uint32_t expHiReg = 0x15a;
constexpr uint32_t expLoReg = 0x15b;
constexpr uint32_t frameLengthHiReg = 0x160;
constexpr uint32_t frameLengthLoReg = 0x161;
constexpr std::initializer_list<uint32_t> registerList [[maybe_unused]]
	= { expHiReg, expLoReg, gainReg, frameLengthHiReg, frameLengthLoReg };

class CamHelperImx219 : public CamHelper
{
public:
	CamHelperImx219();
	uint32_t GainCode(double gain) const override;
	double Gain(uint32_t gain_code) const override;
	unsigned int MistrustFramesModeSwitch() const override;
	bool SensorEmbeddedDataPresent() const override;

private:
	/*
	 * Smallest difference between the frame length and integration time,
	 * in units of lines.
	 */
	static constexpr int frameIntegrationDiff = 4;

	void PopulateMetadata(const MdParser::RegisterMap &registers,
			      Metadata &metadata) const override;
};

CamHelperImx219::CamHelperImx219()
#if ENABLE_EMBEDDED_DATA
	: CamHelper(std::make_unique<MdParserSmia>(registerList), frameIntegrationDiff)
#else
	: CamHelper({}, frameIntegrationDiff)
#endif
{
}

uint32_t CamHelperImx219::GainCode(double gain) const
{
	return (uint32_t)(256 - 256 / gain);
}

double CamHelperImx219::Gain(uint32_t gain_code) const
{
	return 256.0 / (256 - gain_code);
}

unsigned int CamHelperImx219::MistrustFramesModeSwitch() const
{
	/*
	 * For reasons unknown, we do occasionally get a bogus metadata frame
	 * at a mode switch (though not at start-up). Possibly warrants some
	 * investigation, though not a big deal.
	 */
	return 1;
}

bool CamHelperImx219::SensorEmbeddedDataPresent() const
{
	return ENABLE_EMBEDDED_DATA;
}

void CamHelperImx219::PopulateMetadata(const MdParser::RegisterMap &registers,
				       Metadata &metadata) const
{
	DeviceStatus deviceStatus;

	deviceStatus.shutter_speed = Exposure(registers.at(expHiReg) * 256 + registers.at(expLoReg));
	deviceStatus.analogue_gain = Gain(registers.at(gainReg));
	deviceStatus.frame_length = registers.at(frameLengthHiReg) * 256 + registers.at(frameLengthLoReg);

	metadata.Set("device.status", deviceStatus);
}

static CamHelper *Create()
{
	return new CamHelperImx219();
}

static RegisterCamHelper reg("imx219", &Create);
