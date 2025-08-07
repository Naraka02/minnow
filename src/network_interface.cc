#include <iostream>

#include "arp_message.hh"
#include "debug.hh"
#include "ethernet_frame.hh"
#include "exception.hh"
#include "helpers.hh"
#include "network_interface.hh"

using namespace std;

//! \param[in] ethernet_address Ethernet (what ARP calls "hardware") address of the interface
//! \param[in] ip_address IP (what ARP calls "protocol") address of the interface
NetworkInterface::NetworkInterface( string_view name,
                                    shared_ptr<OutputPort> port,
                                    const EthernetAddress& ethernet_address,
                                    const Address& ip_address )
  : name_( name )
  , port_( notnull( "OutputPort", move( port ) ) )
  , ethernet_address_( ethernet_address )
  , ip_address_( ip_address )
{
  cerr << "DEBUG: Network interface has Ethernet address " << to_string( ethernet_address_ ) << " and IP address "
       << ip_address.ip() << "\n";
}

//! \param[in] dgram the IPv4 datagram to be sent
//! \param[in] next_hop the IP address of the interface to send it to (typically a router or default gateway, but
//! may also be another host if directly connected to the same network as the destination) Note: the Address type
//! can be converted to a uint32_t (raw 32-bit IP address) by using the Address::ipv4_numeric() method.
void NetworkInterface::send_datagram( const InternetDatagram& dgram, const Address& next_hop )
{
  const uint32_t next_hop_ip = next_hop.ipv4_numeric();
  
  // Check if we already know the Ethernet address for this IP
  auto arp_entry = arp_table_.find( next_hop_ip );
  if ( arp_entry != arp_table_.end() ) {
    // We know the Ethernet address, send the frame immediately
    EthernetFrame frame;
    frame.header.dst = arp_entry->second.first;
    frame.header.src = ethernet_address_;
    frame.header.type = EthernetHeader::TYPE_IPv4;
    frame.payload = serialize( dgram );
    transmit( frame );
    return;
  }
  
  // We don't know the Ethernet address, queue the datagram
  pending_datagrams_[next_hop_ip].push( dgram );
  
  // Check if we recently sent an ARP request for this IP (within 5 seconds)
  auto recent_request = recent_arp_requests_.find( next_hop_ip );
  if ( recent_request != recent_arp_requests_.end() && 
       current_time_ms_ - recent_request->second < 5000 ) {
    // Already sent ARP request recently, just wait
    return;
  }
  
  // Send ARP request
  ARPMessage arp_request;
  arp_request.opcode = ARPMessage::OPCODE_REQUEST;
  arp_request.sender_ethernet_address = ethernet_address_;
  arp_request.sender_ip_address = ip_address_.ipv4_numeric();
  arp_request.target_ethernet_address = {}; // Unknown, set to all zeros
  arp_request.target_ip_address = next_hop_ip;
  
  EthernetFrame arp_frame;
  arp_frame.header.dst = ETHERNET_BROADCAST;
  arp_frame.header.src = ethernet_address_;
  arp_frame.header.type = EthernetHeader::TYPE_ARP;
  arp_frame.payload = serialize( arp_request );
  transmit( arp_frame );
  
  // Record that we sent this ARP request
  recent_arp_requests_[next_hop_ip] = current_time_ms_;
}

//! \param[in] frame the incoming Ethernet frame
void NetworkInterface::recv_frame( EthernetFrame frame )
{
  // Check if frame is destined for us (our address or broadcast)
  if ( frame.header.dst != ethernet_address_ && frame.header.dst != ETHERNET_BROADCAST ) {
    return; // Not for us, ignore
  }
  
  if ( frame.header.type == EthernetHeader::TYPE_IPv4 ) {
    // Parse as IPv4 datagram
    InternetDatagram dgram;
    if ( parse( dgram, frame.payload ) ) {
      datagrams_received_.push( std::move( dgram ) );
    }
  }
  else if ( frame.header.type == EthernetHeader::TYPE_ARP ) {
    // Parse as ARP message
    ARPMessage arp_msg;
    if ( parse( arp_msg, frame.payload ) && arp_msg.supported() ) {
      // Learn the mapping from sender's IP and Ethernet address
      arp_table_[arp_msg.sender_ip_address] = { arp_msg.sender_ethernet_address, current_time_ms_ };
      
      // Send any pending datagrams for this IP
      auto pending = pending_datagrams_.find( arp_msg.sender_ip_address );
      if ( pending != pending_datagrams_.end() ) {
        while ( !pending->second.empty() ) {
          InternetDatagram dgram = std::move( pending->second.front() );
          pending->second.pop();
          
          EthernetFrame reply_frame;
          reply_frame.header.dst = arp_msg.sender_ethernet_address;
          reply_frame.header.src = ethernet_address_;
          reply_frame.header.type = EthernetHeader::TYPE_IPv4;
          reply_frame.payload = serialize( dgram );
          transmit( reply_frame );
        }
        pending_datagrams_.erase( pending );
      }
      
      // If it's an ARP request asking for our IP, send a reply
      if ( arp_msg.opcode == ARPMessage::OPCODE_REQUEST && 
           arp_msg.target_ip_address == ip_address_.ipv4_numeric() ) {
        ARPMessage arp_reply;
        arp_reply.opcode = ARPMessage::OPCODE_REPLY;
        arp_reply.sender_ethernet_address = ethernet_address_;
        arp_reply.sender_ip_address = ip_address_.ipv4_numeric();
        arp_reply.target_ethernet_address = arp_msg.sender_ethernet_address;
        arp_reply.target_ip_address = arp_msg.sender_ip_address;
        
        EthernetFrame reply_frame;
        reply_frame.header.dst = arp_msg.sender_ethernet_address;
        reply_frame.header.src = ethernet_address_;
        reply_frame.header.type = EthernetHeader::TYPE_ARP;
        reply_frame.payload = serialize( arp_reply );
        transmit( reply_frame );
      }
    }
  }
}

//! \param[in] ms_since_last_tick the number of milliseconds since the last call to this method
void NetworkInterface::tick( const size_t ms_since_last_tick )
{
  current_time_ms_ += ms_since_last_tick;
  
  // Expire ARP table entries older than 30 seconds (30,000 ms)
  auto arp_it = arp_table_.begin();
  while ( arp_it != arp_table_.end() ) {
    if ( current_time_ms_ - arp_it->second.second >= 30000 ) {
      arp_it = arp_table_.erase( arp_it );
    } else {
      ++arp_it;
    }
  }
  
  // Clean up old ARP request timestamps and drop corresponding pending datagrams (older than 5 seconds)
  auto arp_req_it = recent_arp_requests_.begin();
  while ( arp_req_it != recent_arp_requests_.end() ) {
    if ( current_time_ms_ - arp_req_it->second >= 5000 ) {
      // Drop pending datagrams for this IP since the ARP request expired
      pending_datagrams_.erase( arp_req_it->first );
      arp_req_it = recent_arp_requests_.erase( arp_req_it );
    } else {
      ++arp_req_it;
    }
  }
}
