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
  for ( const auto& [seqno, seg] : outstanding_segments_ ) {
    max_retransmissions_count = max( max_retransmissions_count, seg.retransmissions_count );
  }
  return max_retransmissions_count;
}

void TCPSender::push( const TransmitFunction& transmit )
{
  // Calculate available window capacity
  Wrap32 window_end = last_ackno_ + ( window_size_ ? window_size_ : 1 );
  uint64_t capacity
    = window_end.unwrap( isn_, writer().bytes_pushed() ) - next_seqno_.unwrap( isn_, writer().bytes_pushed() );

  while ( capacity > 0 ) {
    TCPSenderMessage message;

    // Add SYN flag if not yet sent
    if ( !is_syn_sent_ ) {
      message.SYN = true;
      is_syn_sent_ = true;
    }

    message.seqno = next_seqno_;

    // Calculate payload size considering SYN flag
    uint64_t payload_size = min( TCPConfig::MAX_PAYLOAD_SIZE, capacity - message.SYN );
    string payload;
    read( reader(), payload_size, payload );

    // Add FIN flag if stream is finished and we have space
    if ( !is_fin_sent_ && reader().is_finished() && payload.size() < capacity ) {
      message.FIN = true;
      is_fin_sent_ = true;
    }

    message.payload = move( payload );
    message.RST = input_.has_error();

    // Skip empty segments
    if ( message.sequence_length() == 0 ) {
      break;
    }

    // Reset timer for first segment in flight
    if ( outstanding_segments_.empty() ) {
      last_tick_ms_ = 0;
      RTO_ms_ = initial_RTO_ms_;
    }

    // Transmit the segment
    transmit( message );

    // Store segment for retransmission tracking
    outstanding_segments_[next_seqno_.unwrap( isn_, writer().bytes_pushed() )]
      = { .message = message, .retransmissions_count = 0 };

    // Update sequence number and remaining capacity
    next_seqno_ += message.sequence_length();
    capacity -= message.sequence_length();

    // Stop after FIN segment
    if ( message.FIN ) {
      break;
    }
  }
}

TCPSenderMessage TCPSender::make_empty_message() const
{
  return TCPSenderMessage { .seqno = next_seqno_, .RST = input_.has_error() };
}

void TCPSender::receive( const TCPReceiverMessage& msg )
{
  window_size_ = msg.window_size;

  if ( msg.RST ) {
    input_.set_error();
    return;
  }

  if ( !msg.ackno.has_value() ) {
    return;
  }

  last_ackno_ = *msg.ackno;

  uint64_t abs_ackno = msg.ackno->unwrap( isn_, writer().bytes_pushed() );
  uint64_t next_seqno = next_seqno_.unwrap( isn_, writer().bytes_pushed() );

  // Ignore invalid acknowledgments
  if ( abs_ackno > next_seqno ) {
    return;
  }

  // Remove acknowledged segments
  auto it = outstanding_segments_.begin();
  while ( it != outstanding_segments_.end() ) {
    uint64_t seg_start = it->first;
    uint64_t seg_end = seg_start + it->second.message.sequence_length();

    if ( seg_end <= abs_ackno ) {
      it = outstanding_segments_.erase( it );
      last_tick_ms_ = 0;
      RTO_ms_ = initial_RTO_ms_;
    } else {
      break;
    }
  }
}

void TCPSender::tick( uint64_t ms_since_last_tick, const TransmitFunction& transmit )
{
  last_tick_ms_ += ms_since_last_tick;

  if ( outstanding_segments_.empty() ) {
    return;
  }

  auto it = outstanding_segments_.begin();
  auto& oldest = it->second;

  if ( last_tick_ms_ >= RTO_ms_ ) {
    transmit( oldest.message );
    last_tick_ms_ = 0;
    oldest.retransmissions_count++;

    if ( window_size_ > 0 ) {
      RTO_ms_ *= 2;
    }
  }
}
