#include "router.hh"
#include "debug.hh"

#include <iostream>

using namespace std;

// route_prefix: The "up-to-32-bit" IPv4 address prefix to match the datagram's destination address against
// prefix_length: For this route to be applicable, how many high-order (most-significant) bits of
//    the route_prefix will need to match the corresponding bits of the datagram's destination address?
// next_hop: The IP address of the next hop. Will be empty if the network is directly attached to the router (in
//    which case, the next hop address should be the datagram's final destination).
// interface_num: The index of the interface to send the datagram out on.
void Router::add_route( const uint32_t route_prefix,
                        const uint8_t prefix_length,
                        const optional<Address> next_hop,
                        const size_t interface_num )
{
  cerr << "DEBUG: adding route " << Address::from_ipv4_numeric( route_prefix ).ip() << "/"
       << static_cast<int>( prefix_length ) << " => " << ( next_hop.has_value() ? next_hop->ip() : "(direct)" )
       << " on interface " << interface_num << "\n";

  // Add the route to the routing table
  routing_table_.push_back( { route_prefix, prefix_length, next_hop, interface_num } );
}

// Go through all the interfaces, and route every incoming datagram to its proper outgoing interface.
void Router::route()
{
  // Check all interfaces for incoming datagrams
  for ( auto& interface : interfaces_ ) {
    auto& datagrams_received = interface->datagrams_received();
    
    // Process all received datagrams
    while ( !datagrams_received.empty() ) {
      InternetDatagram datagram = std::move( datagrams_received.front() );
      datagrams_received.pop();
      
      // Decrement TTL
      if ( datagram.header.ttl <= 1 ) {
        // TTL is 0 or would become 0, drop the datagram
        continue;
      }
      datagram.header.ttl--;
      
      // Recompute checksum after TTL change
      datagram.header.compute_checksum();
      
      // Find the longest-prefix-match route
      const RouteEntry* best_route = nullptr;
      uint8_t longest_prefix = 0;
      
      for ( const auto& route : routing_table_ ) {
        // Check if this route matches the destination
        if ( route.prefix_length == 0 || 
             ( route.prefix_length <= 32 && 
               ((datagram.header.dst ^ route.route_prefix) >> (32 - route.prefix_length)) == 0 ) ) {
          // This route matches, check if it's more specific (longer prefix)
          if ( route.prefix_length >= longest_prefix ) {
            longest_prefix = route.prefix_length;
            best_route = &route;
          }
        }
      }
      
      // If no route found, drop the datagram
      if ( best_route == nullptr ) {
        continue;
      }
      
      // Determine the next hop address
      Address next_hop_addr = best_route->next_hop.has_value() 
                                ? *best_route->next_hop
                                : Address::from_ipv4_numeric( datagram.header.dst );
      
      // Send the datagram out the appropriate interface
      interfaces_[best_route->interface_num]->send_datagram( datagram, next_hop_addr );
    }
  }
}
