#include "client_session.hpp"

#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/endian/conversion.hpp>

#include <glog/logging.h>

#include <cryptopp/salsa.h>
#include <cryptopp/osrng.h>

#include <utility/socks_constants.hpp>

namespace msocks
{

bool client_session::do_socks5_auth(yield_context &yield)
{
  struct __attribute__((packed))
  {
    uint8_t version;
    uint8_t n_method;
  } auth_method{};
  
  async_read(
    local,
    buffer(&auth_method, sizeof(auth_method)),
    transfer_exactly(sizeof(auth_method)),
    yield
  );
  
  std::vector<uint8_t> methods_buf;
  async_read(
    local,
    dynamic_buffer(methods_buf),
    transfer_exactly(auth_method.n_method),
    yield
  );
  
  bool find_auth = false;
  for ( auto method : methods_buf )
  {
    if ( method == constant::AUTH_NO_AUTH )
    {
      find_auth = true;
      break;
    }
  }
  
  if ( !find_auth )
  {
    LOG(WARNING) << "anonymous auth method not found";
    return false;
  }
  
  struct __attribute__((packed))
  {
    uint8_t version;
    uint8_t method;
  } auth_reply{constant::SOCKS5_VERSION, constant::AUTH_NO_AUTH};
  
  async_write(
    local,
    buffer(&auth_reply, sizeof(auth_reply)),
    transfer_exactly(sizeof(auth_reply)),
    yield
  );
  return true;
}

std::pair<bool, std::string>
client_session::do_get_proxy_addr(yield_context &yield)
{
  struct __attribute__((packed))
  {
    uint8_t version;
    uint8_t cmd;
    uint8_t reserve;
    uint8_t addr_type;
  } request{};
  
  async_read(
    local,
    buffer(&request, sizeof(request)),
    transfer_exactly(sizeof(request)),
    yield
  );
  
  if ( request.version != constant::SOCKS5_VERSION )
  {
    LOG(WARNING) << "unsupported version: " << request.version;
    return {false, ""};
  }
  
  switch ( request.cmd )
  {
    case constant::CONN_TCP:
      break;
    case constant::CONN_BND:
    case constant::CONN_UDP:
    default:
      LOG(WARNING) << "unsupported cmd: " << request.cmd;
      return {false, ""};
  }
  
  std::string a_p;
  std::vector<uint8_t> ap_buf;
  if ( request.addr_type == constant::ADDR_IPV4 )
  {
    async_read(
      local,
      dynamic_buffer(ap_buf),
      transfer_exactly(32 / 8 + 2),
      yield
    );
    
    struct __attribute__((packed))
    {
      uint32_t addr;
      uint16_t port;
    } addr_pt{};
    
    buffer_copy(
      buffer(&addr_pt, sizeof(addr_pt)),
      buffer(ap_buf),
      sizeof(addr_pt)
    );
    a_p = ip::make_address_v4(big_to_native(addr_pt.addr)).to_string();
    a_p += ":";
    a_p += std::to_string(big_to_native(addr_pt.port));
  }
  else if ( request.addr_type == constant::ADDR_IPV6 )
  {
    async_read(
      local,
      dynamic_buffer(ap_buf),
      transfer_exactly(128 / 8 + 2),
      yield
    );
    
    struct __attribute__((packed))
    {
      ip::address_v6::bytes_type addr;
      uint16_t port;
    } addr_pt{};
    
    buffer_copy(
      buffer(&addr_pt, sizeof(addr_pt)),
      buffer(ap_buf),
      sizeof(addr_pt)
    );
    std::reverse(addr_pt.addr.begin(), addr_pt.addr.end());
    a_p = ip::make_address_v6(addr_pt.addr).to_string();
    a_p += ":";
    a_p += std::to_string(big_to_native(addr_pt.port));
  }
  else if ( request.addr_type == constant::ADDR_DOMAIN )
  {
    uint8_t domain_length;
    async_read(
      local,
      buffer(&domain_length, sizeof(domain_length)),
      transfer_exactly(sizeof(domain_length)),
      yield
    );
    
    async_read(
      local,
      dynamic_buffer(ap_buf),
      transfer_exactly(domain_length + 2),
      yield
    );
    std::copy(ap_buf.begin(), ap_buf.begin() + domain_length,
              std::back_inserter(a_p));
    a_p += ":";
    uint16_t port = 0;
    std::copy(ap_buf.begin() + domain_length, ap_buf.end(), (uint8_t *) &port);
    big_to_native_inplace(port);
    a_p += std::to_string(port);
  }
  else
  {
    LOG(WARNING) << "unsupported address type: " << request.addr_type;
    return {false, ""};
  }
  
  struct __attribute__((packed))
  {
    uint8_t version;
    uint8_t rep;
    uint8_t rsv;
    uint8_t atyp;
    uint32_t bnd_addr;
    uint16_t bnd_port;
  } reply{
    constant::SOCKS5_VERSION,
    0x00,
    0x00,
    constant::ADDR_IPV4,
    0x00000000,
    0x0000
  };
  
  async_write(
    local,
    buffer(&reply, sizeof(reply)),
    transfer_all(),
    yield
  );
  return {true, std::move(a_p)};
}

std::pair<bool, std::string>
client_session::do_local_socks5(
  yield_context &yield)
{
  if ( do_socks5_auth(yield))
  {
    return do_get_proxy_addr(yield);
  }
  else
  {
    return {false, ""};
  }
}

void client_session::start(yield_context &yield)
{
  try
  {
    auto[success, addr_port] = do_local_socks5(yield);
    if ( !success )
      return;
    remote.async_connect(addr, yield);
    LOG(INFO) << "socket connect to " << addr_port;
    send_handshake(yield, std::move(addr_port));
  }
  catch ( system_error &e )
  {
    LOG(WARNING) << e.what();
    return;
  }
  auto p = shared_from_this();
  spawn(
    yield,
    [this, p](yield_context yield)
    {
      do_forward_local_to_remote(yield);
    });
  spawn(
    yield,
    [this, p](yield_context yield)
    {
      do_forward_remote_to_local(yield);
    });
}

client_session::client_session(
  ip::tcp::socket local_,
  const ip::tcp::endpoint &addr_,
  const CryptoPP::SecByteBlock& key_) :
  local(std::move(local_)),
  remote(local.get_executor().context()),
  addr(std::move(addr_)),
  key(key_),
  iv(8)
{
}

void client_session::do_forward_local_to_remote(yield_context &yield)
{
  try
  {
    std::vector<uint8_t> buf;
    while ( true )
    {
      auto n_read = async_read(local, dynamic_buffer(buf), yield);
      encrypt.ProcessString(buf.data(), n_read);
      async_write(remote, buffer(buf), transfer_all(), yield);
      buf.clear();
    }
    
  }
  catch (system_error & e)
  {
    local.close();
    remote.close();
    LOG(WARNING) << e.what();
  }
}

void client_session::send_handshake(yield_context &yield, std::string addr_port)
{
  AutoSeededRandomPool rng;
  rng.GenerateBlock(iv.data(), iv.size());
  
  encrypt.SetKeyWithIV(key.data(), key.size(), iv.data(), iv.size());
  decrypt.SetKeyWithIV(key.data(), key.size(), iv.data(), iv.size());
  
  encrypt.ProcessString((byte *) addr_port.data(), addr_port.size());
  uint16_t size = iv.size() + addr_port.size();
  
  std::vector<const_buffer> sequence;
  sequence.emplace_back(buffer(&size, sizeof(size)));
  sequence.emplace_back(buffer(iv.data(), iv.size()));
  sequence.emplace_back(buffer(addr_port));
  async_write(remote, sequence, transfer_all(), yield);
}

void client_session::do_forward_remote_to_local(yield_context &yield)
{
  try
  {
    std::vector<uint8_t> buf;
    while ( true )
    {
      auto n_read = async_read(remote, dynamic_buffer(buf), yield);
      decrypt.ProcessString(buf.data(), n_read);
      async_write(local, buffer(buf), transfer_all(), yield);
      buf.clear();
    }
  }
  catch (system_error & e)
  {
    local.close();
    remote.close();
    LOG(WARNING) << e.what();
  }
}

}
