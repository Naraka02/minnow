#include "byte_stream.hh"

using namespace std;

ByteStream::ByteStream( uint64_t capacity ) : capacity_( capacity ) {}

void Writer::push( string data )
{
  if ( closed_ || data.empty() ) {
    return;
  }

  uint64_t data_size = data.size();
  uint64_t space_available = available_capacity();
  uint64_t bytes_to_push = min( data_size, space_available );

  for ( uint64_t i = 0; i < bytes_to_push; ++i ) {
    buffer_.push_back( data[i] );
  }

  bytes_pushed_ += bytes_to_push;
}

void Writer::close()
{
  closed_ = true;
}

bool Writer::is_closed() const
{
  return closed_;
}

uint64_t Writer::available_capacity() const
{
  return capacity_ - buffer_.size();
}

uint64_t Writer::bytes_pushed() const
{
  return bytes_pushed_;
}

string_view Reader::peek() const
{
  if ( buffer_.empty() ) {
    return {};
  }
  return string_view( buffer_.data(), buffer_.size() );
}

void Reader::pop( uint64_t len )
{
  if ( len <= 0 || len > buffer_.size() ) {
    return;
  }
  
  buffer_.erase( buffer_.begin(), buffer_.begin() + len );
  bytes_popped_ += len;
}

bool Reader::is_finished() const
{
  return closed_ && bytes_buffered() == 0;
}

uint64_t Reader::bytes_buffered() const
{
  return bytes_pushed_ - bytes_popped_;
}

uint64_t Reader::bytes_popped() const
{
  return bytes_popped_;
}
