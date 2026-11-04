/* Blue Robotics Arduino MS5837-30BA Pressure/Temperature Sensor Library
------------------------------------------------------------
 
Title: Blue Robotics Arduino MS5837-30BA Pressure/Temperature Sensor Library

Description: This library provides utilities to communicate with and to
read data from the Measurement Specialties MS5837-30BA pressure/temperature 
sensor.

Authors: Rustom Jehangir, Blue Robotics Inc.
         Adam Šimko, Blue Robotics Inc.

-------------------------------
The MIT License (MIT)

Copyright (c) 2015 Blue Robotics Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-------------------------------*/ 

#ifndef KAPPABETA_MS5837_H
#define KAPPABETA_MS5837_H

#include "Arduino.h"
#include <Wire.h>

class KappaBetaMS5837 {
public:
	static const float Pa;
	static const float bar;
	static const float mbar;

	static const uint8_t MS5837_30BA;
	static const uint8_t MS5837_02BA;

	KappaBetaMS5837();

	bool init(TwoWire& wirePort = Wire);
	bool begin(TwoWire& wirePort = Wire) { return init(wirePort); }

	/** Set model. Valid options are KappaBetaMS5837::MS5837_30BA (default)
	 * and KappaBetaMS5837::MS5837_02BA.
	 */
	void setModel(uint8_t model);

	/** Provide the density of the working fluid in kg/m^3. Default is for 
	 * seawater. Should be 997 for freshwater.
	 */
	void setFluidDensity(float density);

	/** The read from I2C takes up to 40 ms, so use sparingly is possible.
	 */
	bool read(); // false invalidates all measurement getters

	/** Pressure returned in mbar or mbar*conversion rate.
	 */
	float pressure(float conversion = 1.0f);

	/** Temperature returned in deg C.
	 */
	float temperature();

	/** Depth returned in meters (valid for operation in incompressible
	 *  liquids only. Uses density that is set for fresh or seawater.
	 */
	float depth();

	/** Altitude returned in meters (valid for operation in air only).
	 */
	float altitude();

private:
	TwoWire* wire = nullptr;
	bool initialized = false;
	bool measurementValid = false;
	bool command(uint8_t value);
	bool readBytes(uint8_t commandValue, uint8_t* data, uint8_t length);
	bool readAdc(uint32_t& value);
	uint16_t C[8] = {};
	uint32_t D1 = 0, D2 = 0;
	int32_t TEMP = 0;
	int32_t P = 0;
	uint8_t _model = 0;

	float fluidDensity;

	/** Performs calculations per the sensor data sheet for conversion and
	 *  second order compensation.
	 */
	void calculate();

	uint8_t crc4(uint16_t n_prom[]);
};

#endif
