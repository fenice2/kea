// Copyright (C) 2018 Internet Systems Consortium, Inc. ("ISC")
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef DHCP_QUEUE_CONTROL_PARSER_H
#define DHCP_QUEUE_CONTROL_PARSER_H

#include <cc/data.h>
#include <cc/simple_parser.h>
#include <dhcpsrv/parsers/dhcp_parsers.h>

namespace isc {
namespace dhcp {

/// @brief Parser for the configuration of DHCP packet queue controls
///
/// This parser parses the "dhcp-queue-control" parameter which holds the
/// the configurable parameters that tailor DHCP packet queue behavior.
/// Currently "dhcp-queue-control" is treated as a map of arbitrary values,
/// with only one required value, "queue-type". This was done to
/// provide latitude for differing queue implementations.
///
/// This parser is used in both DHCPv4 and DHCPv6. Derived parsers
/// are not needed.
class DHCPQueueControlParser : public isc::data::SimpleParser {
public:

    /// @brief Constructor
    ///
    /// @param family AF_INET for DHCPv4 and AF_INET6 for DHCPv6.
    explicit DHCPQueueControlParser(const uint16_t family);

    /// @brief Parses content of the "dhcp-queue-control".
    ///
    /// @param values pointer to the content of parsed values
    ///
    /// @return A pointer to a newly constructed DHCPQueueControl populated
    /// with the parsed values
    ///
    /// @throw DhcpConfigError if any of the values are invalid.
    data::ElementPtr parse(const isc::data::ConstElementPtr& values);

private:
    /// @brief AF_INET for DHCPv4 and AF_INET6 for DHCPv6.
    int family_;
};

}
} // end of namespace isc

#endif // DHCP_QUEUE_CONTROL_PARSER_H