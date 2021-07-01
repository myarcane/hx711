// MIT License
//
// Copyright (c) 2021 Daniel Robertson
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "../include/HX711.h"
#include "../include/TimeoutException.h"
#include <thread>
#include <lgpio.h>
#include <sys/time.h>

namespace HX711 {

std::int32_t HX711::_convertFromTwosComplement(const std::int32_t val) noexcept {
    return -(val & 0x800000) + (val & 0x7fffff);
}

bool HX711::_isSaturated(const HX_VALUE v) {
    //Datasheet pg. 4
    return v == HX_MIN_VALUE || v == HX_MAX_VALUE;
}

bool HX711::_readBit() noexcept {

    //first, clock pin is set high to make DOUT ready to be read from
    ::lgGpioWrite(this->_gpioHandle, this->_clockPin, 1);

    //then delay for sufficient time to allow DOUT to be ready (0.1us)
    //this will also permit a sufficient amount of time for the clock
    //pin to remain high
    _delayMicroseconds(1);

    //at this stage, DOUT is ready and the clock pin has been held
    //high for sufficient amount of time, so read the bit value
    const bool bit = ::lgGpioRead(this->_gpioHandle, this->_dataPin) == 1;

    //the clock pin then needs to be held for at least 0.2us before
    //the next bit can be read
    ::lgGpioWrite(this->_gpioHandle, this->_clockPin, 0);
    _delayMicroseconds(1);

    return bit;

}

std::uint8_t HX711::_readByte() noexcept {

    std::uint8_t val = 0;

    //8 bits per byte...
    for(std::uint8_t i = 0; i < 8; ++i) {
        if(this->_bitFormat == Format::MSB) {
            val <<= 1;
            val |= this->_readBit();
        }
        else {
            val >>= 1;
            val |= this->_readBit() * 0x80;
        }
    }

    return val;

}

void HX711::_readRawBytes(std::uint8_t* bytes) {

    /**
     * Bytes are ready to be read from the HX711 when DOUT goes low. Therefore,
     * wait until this occurs.
     * Datasheet pg. 5
     * 
     * The - potential- issue is that DOUT going low does not appear to be
     * defined. It appears to occur whenever it is ready, whenever that is.
     * 
     * The code below should limit that to a reasonable time-frame of checking
     * after a predefined interval for a predefined number of attempts.
     */

    std::unique_lock<std::mutex> communication(this->_commLock);

    /**
     * When DOUT goes low, there is a minimum of 0.1us until the clock pin
     * can go high. T1 in Fig.2.
     * Datasheet pg. 5
     */
    _delayMicroseconds(1);

    //delcare array of bytes of sufficient size
    //uninitialised is fine; they'll be overwritten
    std::uint8_t raw[_BYTES_PER_CONVERSION_PERIOD];

    //then populate it with values from the hx711
    for(std::uint8_t i = 0; i < _BYTES_PER_CONVERSION_PERIOD; ++i) {
        raw[i] = this->_readByte();
    }

    /**
     * The HX711 requires a certain number of "positive clock
     * pulses" depending on the set gain value.
     * Datasheet pg. 4
     * 
     * The expression below calculates the number of pulses
     * after having read the three bytes above. For example,
     * a gain of 128 requires 25 pulses: 24 pulses were made
     * when reading the three bytes (3 * 8), so only one
     * additional pulse is needed.
     */
    const std::uint8_t pulsesNeeded = 
        PULSES[static_cast<std::size_t>(this->_gain)] -
            8 * _BYTES_PER_CONVERSION_PERIOD;

    for(std::uint8_t i = 0; i < pulsesNeeded; ++i) {
        this->_readBit();
    }

    //not reading from the sensor any more so no need to keep
    //the lock in place
    communication.unlock();

    //if no byte pointer is given, don't try to write to it
    if(bytes == nullptr) {
        return;
    }

    /**
     * The HX711 will supply bits in big-endian format;
     * the 0th read bit is the MSB.
     * Datasheet pg. 4
     * 
     * If this->_byteFormat indicates the HX711 is outputting
     * bytes in LSB format, swap the first and last bytes
     * 
     * Remember, the bytes param expects an array of bytes
     * which will be converted to an int.
     */
    if(this->_byteFormat == Format::LSB) {
        const std::uint8_t swap = raw[0];
        raw[0] = raw[_BYTES_PER_CONVERSION_PERIOD - 1];
        raw[_BYTES_PER_CONVERSION_PERIOD - 1] = swap;
    }

    //finally, copy the local raw bytes to the byte array
    for(std::uint8_t i = 0; i < _BYTES_PER_CONVERSION_PERIOD; ++i) {
        bytes[i] = raw[i];
    }

}

HX_VALUE HX711::_readInt() {

    std::uint8_t bytes[_BYTES_PER_CONVERSION_PERIOD];
    
    this->_readRawBytes(bytes);

    /**
     * An int (int32_t) is 32 bits (4 bytes), but
     * the HX711 only uses 24 bits (3 bytes).
     */
    const std::int32_t twosComp = ((       0 << 24) |
                                   (bytes[0] << 16) |
                                   (bytes[1] << 8)  |
                                    bytes[2]         );

    return _convertFromTwosComplement(twosComp);

}

void HX711::_delayMicroseconds(const unsigned int us) noexcept {

    /**
     * This requires some explanation.
     * 
     * Delays on a pi are inconsistent due to the OS not being a real-time OS.
     * A previous version of this code used wiringPi which used its
     * delayMicroseconds function to delay in the microsecond range. The way this
     * was implemented was with a busy-wait loop for times under 100 nanoseconds.
     * 
     * https://github.com/WiringPi/WiringPi/blob/f15240092312a54259a9f629f9cc241551f9faae/wiringPi/wiringPi.c#L2165-L2166
     * https://github.com/WiringPi/WiringPi/blob/f15240092312a54259a9f629f9cc241551f9faae/wiringPi/wiringPi.c#L2153-L2154
     * 
     * This (the busy-wait) would, presumably, help to prevent context switching
     * therefore keep the timing required by the HX711 module relatively
     * consistent.
     * 
     * When this code changed to using the lgpio library, its lguSleep function
     * appeared to be an equivalent replacement. But it did not work.
     * 
     * http://abyz.me.uk/lg/lgpio.html#lguSleep
     * https://github.com/joan2937/lg/blob/8f385c9b8487e608aeb4541266cc81d1d03514d3/lgUtil.c#L56-L67
     * 
     * The problem appears to be that lguSleep is not busy-waiting. And, when
     * a sleep occurs, it is taking far too long to return. Contrast this
     * behaviour with wiringPi, which constantly calls gettimeofday until return.
     * 
     * In short, use this function for delays under 100us.
     */

    struct timeval tNow;
    struct timeval tLong;
    struct timeval tEnd;

    tLong.tv_sec = us / 1000000;
    tLong.tv_usec = us % 1000000;

    ::gettimeofday(&tNow, nullptr);
    timeradd(&tNow, &tLong, &tEnd);

    while(timercmp(&tNow, &tEnd, <)) {
        ::gettimeofday(&tNow, nullptr);
    }

}

void HX711::_watchPin() noexcept {
    for(;;) {
    
        if(this->_watchState == PinWatchState::END) {
            break;
        }
        else if(this->_watchState == PinWatchState::PAUSE) {
            std::this_thread::yield();
            continue;
        }

        std::unique_lock<std::mutex> ready(this->_readyLock);

        if(!this->isReady()) {
            ready.unlock();
            ::lguSleep(
                static_cast<double>(this->_notReadySleep.count()) / 
                decltype(this->_notReadySleep)::period::den);
            continue;
        }

        const HX_VALUE v = this->_readInt();

        if(_isSaturated(v)) {
            ready.unlock();
            ::lguSleep(
                static_cast<double>(this->_saturatedSleep.count()) / 
                decltype(this->_saturatedSleep)::period::den);
            continue;
        }

        this->_lastVal = v;
        this->_dataReady.notify_all();
        ready.unlock();
        
        ::lguSleep(
            static_cast<double>(this->_pollSleep.count()) / 
            decltype(this->_pollSleep)::period::den);

    }
}

HX711::HX711(const int dataPin, const int clockPin) noexcept :
    _gpioHandle(-1),
    _dataPin(dataPin),
    _clockPin(clockPin),
    _maxWait(_DEFAULT_MAX_WAIT),
    _lastVal(HX_MAX_VALUE),
    _watchState(PinWatchState::NONE),
    _notReadySleep(_DEFAULT_NOT_READY_SLEEP),
    _saturatedSleep(_DEFAULT_SATURATED_SLEEP),
    _pollSleep(_DEFAULT_POLL_SLEEP),
    _channel(Channel::A),
    _gain(Gain::GAIN_128),
    _bitFormat(Format::MSB),
    _byteFormat(Format::MSB) {
}

HX711::~HX711() {
    this->_watchState = PinWatchState::END;
    ::lgGpioFree(this->_gpioHandle, this->_clockPin);
    ::lgGpioFree(this->_gpioHandle, this->_dataPin);
    ::lgGpiochipClose(this->_gpioHandle);
}

void HX711::begin() {

    if(!(   
        (this->_gpioHandle = ::lgGpiochipOpen(0)) >= 0 &&
        ::lgGpioClaimInput(this->_gpioHandle, 0, this->_dataPin) == 0 &&
        ::lgGpioClaimOutput(this->_gpioHandle, 0, this->_clockPin, 0) == 0
    )) {
        throw std::runtime_error("unable to access GPIO");
    }

    this->powerDown();
    this->powerUp();

    /**
     * Cannot simply set this->_gain. this->setConfig()
     * must be called to set the HX711 module at the
     * hardware-level.
     * 
     * If, for whatever reason, the sensor cannot be
     * reached, setGain will fail and throw a
     * TimeoutException.
     * 
     * try {
     *     hx.begin();
     * }
     * catch(const TimeoutException& e) {
     *     //sensor failed to connect
     * }
     */
    this->setConfig(this->_channel, this->_gain);

    std::thread(&HX711::_watchPin, this).detach();

}

void HX711::setMaxWaitTime(const std::chrono::nanoseconds maxWait) noexcept {
    this->_maxWait = maxWait;
}

bool HX711::isReady() noexcept {

    /**
     * The datasheet states DOUT is used to shift-out data,
     * and in the process DOUT will either be HIGH or LOW
     * to represent bits of the resulting integer. The issue
     * is that during the "conversion period" of shifting
     * bits out, DOUT could be LOW, but not necessarily
     * mean there is "new" data for retrieval. Page 4 states
     * that the "25th pulse at PD_SCK input will pull DOUT
     * pin back to high".
     * 
     * This is justification enough to guard against
     * potentially erroneous "ready" states while a
     * conversion is in progress. The lock is already in
     * place to prevent extra reads from the sensor, so
     * it should suffice to stop this issue as well.
     */
    //std::lock_guard<std::mutex> lock(this->_commLock);

    /**
     * HX711 will be "ready" when DOUT is low.
     * "Ready" means "data is ready for retrieval".
     * Datasheet pg. 4
     * 
     * This should be a one-shot test. Any follow-ups
     * or looping for checking if the sensor is ready
     * over time can/should be done by other calling code
     */
    return ::lgGpioRead(this->_gpioHandle, this->_dataPin) == 0;

}

std::vector<Timing> HX711::testTiming(const size_t samples) noexcept {

    using namespace std::chrono;

    std::vector<Timing> vec;
    vec.reserve(samples);

    for(size_t i = 0; i < samples; ++i) {

        Timing t;

        t.begin = high_resolution_clock::now();
        
        while(!this->isReady());
        t.ready = high_resolution_clock::now();

        this->_readInt();
        t.end = high_resolution_clock::now();

        while(!this->isReady()) ;
        t.nextbegin = high_resolution_clock::now();

        vec.push_back(t);

    }

    return vec;

}

HX_VALUE HX711::getValue() {

    std::unique_lock<std::mutex> ready(this->_readyLock);

    if(this->_dataReady.wait_for(ready, this->_maxWait) == std::cv_status::timeout) {
        throw TimeoutException("timed out while trying to read from HX711");
    }

    return this->_lastVal;

}

int HX711::getDataPin() const noexcept {
    return this->_dataPin;
}

int HX711::getClockPin() const noexcept {
    return this->_clockPin;
}

Channel HX711::getChannel() const noexcept {
    return this->_channel;
}

Gain HX711::getGain() const noexcept {
    return this->_gain;
}

void HX711::setConfig(const Channel c, const Gain g) {

    if(c == Channel::A && g == Gain::GAIN_32) {
        throw std::invalid_argument("Channel A can only use a gain of 128 or 64");
    }
    else if(c == Channel::B && g != Gain::GAIN_32) {
        throw std::invalid_argument("Channel B can only use a gain of 32");
    }

    const Channel backupChannel = this->_channel;
    const Gain backupGain = this->_gain;

    /**
     * If the attempt to set the gain fails, it should
     * revert back to whatever it was before
     */
    try {

        this->_channel = c;
        this->_gain = g;
        
        /**
         * A read must take place to set the gain at the
         * hardware level. See datasheet pg. 4 "Serial
         * Interface".
         */
        this->_readRawBytes();
        
    }
    catch(const TimeoutException& e) {
        this->_channel = backupChannel;
        this->_gain = backupGain;
        throw;
    }

}

Format HX711::getBitFormat() const noexcept {
    return this->_bitFormat;
}

Format HX711::getByteFormat() const noexcept {
    return this->_byteFormat;
}

void HX711::setBitFormat(const Format f) noexcept {
    this->_bitFormat = f;
}

void HX711::setByteFormat(const Format f) noexcept {
    this->_byteFormat = f;
}

void HX711::powerDown() noexcept {

    this->_watchState = PinWatchState::PAUSE;
    std::lock_guard<std::mutex> lock(this->_commLock);

    ::lgGpioWrite(this->_gpioHandle, this->_clockPin, 0);
    _delayMicroseconds(1);
    ::lgGpioWrite(this->_gpioHandle, this->_clockPin, 1);

    /**
     * "When PD_SCK pin changes from low to high
     * and stays at high for longer than 60µs, HX711
     * enters power down mode (Fig.3)."
     * Datasheet pg. 5
     */
    _delayMicroseconds(60);

}

void HX711::powerUp() {

    this->_watchState = PinWatchState::NORMAL;
    std::unique_lock<std::mutex> lock(this->_commLock);

    ::lgGpioWrite(this->_gpioHandle, this->_clockPin, 0);

    /**
     * "When PD_SCK returns to low,
     * chip will reset and enter normal operation mode"
     * Datasheet pg. 5
     */

    lock.unlock();

    /**
     * "After a reset or power-down event, input
     * selection is default to Channel A with a gain of
     * 128."
     * Datasheet pg. 5
     * 
     * This means the following statement to set the gain
     * is needed ONLY IF the current gain isn't 128
     */
    if(this->_gain != Gain::GAIN_128) {
        this->setConfig(this->_channel, this->_gain);
    }

}

};