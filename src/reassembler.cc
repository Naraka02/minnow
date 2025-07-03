#include "reassembler.hh"

using namespace std;

void Reassembler::insert( uint64_t first_index, string data, bool is_last_substring )
{
  if ( is_last_substring && !last_received_ ) {
    last_received_ = true;
    stream_size_ = first_index + data.size();
  }

  uint64_t next_index = writer().bytes_pushed();
  uint64_t available_capacity = writer().available_capacity();
  uint64_t first_unacceptable = next_index + available_capacity;

  if ( data.empty() || first_index >= first_unacceptable || first_index + data.size() <= next_index ) {
    if ( last_received_ && writer().bytes_pushed() == stream_size_ ) {
      output_.writer().close();
    }
    return;
  }

  if ( first_index < next_index ) {
    data = data.substr( next_index - first_index );
    first_index = next_index;
  }
  if ( first_index + data.size() > first_unacceptable ) {
    data = data.substr( 0, first_unacceptable - first_index );
  }

  uint64_t new_start = first_index;
  uint64_t new_end = first_index + data.size() - 1;
  string new_data = data;

  // Merge with overlap segments
  auto it = pending_data_.lower_bound( new_start );
  if ( it != pending_data_.begin() ) {
    auto prev = std::prev( it );
    uint64_t prev_start = prev->first;
    uint64_t prev_end = prev_start + prev->second.size() - 1;
    if ( prev_end >= new_start ) {
      if ( new_end > prev_end ) {
        uint64_t offset_in_new = prev_end - new_start + 1;
        new_data = prev->second + new_data.substr( offset_in_new );
      } else {
        new_data = prev->second;
      }
      new_start = prev_start;
      new_end = new_start + new_data.size() - 1;
      pending_data_.erase( prev );
    }
  }
  it = pending_data_.lower_bound( new_start );
  while ( it != pending_data_.end() ) {
    uint64_t seg_start = it->first;
    if ( seg_start > new_end + 1 ) {
      break;
    }
    string const& seg_data = it->second;
    uint64_t seg_end = seg_start + seg_data.size() - 1;
    if ( seg_end > new_end ) {
      uint64_t offset_in_seg = new_end - seg_start + 1;
      if ( offset_in_seg < seg_data.size() ) {
        new_data += seg_data.substr( offset_in_seg );
        new_end = new_start + new_data.size() - 1;
      }
    }
    it = pending_data_.erase( it );
  }

  pending_data_[new_start] = new_data;

  // Write consecutive data to the output stream
  while ( !pending_data_.empty() ) {
    auto it_first = pending_data_.begin();
    if ( it_first->first != writer().bytes_pushed() ) {
      break;
    }
    output_.writer().push( it_first->second );
    pending_data_.erase( it_first );
  }

  if ( last_received_ && writer().bytes_pushed() == stream_size_ ) {
    output_.writer().close();
  }
}

uint64_t Reassembler::count_bytes_pending() const
{
  uint64_t count = 0;
  for ( const auto& [index, data] : pending_data_ ) {
    count += data.size();
  }
  return count;
}
