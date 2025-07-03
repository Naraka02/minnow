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
    if ( !message.SYN ) {
      return;
    }
    isn_ = message.seqno;
    is_syn_received_ = true;
  }

  uint64_t first_index = message.seqno.unwrap( isn_, reassembler_.writer().bytes_pushed() ) - 1 + message.SYN;

  reassembler_.insert( first_index, message.payload, message.FIN );
}

TCPReceiverMessage TCPReceiver::send() const
{
  const Writer& writer = reassembler_.writer();
  uint16_t window_size
    = writer.available_capacity() > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>( writer.available_capacity() );
  return TCPReceiverMessage {
    .ackno = is_syn_received_ ? std::make_optional<Wrap32>( isn_ + writer.bytes_pushed() + 1 + writer.is_closed() )
                              : std::nullopt,
    .window_size = window_size,
    .RST = writer.has_error() };
}
