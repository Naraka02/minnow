#include "tcp_sender.hh"
#include "debug.hh"
#include "tcp_config.hh"

using namespace std;

// This function is for testing only; don't add extra state to support it.
uint64_t TCPSender::sequence_numbers_in_flight() const
{
  uint64_t total = 0;
  for ( const auto& [seqno, seg] : outstanding_segments_ ) {
    total += seg.message.sequence_length();
  }
  return total;
}

// This function is for testing only; don't add extra state to support it.
uint64_t TCPSender::consecutive_retransmissions() const
{
  uint64_t max_retransmissions_count = 0;
  for ( auto it = outstanding_segments_.begin(); it != outstanding_segments_.end(); ++it ) {
    if ( it->second.retransmissions_count > max_retransmissions_count ) {
      max_retransmissions_count = it->second.retransmissions_count;
    }
  }
  return max_retransmissions_count;
}

void TCPSender::push( const TransmitFunction& transmit )
{
  if ( !is_syn_sent_ ) {
    TCPSenderMessage syn_msg;
    syn_msg.SYN = true;
    syn_msg.seqno = isn_;

    int64_t curr_window_size = window_size_ > 0 ? window_size_ : 1;
    uint64_t window_remaining = curr_window_size;

    if ( window_remaining > 0 )
      window_remaining--;

    std::string payload;
    uint64_t max_payload = std::min( window_remaining, TCPConfig::MAX_PAYLOAD_SIZE );
    read( reader(), max_payload, payload );
    syn_msg.payload = payload;

    if ( !is_fin_sent_ && reader().is_finished() ) {
      syn_msg.FIN = true;
      is_fin_sent_ = true;
      last_seqno_ = isn_ + 2 + payload.size();
    } else {
      last_seqno_ = isn_ + 1 + payload.size();
    }
    outstanding_segments_[isn_.unwrap( isn_, 0 )]
      = TCPSegment { .message = syn_msg, .last_tick_ms = last_tick_ms_ };
    is_syn_sent_ = true;
    transmit( syn_msg );
    return;
  }

  uint64_t in_flight = sequence_numbers_in_flight();
  uint64_t curr_window_size = window_size_ > 0 ? window_size_ : 1;
  uint64_t window_remaining = ( curr_window_size > in_flight ) ? ( curr_window_size - in_flight ) : 0;

  while ( window_remaining > 0
          && ( !reader().is_finished() || ( !is_fin_sent_ && reader().is_finished() && window_remaining > 0 ) ) ) {
    TCPSenderMessage data_msg;
    data_msg.seqno = last_seqno_;
    std::string payload;
    uint64_t max_payload = std::min( window_remaining, TCPConfig::MAX_PAYLOAD_SIZE );

    read( reader(), max_payload, payload );
    bool can_fin
      = !is_fin_sent_ && reader().is_finished() && window_remaining > 0 && window_remaining > payload.size();

    if ( payload.empty() && !can_fin )
      break;

    data_msg.payload = payload;
    if ( can_fin ) {
      data_msg.FIN = true;
      is_fin_sent_ = true;
    }

    outstanding_segments_[last_seqno_.unwrap( isn_, writer().bytes_pushed() )]
      = TCPSegment { .message = data_msg, .last_tick_ms = last_tick_ms_ };
    last_seqno_ += data_msg.sequence_length();
    data_msg.RST = input_.has_error();
    transmit( data_msg );
    window_remaining -= data_msg.sequence_length();

    if ( can_fin )
      break;

  }
}

TCPSenderMessage TCPSender::make_empty_message() const
{
  return TCPSenderMessage { .seqno = last_seqno_, .RST = input_.has_error() };
}

void TCPSender::receive( const TCPReceiverMessage& msg )
{
  window_size_ = msg.window_size;
  if ( msg.RST ) {
    input_.set_error();
    return;
  }
  if ( !msg.ackno.has_value() )
    return;

  uint64_t abs_ackno = msg.ackno->unwrap( isn_, writer().bytes_pushed() );

  if ( abs_ackno > last_seqno_.unwrap( isn_, writer().bytes_pushed() ) )
    return;

  auto it = outstanding_segments_.begin();
  while ( it != outstanding_segments_.end() ) {
    uint64_t seg_start = it->first;
    uint64_t seg_end = seg_start + it->second.message.sequence_length();
    debug( "seg_end : {}, abs_ackno: {}", seg_end, abs_ackno );
    if ( seg_end <= abs_ackno ) {
      it = outstanding_segments_.erase( it );
    } else {
      ++it;
    }
  }

  if ( abs_ackno > last_ackno_.unwrap( isn_, writer().bytes_pushed() ) ) {
    last_ackno_ = Wrap32::wrap( abs_ackno, isn_ );
    RTO_ms_ = initial_RTO_ms_;
    if ( !outstanding_segments_.empty() ) {
      outstanding_segments_.begin()->second.last_tick_ms = last_tick_ms_;
      outstanding_segments_.begin()->second.retransmissions_count = 0;
    }
  }
}

void TCPSender::tick( uint64_t ms_since_last_tick, const TransmitFunction& transmit )
{
  last_tick_ms_ += ms_since_last_tick;

  if ( !outstanding_segments_.empty() ) {
    auto it = outstanding_segments_.begin();
    auto& oldest = it->second;
    if ( last_tick_ms_ - oldest.last_tick_ms >= RTO_ms_ ) {
      transmit( oldest.message );
      oldest.last_tick_ms = last_tick_ms_;
      if ( window_size_ > 0 ) {
        RTO_ms_ *= 2;
      }
      oldest.retransmissions_count += 1;
    }
  }
}

