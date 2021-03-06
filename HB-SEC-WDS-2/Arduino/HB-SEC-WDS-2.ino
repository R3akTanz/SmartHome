//---------------------------------------------------------
// HB-SEC-WDS-2
// Version 1.02
// (C) 2018-2020 Tom Major (Creative Commons)
// https://creativecommons.org/licenses/by-nc-sa/4.0/
// You are free to Share & Adapt under the following terms:
// Give Credit, NonCommercial, ShareAlike
// +++
// AskSin++ 2016-10-31 papa Creative Commons
//---------------------------------------------------------

//---------------------------------------------------------
// !! NDEBUG sollte aktiviert werden wenn die Sensorentwicklung und die Tests abgeschlossen sind und das Gerät in den 'Produktionsmodus' geht.
// Zum Beispiel im HB-UNI-Sensor1 werden bei aktiviertem BME280 und MAX44009 werden damit ca. 2,6 KBytes Flash und 100 Bytes RAM eingespart.
// Insbesondere die RAM-Einsparungen sind wichtig für die Stabilität / dynamische Speicherzuweisungen etc.
// Dies beseitigt dann auch die mögliche Arduino-Warnung 'Low memory available, stability problems may occur'.
//
//#define NDEBUG

//---------------------------------------------------------
// define this to read the device id, serial and device type from bootloader section
// #define USE_OTA_BOOTLOADER

#define EI_NOTEXTERNAL
#include <EnableInterrupt.h>
#include <AskSinPP.h>
#include <LowPower.h>
#include <Register.h>
#include <ThreeState.h>
#include "tmBattery.h"

// clang-format off
//---------------------------------------------------------
// User definitions
// Pins
#define CONFIG_BUTTON_PIN       8
#define LED_PIN                 4
#define SENS_PIN_WET            A3      // ADC sensor pin
#define SENS_PIN_WATER          A2      // ADC sensor pin
// Parameters
#define BAT_VOLT_LOW            24      // 2.4V
#define BAT_VOLT_CRITICAL       21      // 2.1V
#define MEASUREMENT_INTERVAL    120     // alle 2min messen
#define DETECTION_THRESHOLD     800     // Wasser-Erkennung, Vergleichswert ADC
#define PEERS_PER_CHANNEL       6       // number of available peers per channel
//---------------------------------------------------------
// clang-format on

//---------------------------------------------------------
// Schaltungsvariante und Pins für Batteriespannungsmessung, siehe README HB-UNI-Sensor1
//------------
// 1) Standard: tmBattery, UBatt = Betriebsspannung AVR
#define BAT_SENSOR tmBattery
//------------
// 2) für StepUp/StepDown: tmBatteryResDiv, sense pin A0, activation pin D9, Faktor = Rges/Rlow*1000, z.B. 470k/100k, Faktor 570k/100k*1000 = 5700
//#define BAT_SENSOR tmBatteryResDiv<A0, 9, 5700>
//------------
// 3) Echte Batteriespannungsmessung unter Last, siehe README und Thema "Babbling Idiot Protection"
// tmBatteryLoad: sense pin A0, activation pin D9, Faktor = Rges/Rlow*1000, z.B. 10/30 Ohm, Faktor 40/10*1000 = 4000, 200ms Belastung vor Messung
//#define BAT_SENSOR tmBatteryLoad<A0, 9, 4000, 200>


// all library classes are placed in the namespace 'as'
using namespace as;

// define all device properties
const struct DeviceInfo PROGMEM devinfo = {
    { 0x49, 0x29, 0xd3 },                // Device ID
    "WATERCHK_1",                        // Device Serial
    { 0x00, 0xb2 },                      // Device Model
    0x13,                                // Firmware Version
    as::DeviceType::ThreeStateSensor,    // Device Type
    { 0x01, 0x00 }                       // Info Bytes
};

//---------------------------------------------------------
// meine Wassermelder ADC extension
//
// Sensor pin Masse: 4.7k nach Masse
// Sensor pin Wet:   4.7k zum ADC Eingang SENS_PIN_WET (A3), 100k vom ADC Eingang nach +3V
// Sensor pin Water: 4.7k zum ADC Eingang SENS_PIN_WATER (A2), 100k vom ADC Eingang nach +3V
//
// ADC Werte mit 10mm Abstand zwischen den Sensorpins, Batteriespannung 3V:
// offen          1023
// gebrueckt      88
// Wasser         550-650
// Mineralwasser  ca. 550
// Sekt           ca. 500

//---------------------------------------------------------
class ADCPosition : public Position {
    uint8_t m_SensePinWet, m_SensePinWater;

public:
    ADCPosition()
        : m_SensePinWet(0)
        , m_SensePinWater(0)
    {
        _present = true;
    }

    void init(uint8_t adcPinWet, uint8_t adcPinWater)
    {
        m_SensePinWet   = adcPinWet;
        m_SensePinWater = adcPinWater;
        pinMode(m_SensePinWet, INPUT);
        digitalWrite(m_SensePinWet, LOW);    // kein pull-up
        pinMode(m_SensePinWater, INPUT);
        digitalWrite(m_SensePinWater, LOW);    // kein pull-up
    }

    void measure(__attribute__((unused)) bool async = false)
    {
        uint16_t adcWet   = measureChannel(m_SensePinWet);
        uint16_t adcWater = measureChannel(m_SensePinWater);

        DPRINT(F("ADC Wet:   "));
        DDECLN(adcWet);
        DPRINT(F("ADC Water: "));
        DDECLN(adcWater);

        if (adcWater < DETECTION_THRESHOLD) {
            _position = State::PosC;
            DPRINTLN(F("Status:    WATER"));
        } else if (adcWet < DETECTION_THRESHOLD) {
            _position = State::PosB;
            DPRINTLN(F("Status:    WET"));
        } else {
            _position = State::PosA;
            DPRINTLN(F("Status:    DRY"));
        }
    }

    uint16_t measureChannel(uint8_t pin)
    {
        // setup ADC: complete ADC init in case other modules have chamged this
        ADCSRA = 1 << ADEN | 1 << ADPS2 | 1 << ADPS1 | 1 << ADPS0;    // enable ADC, prescaler 128 = 62.5kHz ADC clock @8MHz (range 50..1000 kHz)
        ADMUX  = 1 << REFS0;                                          // AREF: AVCC with external capacitor at AREF pin
        ADCSRB = 0;
        uint8_t channel = pin - PIN_A0;
        ADMUX |= (channel & 0x0F);    // select channel
        delay(30);                    // load CVref 100nF, 5*Tau = 25ms

        // 1x dummy read, dann Mittelwert aus 4 samples
        uint16_t adc = 0;
        for (uint8_t i = 0; i < 5; i++) {
            ADCSRA |= 1 << ADSC;
            uint8_t timeout = 50;    // start ADC
            while (ADCSRA & (1 << ADSC)) {
                delayMicroseconds(10);
                timeout--;
                if (timeout == 0)
                    break;
            }
            if (i > 0) {
                adc += (ADC & 0x3FF);
            }
        }
        return (adc >> 2);
    }

    uint32_t interval() { return seconds2ticks(MEASUREMENT_INTERVAL); }
};

// --------------------------------------------------------
template <class HALTYPE, class List0Type, class List1Type, class List4Type, int PEERCOUNT>
class ADCThreeStateChannel : public ThreeStateGenericChannel<ADCPosition, HALTYPE, List0Type, List1Type, List4Type, PEERCOUNT> {
public:
    typedef ThreeStateGenericChannel<ADCPosition, HALTYPE, List0Type, List1Type, List4Type, PEERCOUNT> BaseChannel;

    ADCThreeStateChannel()
        : BaseChannel() {};
    ~ADCThreeStateChannel() {}

    void init(uint8_t adcpin1, uint8_t adcpin2)
    {
        BaseChannel::init();
        BaseChannel::possens.init(adcpin1, adcpin2);
    }
};

// --------------------------------------------------------
// Configure the used hardware
typedef AvrSPI<10, 11, 12, 13>                 SPIType;
typedef Radio<SPIType, 2>                      RadioType;
typedef StatusLed<LED_PIN>                     LedType;
typedef AskSin<LedType, BAT_SENSOR, RadioType> HalType;

DEFREGISTER(Reg0, MASTERID_REGS, DREG_CYCLICINFOMSG, DREG_LOWBATLIMIT, DREG_TRANSMITTRYMAX)
class WDSList0 : public RegList0<Reg0> {
public:
    WDSList0(uint16_t addr)
        : RegList0<Reg0>(addr)
    {
    }
    void defaults()
    {
        clear();
        cycleInfoMsg(true);
        lowBatLimit(BAT_VOLT_LOW);
        transmitDevTryMax(6);
    }
};

DEFREGISTER(Reg1, CREG_AES_ACTIVE, CREG_MSGFORPOS, CREG_EVENTFILTERTIME, CREG_TRANSMITTRYMAX)
class WDSList1 : public RegList1<Reg1> {
public:
    WDSList1(uint16_t addr)
        : RegList1<Reg1>(addr)
    {
    }
    void defaults()
    {
        clear();
        msgForPosA(1);    // DRY
        msgForPosB(3);    // WET
        msgForPosC(2);    // WATER
        aesActive(false);
        eventFilterTime(5);
        transmitTryMax(6);
    }
};

typedef ADCThreeStateChannel<HalType, WDSList0, WDSList1, DefList4, PEERS_PER_CHANNEL> ChannelType;

class DevType : public ThreeStateDevice<HalType, ChannelType, 1, WDSList0> {
public:
    typedef ThreeStateDevice<HalType, ChannelType, 1, WDSList0> TSDevice;
    DevType(const DeviceInfo& info, uint16_t addr)
        : TSDevice(info, addr)
    {
    }
    virtual ~DevType() {}

    virtual void configChanged()
    {
        TSDevice::configChanged();
        DPRINTLN(F("Config Changed: List0"));

        uint8_t cycInfo = this->getList0().cycleInfoMsg();
        DPRINT(F("cycleInfoMsg: "));
        DDECLN(cycInfo);

        uint8_t lowBatLimit = this->getList0().lowBatLimit();
        DPRINT(F("lowBatLimit: "));
        DDECLN(lowBatLimit);
        battery().low(lowBatLimit);

        uint8_t txDevTryMax = this->getList0().transmitDevTryMax();
        DPRINT(F("transmitDevTryMax: "));
        DDECLN(txDevTryMax);
    }
};

HalType               hal;
DevType               sdev(devinfo, 0x20);
ConfigButton<DevType> cfgBtn(sdev);

void setup()
{
    DINIT(57600, ASKSIN_PLUS_PLUS_IDENTIFIER);
    sdev.init(hal);
    sdev.channel(1).init(SENS_PIN_WET, SENS_PIN_WATER);
    buttonISR(cfgBtn, CONFIG_BUTTON_PIN);
    hal.battery.low(BAT_VOLT_LOW);
    hal.battery.critical(BAT_VOLT_CRITICAL);
    hal.battery.init(seconds2ticks(60ul * 60 * 24), sysclock);    // 1x Batt.messung täglich
    sdev.initDone();
}

void loop()
{
    bool worked = hal.runready();
    bool poll   = sdev.pollRadio();
    if (worked == false && poll == false) {
        // deep discharge protection
        // if we drop below critical battery level - switch off all and sleep forever
        if (hal.battery.critical()) {
            // this call will never return
            hal.activity.sleepForever(hal);
        }
        // if nothing to do - go sleep
        hal.activity.savePower<Sleep<>>(hal);
    }
}
