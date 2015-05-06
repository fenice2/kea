// Copyright (C) 2015 Internet Systems Consortium, Inc. ("ISC")
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
// REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
// AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
// INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
// LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
// OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
// PERFORMANCE OF THIS SOFTWARE.

#ifndef OBSERVATION_H
#define OBSERVATION_H

#include <boost/shared_ptr.hpp>
#include <exceptions/exceptions.h>
#include <cc/data.h>
#include <boost/date_time/time_duration.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <list>

namespace isc {
namespace stats {

/// @brief Exception thrown if invalid statistic type is used
///
/// For example statistic is of type duration, but methods using
/// it as integer are called.
class InvalidStatType : public Exception {
public:
    InvalidStatType(const char* file, size_t line, const char* what) :
        isc::Exception(file, line, what) {}
};

/// @brief Defines duration resolution
///
/// Boost offers a base boost::posix_time::time_duration class, that has specific
/// implementations: boost::posix_time::{hours,minutes,seconds,millisec,nanosec}.
/// For statistics purposes, the most appropriate choice seems to be milliseconds
/// precision, so we'll stick with that.
typedef boost::posix_time::millisec::time_duration StatsDuration;

/// @defgroup stat_samples Specifies supported observation types.
///
/// @brief The list covers all supported types of observations.
///
/// @{

/// @brief Integer (implemented as unsigned 64-bit integer)
typedef std::pair<uint64_t, boost::posix_time::ptime> IntegerSample;

/// @brief Float (implemented as double precision)
typedef std::pair<double, boost::posix_time::ptime> FloatSample;

/// @brief Time Duration
typedef std::pair<StatsDuration, boost::posix_time::ptime> DurationSample;

/// @brief String
typedef std::pair<std::string, boost::posix_time::ptime> StringSample;

/// @}

/// @brief Represents a single observable characteristic (a 'statistic')
///
/// Currently it supports one of four types: integer (implemented as unsigned 64
/// bit integer), float (implemented as double), time duration (implemented with
/// millisecond precision) and string. Absolute (setValue) and
/// incremental (addValue) modes are supported. Statistic type is determined
/// during its first use. Once type is set, any additional observations recorded
/// must be of the same type. Attempting to set or extract information about
/// other types will result in InvalidStateType exception.
///
/// Observation can be retrieved in one of @ref getInteger, @ref getFloat,
/// @ref getDuration, @ref getString (appropriate type must be used) or
/// @ref getJSON, which is generic and can be used for all types.
///
/// @todo: Eventually it will be possible to retain multiple samples for the same
/// observation, but that is outside of scope for 0.9.2.
class Observation {
 public:

    /// @brief type of available statistics
    ///
    /// Note that those will later be exposed using control socket. Therefore
    /// an easy to understand names were chosen (integer instead of uint64).
    /// To avoid confusion, we will support only one type of integer and only
    /// one type of floating points. Initially, these are represented by
    /// uint64_t and double. If convincing use cases appear to change them
    /// to something else, we may change the underlying type.
    enum Type {
        STAT_INTEGER, ///< this statistic is unsinged 64-bit integer value
        STAT_FLOAT,   ///< this statistic is a floating point value
        STAT_DURATION,///< this statistic represents time duration
        STAT_STRING   ///< this statistic represents a string
    };

    /// @brief Constructor for integer observations
    ///
    /// @param value integer value observed.
    Observation(uint64_t value);

    /// @brief Constructor for floating point observations
    ///
    /// @param value floating point value observed.
    Observation(double value);

    /// @brief Constructor for duration observations
    ///
    /// @param value duration observed.
    Observation(StatsDuration value);

    /// @brief Constructor for string observations
    ///
    /// @param value string observed.
    Observation(const std::string& value);

    /// @brief Records absolute integer observation
    ///
    /// @param value integer value observed
    /// @throw InvalidStatType if statistic is not integer
    void setValue(uint64_t value);

    /// @brief Records absolute floating point observation
    ///
    /// @param value floating point value observed
    /// @throw InvalidStatType if statistic is not fp
    void setValue(double value);

    /// @brief Records absolute duration observation
    ///
    /// @param value duration value observed
    /// @throw InvalidStatType if statistic is not time duration
    void setValue(StatsDuration duration);

    /// @brief Records absolute string observation
    ///
    /// @param value string value observed
    /// @throw InvalidStatType if statistic is not a string
    void setValue(const std::string& value = "");

    /// @brief Records incremental integer observation
    ///
    /// @param value integer value observed
    /// @throw InvalidStatType if statistic is not integer
    void addValue(uint64_t value = 1);

    /// @brief Records inremental floating point observation
    ///
    /// @param value floating point value observed
    /// @throw InvalidStatType if statistic is not fp
    void addValue(double value = 1.0f);

    /// @brief Records incremental duration observation
    ///
    /// @param value duration value observed
    /// @throw InvalidStatType if statistic is not time duration
    void addValue(StatsDuration value = StatsDuration(0,0,0,0));

    /// @brief Records incremental string observation.
    ///
    /// @param value string value observed
    /// @throw InvalidStatType if statistic is not a string
    void addValue(const std::string& value = "");

    /// @brief Resets statistic.
    ///
    /// Sets statistic to a neutral (0, 0.0 or "") value.
    void reset();

    /// @brief Returns statistic type
    /// @return statistic type
    Type getType() const {
        return (type_);
    }

    /// @brief Returns observed integer sample
    /// @return observed sample (value + timestamp)
    IntegerSample getInteger();

    /// @brief Returns observed float sample
    /// @return observed sample (value + timestamp)
    FloatSample getFloat();

    /// @brief Returns observed duration sample
    /// @return observed sample (value + timestamp)
    DurationSample getDuration();

    /// @brief Returns observed string sample
    /// @return observed sample (value + timestamp)
    StringSample getString();

    const std::list<IntegerSample>& getIntegerList() {
        return (integer_samples_);
    }

    const std::list<FloatSample>& getFloatList() {
        return (float_samples_);
    }

    const std::list<DurationSample>& getDurationList() {
        return (duration_samples_);
    }

    const std::list<StringSample>& getStringList() {
        return (string_samples_);
    }

    /// Returns as a JSON structure
    isc::data::ConstElementPtr getJSON();

    static std::string typeToText(Type type);

    static std::string ptimeToText(boost::posix_time::ptime time);

    static std::string durationToText(StatsDuration dur);

 protected:
    template<typename SampleType, typename StorageType>
        void setValueInternal(SampleType value, StorageType& storage,
            Type exp_type);

    template<typename SampleType, typename Storage>
    SampleType getValueInternal(Storage& storage, Type exp_type);

    std::string name_;
    Type type_;

    size_t max_samples_;

    std::list<IntegerSample> integer_samples_;
    std::list<FloatSample> float_samples_;
    std::list<DurationSample> duration_samples_;
    std::list<StringSample> string_samples_;
};

 typedef boost::shared_ptr<Observation> ObservationPtr;

};
};

#endif // OBSERVATION_H
