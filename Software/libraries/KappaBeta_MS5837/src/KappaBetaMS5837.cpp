// Transport-checked fork of Blue Robotics MS5837 Arduino library 1.1.1.
// Original compensation and CRC retained; see LICENSE and README.md.
#include "KappaBetaMS5837.h"
#include <cmath>
#include <Wire.h>

#define MS5837_ADDR               0x76  
#define MS5837_RESET              0x1E
#define MS5837_ADC_READ           0x00
#define MS5837_PROM_READ          0xA0
#define MS5837_CONVERT_D1_8192    0x4A
#define MS5837_CONVERT_D2_8192    0x5A

const float KappaBetaMS5837::Pa = 100.0f;
const float KappaBetaMS5837::bar = 0.001f;
const float KappaBetaMS5837::mbar = 1.0f;

const uint8_t KappaBetaMS5837::MS5837_30BA = 0;
const uint8_t KappaBetaMS5837::MS5837_02BA = 1;

KappaBetaMS5837::KappaBetaMS5837() {
	fluidDensity = 1029;
}

bool KappaBetaMS5837::command(uint8_t value) {
    wire->beginTransmission(MS5837_ADDR);
    const bool written = wire->write(value) == 1;
    const uint8_t result = wire->endTransmission();
    return written && result == 0;
}

bool KappaBetaMS5837::readBytes(uint8_t commandValue, uint8_t* data, uint8_t length) {
    if (!command(commandValue)) return false;
    if (wire->requestFrom(MS5837_ADDR, length) != length) {
        while (wire->available()) wire->read();
        return false;
    }
    for (uint8_t i = 0; i < length; ++i) {
        if (!wire->available()) return false;
        const int value = wire->read();
        if (value < 0) return false;
        data[i] = static_cast<uint8_t>(value);
    }
    return true;
}

bool KappaBetaMS5837::init(TwoWire& wirePort) {
    wire = &wirePort;
    initialized = false;
    measurementValid = false;
    // Caller initializes the shared bus and its GPIOs. Never call Wire.begin here.
    if (!command(MS5837_RESET)) return false;
    delay(10);
    bool anyNonzero = false;
    bool anyNotOnes = false;
    for (uint8_t i = 0; i < 7; ++i) {
        uint8_t bytes[2];
        if (!readBytes(MS5837_PROM_READ + i * 2, bytes, 2)) return false;
        C[i] = (static_cast<uint16_t>(bytes[0]) << 8) | bytes[1];
        if (i > 0) {
            anyNonzero |= C[i] != 0;
            anyNotOnes |= C[i] != 0xffff;
        }
    }
    // A missing/stuck device must not pass simply because zero PROM has zero CRC.
    if (!anyNonzero || !anyNotOnes) return false;
    const uint8_t expected = C[0] >> 12;
    initialized = crc4(C) == expected;
    return initialized;
}

void KappaBetaMS5837::setModel(uint8_t model) {
	_model = model;
	measurementValid = false;
}

void KappaBetaMS5837::setFluidDensity(float density) {
	fluidDensity = density;
}

bool KappaBetaMS5837::readAdc(uint32_t& value) {
    uint8_t bytes[3];
    if (!readBytes(MS5837_ADC_READ, bytes, 3)) return false;
    value = (static_cast<uint32_t>(bytes[0]) << 16) |
            (static_cast<uint32_t>(bytes[1]) << 8) | bytes[2];
    return value != 0 && value != 0xffffff;
}

bool KappaBetaMS5837::read() {
    measurementValid = false;
    if (!initialized || !wire) return false;
    if (!command(MS5837_CONVERT_D1_8192)) return false;
    delay(20);
    if (!readAdc(D1)) return false;
    if (!command(MS5837_CONVERT_D2_8192)) return false;
    delay(20);
    if (!readAdc(D2)) return false;
    calculate();
    measurementValid = true;
    return true;
}

void KappaBetaMS5837::calculate() {
	// Given C1-C6 and D1, D2, calculated TEMP and P
	// Do conversion first and then second order temp compensation
	
	int32_t dT = 0;
	int64_t SENS = 0;
	int64_t OFF = 0;
	int64_t SENSi = 0;
	int64_t OFFi = 0;  
	int32_t Ti = 0;    
	int64_t OFF2 = 0;
	int64_t SENS2 = 0;
	
	// Terms called
	dT = static_cast<int32_t>(D2)-static_cast<int32_t>(C[5])*256;
	if ( _model == MS5837_02BA ) {
		SENS = int64_t(C[1])*65536l+(int64_t(C[3])*dT)/128l;
		OFF = int64_t(C[2])*131072l+(int64_t(C[4])*dT)/64l;
		P = (D1*SENS/(2097152l)-OFF)/(32768l);
	} else {
		SENS = int64_t(C[1])*32768l+(int64_t(C[3])*dT)/256l;
		OFF = int64_t(C[2])*65536l+(int64_t(C[4])*dT)/128l;
		P = (D1*SENS/(2097152l)-OFF)/(8192l);
	}
	
	// Temp conversion
	TEMP = 2000l+int64_t(dT)*C[6]/8388608LL;
	// Widen squared temperature differences even for corrupt but full-length ADC words.
	const int64_t temperatureDelta = int64_t(TEMP) - 2000;
	const int64_t coldDelta = int64_t(TEMP) + 1500;
	
	//Second order compensation
	if ( _model == MS5837_02BA ) {
		if((TEMP/100)<20){         //Low temp
			Ti = (11*int64_t(dT)*int64_t(dT))/(34359738368LL);
			OFFi = (31*temperatureDelta*temperatureDelta)/8;
			SENSi = (63*temperatureDelta*temperatureDelta)/32;
		}
	} else {
		if((TEMP/100)<20){         //Low temp
			Ti = (3*int64_t(dT)*int64_t(dT))/(8589934592LL);
			OFFi = (3*temperatureDelta*temperatureDelta)/2;
			SENSi = (5*temperatureDelta*temperatureDelta)/8;
			if((TEMP/100)<-15){    //Very low temp
				OFFi = OFFi+7*coldDelta*coldDelta;
				SENSi = SENSi+4*coldDelta*coldDelta;
			}
		}
		else if((TEMP/100)>=20){    //High temp
			// Widen before multiplying: the upstream expression overflows int32_t.
			Ti = 2*(int64_t(dT)*dT)/(137438953472LL);
			OFFi = (temperatureDelta*temperatureDelta)/16;
			SENSi = 0;
		}
	}
	
	OFF2 = OFF-OFFi;           //Calculate pressure and temp second order
	SENS2 = SENS-SENSi;
	
	TEMP = (TEMP-Ti);
	
	if ( _model == MS5837_02BA ) {
		P = (((D1*SENS2)/2097152l-OFF2)/32768l); 
	} else {
		P = (((D1*SENS2)/2097152l-OFF2)/8192l);
	}
}

float KappaBetaMS5837::pressure(float conversion) {
    if (!measurementValid) return NAN;
    if ( _model == MS5837_02BA ) {
        return P*conversion/100.0f;
    }
    else {
        return P*conversion/10.0f;
    }
}

float KappaBetaMS5837::temperature() {
    if (!measurementValid) return NAN;
	return TEMP/100.0f;
}

float KappaBetaMS5837::depth() {
	return (pressure(KappaBetaMS5837::Pa)-101300)/(fluidDensity*9.80665);
}

float KappaBetaMS5837::altitude() {
	return (1-pow((pressure()/1013.25),.190284))*145366.45*.3048;
}


uint8_t KappaBetaMS5837::crc4(uint16_t n_prom[]) {
	uint16_t n_rem = 0;

	n_prom[0] = ((n_prom[0]) & 0x0FFF);
	n_prom[7] = 0;

	for ( uint8_t i = 0 ; i < 16; i++ ) {
		if ( i%2 == 1 ) {
			n_rem ^= (uint16_t)((n_prom[i>>1]) & 0x00FF);
		} else {
			n_rem ^= (uint16_t)(n_prom[i>>1] >> 8);
		}
		for ( uint8_t n_bit = 8 ; n_bit > 0 ; n_bit-- ) {
			if ( n_rem & 0x8000 ) {
				n_rem = (n_rem << 1) ^ 0x3000;
			} else {
				n_rem = (n_rem << 1);
			}
		}
	}
	
	n_rem = ((n_rem >> 12) & 0x000F);

	return n_rem ^ 0x00;
}
