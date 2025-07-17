#include "tcp_receiver.hh"
#include "debug.hh"

using namespace std;

void TCPReceiver::receive( TCPSenderMessage message )
{
  if ( message.RST ) {
    reassembler_.reader().set_error();
    return;
  }
  if ( !is_syn_received_ ) {
    if ( !message.SYN )
      return;
    isn_ = message.seqno;
    is_syn_received_ = true;
  }
  uint64_t first_index = message.seqno.unwrap( isn_, reassembler_.writer().bytes_pushed() ) - 1 + message.SYN;
  reassembler_.insert( first_index, message.payload, message.FIN );
}

TCPReceiverMessage TCPReceiver::send() const
{
  const Writer& writer = reassembler_.writer();
  uint16_t window_size = min<uint64_t>( writer.available_capacity(), UINT16_MAX );
  std::optional<Wrap32> ackno = std::nullopt;
  if ( is_syn_received_ ) {
    ackno = Wrap32( isn_ + writer.bytes_pushed() + 1 + writer.is_closed() );
  }
  return TCPReceiverMessage { .ackno = ackno, .window_size = window_size, .RST = writer.has_error() };
}
