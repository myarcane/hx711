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

#include <cassert>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>
#include "../include/AbstractScale.h"
#include "../include/Mass.h"
#include "../include/Utility.h"
#include "../include/Value.h"

namespace HX711 {

AbstractScale::AbstractScale(
    const Mass::Unit massUnit,
    const Value refUnit,
    const Value offset) noexcept : 
        _massUnit(massUnit),
        _refUnit(refUnit),
        _offset(offset) {
}

void AbstractScale::setUnit(const Mass::Unit unit) noexcept {
    this->_massUnit = unit;
}

Mass::Unit AbstractScale::getUnit() const noexcept {
    return this->_massUnit;
}

Value AbstractScale::getReferenceUnit() const noexcept {
    return this->_refUnit;
}

void AbstractScale::setReferenceUnit(const Value refUnit) {

    if(refUnit == 0) {
        throw std::invalid_argument("reference unit cannot be 0");
    }

    this->_refUnit = refUnit;

}

Value AbstractScale::getOffset() const noexcept {
    return this->_offset;
}

void AbstractScale::setOffset(const Value offset) noexcept {
    this->_offset = offset;
}

double AbstractScale::normalise(const double v) const noexcept {
    assert(this->_refUnit != 0);
    return (v - this->_offset) / this->_refUnit;
}

double AbstractScale::read(const ReadType rt, const std::size_t samples) {

    if(samples == 0) {
        throw std::range_error("samples must be at least 1");
    }

    std::vector<Value> vals = this->getValues(samples);

    double val;

    switch(rt) {
        case ReadType::Median:
            val = Utility::median(&vals);
            break;
        case ReadType::Average:
            val = Utility::average(&vals);
            break;
        default:
            throw std::invalid_argument("unknown read type");
    }

    return this->normalise(val);

}

void AbstractScale::zero(const ReadType rt, const std::size_t samples) {

    if(samples == 0) {
        throw std::range_error("samples must be at least 1");
    }
    
    const Value backup = this->_refUnit;
    this->setReferenceUnit(1);
    this->_offset = static_cast<Value>(std::round(this->read(rt, samples)));
    this->setReferenceUnit(backup);

}

Mass AbstractScale::weight(const ReadType rt, const std::size_t samples) {
    return Mass(this->read(rt, samples), this->_massUnit);
}

};
