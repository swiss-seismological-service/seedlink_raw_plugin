/*
 * A SeedLink plugin that collects data from a TCP socket and passes it on
 * to SeedLink.
 *
 * This plugin requires a barebone minimalistic server on the trasmitter
 * side and allows to easily stream data to Seelink without much effort.
 *
 * Copyright (c) 2021 Swiss Seismological Service (SED)
 *
 * Written by Luca Scarabello @ ETH Zuerich
 */

#include "plugin.h"
#include "plugin_exceptions.h"
#include "utils.h"

#include <libmseed.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <regex>
#include <string>
#include <unordered_map>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <getopt.h>
#include <signal.h>
#include <syslog.h>
#include <unistd.h>

using namespace std;

namespace {

const char *plugin_name = "raw_seedlink_plugin";

class RecoverableError : public SeedlinkPlugin::PluginError
{
public:
  RecoverableError(const string &message) : PluginError(message) {}
};

class UnrecoverableError : public SeedlinkPlugin::PluginError
{
public:
  UnrecoverableError(const string &message) : PluginError(message) {}
};

std::vector<std::string> splitString(const std::string &str,
                                     const std::regex &regex)
{
  return {std::sregex_token_iterator{str.begin(), str.end(), regex, -1},
          std::sregex_token_iterator()};
}

class Log
{
private:
  static const int LOGMSGLEN = 256;

private:
  static void log(bool isError, char *msg)
  {
    if (useSyslog)
    {
      ::syslog((isError ? LOG_ERR : LOG_INFO), "%s", msg);
    }
    else
    {
      time_t t = ::time(nullptr);
      char *p  = ::asctime(::localtime(&t));
      int len = strlen(p) - 1;
      fprintf((isError ? stderr : stdout), "%.*s - %s: %s\n", len, p,
              ::plugin_name, msg);
    }
  }

public:
  static bool useSyslog;

public:
  static void info(const char *fmt, ...)
  {
    char buf[LOGMSGLEN];
    va_list ap;

    ::va_start(ap, fmt);
    ::vsnprintf(buf, LOGMSGLEN, fmt, ap);
    ::va_end(ap);

    log(false, buf);
  }

  static void error(const char *fmt, ...)
  {
    char buf[LOGMSGLEN];
    va_list ap;

    ::va_start(ap, fmt);
    ::vsnprintf(buf, LOGMSGLEN, fmt, ap);
    ::va_end(ap);

    log(true, buf);
  }
};
bool Log::useSyslog = false;

class TcpClient
{
private:
  int _sock;
  struct sockaddr_in _server;

public:
  static const string LINEEND;
  const std::string address;
  const int port;

public:
  TcpClient(const std::string &address_, int port_)
      : _sock(-1), address(address_), port(port_)
  {}

  ~TcpClient() { close(); }

  bool connected() { return _sock >= 0; }

  void close()
  {
    // close socket if it open
    if (!connected()) return;

    if (::close(_sock) == -1)
    {
      Log::error("Error while closing server socket: %s", ::strerror(errno));
    }
    _sock = -1;
  }

  TcpClient(const TcpClient &other)            = default;
  TcpClient &operator=(const TcpClient &other) = default;

  TcpClient(TcpClient &&other)            = default;
  TcpClient &operator=(TcpClient &&other) = default;

  void connect()
  {
    // close socket if it is open
    close();

    Log::info("Connecting to %s:%d", address.c_str(), port);

    // Create socket
    int tmpSock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (tmpSock == -1)
    {
      throw UnrecoverableError(string("Could not create server socket: ") +
                               ::strerror(errno));
    }

    // Set timeout on the socket
    struct timeval timeout;
    timeout.tv_sec  = 90;
    timeout.tv_usec = 0;

    if (::setsockopt(tmpSock, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof timeout) < 0)
      Log::error("setsockopt failed: %s", ::strerror(errno));

    if (::setsockopt(tmpSock, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                     sizeof timeout) < 0)
      Log::error("setsockopt failed: %s", ::strerror(errno));

    // setup address structure
    if (::inet_aton(address.c_str(), &_server.sin_addr) == 0)
    {
      // resolve the hostname, its not an ip address
      struct hostent *host;
      struct in_addr **addr_list;

      if ((host = ::gethostbyname(address.c_str())) == nullptr)
      {
        throw UnrecoverableError(string("Failed to resolve server name: ") +
                                 ::strerror(errno));
      }
      addr_list = (struct in_addr **)host->h_addr_list;
      for (int i = 0; addr_list[i] != nullptr; i++)
      {
        _server.sin_addr = *addr_list[i];
        Log::info("%s resolved to %s", address.c_str(),
                  inet_ntoa(_server.sin_addr));
        break;
      }
    }

    _server.sin_family = AF_INET;
    _server.sin_port   = htons(port);

    // Connect to remote server
    if (::connect(tmpSock, (struct sockaddr *)&_server, sizeof(_server)) == -1)
    {
      string err = string("Connect failed: ") + ::strerror(errno);
      ::close(tmpSock);
      throw RecoverableError(err);
    }

    Log::info("Connected");

    this->_sock = tmpSock;
  }

  void sendLine(const string &line)
  {
    string fullLine = line + LINEEND;
    send(fullLine.c_str(), fullLine.size());
  }

  void send(const void *data, size_t size)
  {
    if (!connected()) throw RecoverableError("Connection is not open");

    if (size == 0) return;

    const ssize_t numBytesLeft = fullSend(_sock, data, size);

    if (numBytesLeft != 0) // send failed
    {
      if (numBytesLeft < 0)
        throw RecoverableError(string("Sending data to socket failed") +
                               ::strerror(errno));
      throw RecoverableError("Sending data to socket failed");
    }
  }

  static ssize_t fullSend(int fd, const void *buf, size_t count)
  {
    size_t nleft;
    ssize_t nwritten;

    nleft = count;
    while (nleft > 0) // repeat until none left
    {
      if ((nwritten = ::send(fd, buf, nleft, 0)) < 0)
      {
        if (errno == EINTR) // if interrupted by system call
        {
          continue; // repeat the loop
        }
        else
        {
          return nwritten; // otherwise return the error
        }
      }
      nleft -= nwritten;
      buf = static_cast<const char *>(buf) + nwritten;
    }
    return nleft;
  }

  string receiveLine()
  {
    string line;

    auto endsWith = [](std::string const &s,
                       std::string const &ending) -> bool {
      if (ending.size() > s.size()) return false;
      return std::equal(ending.rbegin(), ending.rend(), s.rbegin());
    };

    do
    {
      unsigned char c;
      receive(&c, 1);
      line += c;
    } while (!endsWith(line, LINEEND));

    return line.substr(0, line.size() - LINEEND.length());
  }

  void receive(void *data, size_t size)
  {
    if (!connected()) throw RecoverableError("Connection is not open");

    if (size == 0) return;

    const ssize_t numBytesLeft = fullReceive(_sock, data, size);

    if (numBytesLeft != 0) // Receiving failed
    {
      if (numBytesLeft < 0)
        throw RecoverableError(string("Receiving data from socket failed: ") +
                               ::strerror(errno));
      throw RecoverableError("Server connection closed");
    }
  }

  static ssize_t fullReceive(int fd, void *buf, size_t count)
  {
    size_t nleft;
    ssize_t nread;

    nleft = count;
    while (nleft > 0) // repeat until none left
    {
      if ((nread = ::recv(fd, buf, nleft, 0)) < 0)
      {
        if (errno == EINTR) // if interrupted by system call
        {
          continue; // repeat the loop
        }
        else
        {
          return nread; // otherwise return the error
        }
      }
      else if (nread == 0) // connection closed
      {
        break;
      }
      nleft -= nread;
      buf = static_cast<char *>(buf) + nread;
    }
    return nleft;
  }
};
const string TcpClient::LINEEND = "\n";

class MiniSeedStreamer
{
private:
  class SampleBuffer
  {
  public:
    virtual void prepare(uint32_t numSamples) = 0;
    virtual void feed(int32_t sample)    = 0;
    virtual void feed(float sample)      = 0;
    virtual void feed(double sample)     = 0;
    virtual uint32_t numSamples() const  = 0;
    virtual void *data()                 = 0;
  };

  template <class T> class SampleBufferImpl : public SampleBuffer
  {
  private:
    vector<T> vec;

  public:
    void prepare(uint32_t numSamples) override
    {
      vec.clear();
      if (vec.capacity() < numSamples)
      {
        vec.reserve(numSamples);
      }
    }
    void feed(int32_t sample) override { vec.push_back(sample); }
    void feed(float sample) override { vec.push_back(sample); }
    void feed(double sample) override { vec.push_back(sample); }
    uint32_t numSamples() const override { return vec.size(); }
    void *data() override { return vec.data(); }
  };

private:
  MS3Record *_pmsr = nullptr;
  int _sequence_number;
  struct DataType
  {
    unsigned encoding;
    char sampletype;
  } _dataType;
  unique_ptr<SampleBuffer> _sampleBuf;
  std::string _extHdrBuf;

public:
  const string networkCode;
  const string stationCode;
  const string locationCode;
  const string channelCode;
  const unsigned samprate;

public:
  MiniSeedStreamer(const string &networkCode_,
                   const string &stationCode_,
                   const string &locationCode_,
                   const string &channelCode_,
                   unsigned samprate_,
                   const string &mseedEncoding_)
      : networkCode(networkCode_), stationCode(stationCode_),
        locationCode(locationCode_), channelCode(channelCode_),
        samprate(samprate_)
  {
    _pmsr = ::msr3_init(nullptr);
    if (!_pmsr)
    {
      throw UnrecoverableError("msr_init failed");
    }

    static const unordered_map<string, DataType> encodings = {
        {"INT16", {DE_INT16, 'i'}},
        {"INT32", {DE_INT32, 'i'}},
        {"STEIM1", {DE_STEIM1, 'i'}},
        {"STEIM2", {DE_STEIM2, 'i'}},
        {"FLOAT32", {DE_FLOAT32, 'f'}},
        {"FLOAT64", {DE_FLOAT64, 'd'}}
    };

    try
    {
      string mseedEncodingUpper(mseedEncoding_);
      std::transform(mseedEncoding_.begin(), mseedEncoding_.end(),
                     mseedEncodingUpper.begin(), ::toupper);
      _dataType = encodings.at(mseedEncodingUpper);
    }
    catch (std::out_of_range &e)
    {
      throw UnrecoverableError("Invaling encoding: " + mseedEncoding_);
    }

    _sequence_number = 1;

    switch (_dataType.encoding)
    {
    case DE_INT16:  case DE_INT32:
    case DE_STEIM1: case DE_STEIM2: 
      _sampleBuf.reset(new SampleBufferImpl<int32_t>()); 
       break;
    case DE_FLOAT32: 
       _sampleBuf.reset(new SampleBufferImpl<float>());
       break;
    case DE_FLOAT64: 
       _sampleBuf.reset(new SampleBufferImpl<double>());
       break;
    }
  }

  ~MiniSeedStreamer()
  {
    // Disconnect pointers we own, otherwise msr3_free() will free it
    _pmsr->datasamples = nullptr;
    _pmsr->extra = nullptr;
    ::msr3_free(&_pmsr);
  }

  MiniSeedStreamer(const MiniSeedStreamer &other)            = delete;
  MiniSeedStreamer &operator=(const MiniSeedStreamer &other) = delete;

  MiniSeedStreamer(MiniSeedStreamer &&other)            = delete;
  MiniSeedStreamer &operator=(MiniSeedStreamer &&other) = delete;

  void prepare(uint32_t numSamples) { _sampleBuf->prepare(numSamples); }

  void feed(int32_t sample) { _sampleBuf->feed(sample); }

  void feed(float sample) { _sampleBuf->feed(sample); }

  void feed(double sample) { _sampleBuf->feed(sample); }

  void send(const struct ptime *pt, uint8_t timeQuality)
  {
    send(pt, timeQuality, _sampleBuf->data(), _sampleBuf->numSamples());
  }

private:
  void send(const struct ptime *pt,
            uint8_t timeQuality,
            void *dataptr,
            int64_t numSamples)
  {
    // Disconnect pointers we own, otherwise msr3_init() will free it
    _pmsr->datasamples = nullptr;
    _pmsr->extra = nullptr;
    ::msr3_init(_pmsr);

    // Set header, always the same (Could we avoid re-initialization?)
    if ( ms_nslc2sid(_pmsr->sid, LM_SIDLEN, 0, networkCode.c_str(),
                     stationCode.c_str(), locationCode.c_str(),
                     channelCode.c_str()) < 0 )
    {
       throw UnrecoverableError("Could not create source identifier");
    }
    _pmsr->formatversion = 2; // force miniSEED v2 encoding
    _pmsr->reclen      = PLUGIN_MSEED_SIZE;
    _pmsr->samprate    = samprate;
    _pmsr->sampletype  = _dataType.sampletype;
    _pmsr->encoding    = _dataType.encoding;

    // Extra headers to force the creation of miniSEED v2 info not present
    // anymore in MS3Record: msr.dataquality, msr.sequence_number and blkt_1001
    // {"FDSN":{"DataQuality":"D","Sequence":???,"Time":{"Quality":???}}}
    _extHdrBuf = R"({"FDSN":{"DataQuality":"D","Sequence":)" +
                   std::to_string(_sequence_number) + R"(,"Time":{"Quality":)" +
                   std::to_string(timeQuality) + "}}}";
    _pmsr->extra = _extHdrBuf.data();
    _pmsr->extralength = _extHdrBuf.length();


    // Data
    _pmsr->datasamples     = dataptr;
    _pmsr->numsamples      = numSamples;
    // Miniseed V3 supports nanoseconds, but we are forcing V2 (microseconds)
    _pmsr->starttime = ms_time2nstime(pt->year, pt->yday, pt->hour, pt->minute,
                                      pt->second, pt->usec*1000);

    const auto recordHandler = [](char *record, int reclen, void *handlerdata) {
      MiniSeedStreamer *THIS =
          reinterpret_cast<MiniSeedStreamer *>(handlerdata);

      // Log::info("Sending packet to seedlink, reclen=%d", reclen);

      int code =
          send_mseed((THIS->networkCode + "." + THIS->stationCode).c_str(),
                     record, reclen);

      if (code < 0)
      {
        throw UnrecoverableError(string("Error passing data to SeedLink: ") +
                                 ::strerror(errno));
      }
      else if (code == 0)
      {
        throw UnrecoverableError("Error passing data to SeedLink");
      }

      THIS->_sequence_number = (THIS->_sequence_number + 1) % 1000000;
    };

    int64_t packedSamples;
    uint32_t flags = MSF_FLUSHDATA | MSF_PACKVER2;
    if (::msr3_pack(_pmsr, recordHandler, this, &packedSamples, flags, 0) < 0)
    {
      // int64_t remaining_samples = numSamples - packedSamples;
      Log::error("msr3_pack failed: dropping data");
    }
  }
};

class RawClient
{
private:
  struct Stream
  {
    string locationCode;
    string channelCode;
    unique_ptr<MiniSeedStreamer> msStreamer;
  };

  enum class Endianness
  {
    UNDEFINED,
    BIG,
    LITTLE
  };

  enum class SampleType
  {
    INT8,
    INT16,
    INT32,
    FLOAT32,
    FLOAT64
  };

  struct ChannelSettings
  {
    unsigned samplingRate;
    Endianness endianness;
    unsigned sampleSize;
    SampleType sampleType;
  };

  TcpClient _client;
  unordered_map<unsigned, Stream> _stream;
  unordered_map<unsigned, ChannelSettings> _chSettings;

  void handshake()
  {
    // agree on the protocol
    _client.sendLine("RAW 2.0");
    string clientVersion = _client.receiveLine();

    Log::info("Client protocol version %s", clientVersion.c_str());

    if (clientVersion != "RAW 2.0")
    {
      throw RecoverableError(
          "Protocol error: unsupported client protocol version " +
          clientVersion);
    }

    _chSettings.clear();

    for (auto &kv : _stream)
    {
      const unsigned channel   = kv.first;
      ChannelSettings settings = {0, Endianness::UNDEFINED, 0};

      //
      // Request channel
      //
      Log::info("Requesting channel %d", channel);
      _client.sendLine("CHANNEL");
      _client.sendLine(std::to_string(channel));

      //
      // Get sampling rate
      //
      if (_client.receiveLine() != "SAMPLING RATE")
        throw RecoverableError("Protocol error: expecting SAMPLING RATE");

      string samprate = _client.receiveLine();
      size_t idx;
      settings.samplingRate = stoul(samprate, &idx);
      if (samprate.empty() || idx != samprate.size())
        throw RecoverableError("Receoved invalid sampling rate '" + samprate +
                               "' from the server");
      Log::info("Server says channel %u has samprate %u", channel,
                settings.samplingRate);

      //
      // Get sample endianness
      //
      if (_client.receiveLine() != "SAMPLE ENDIANNESS")
        throw RecoverableError("Protocol error: expecting SAMPLE ENDIANNESS");

      string endianness = _client.receiveLine();
      if (endianness == "big")
        settings.endianness = Endianness::BIG;
      else if (endianness == "little")
        settings.endianness = Endianness::LITTLE;
      else
        throw RecoverableError("Received invalid endianess '" + endianness +
                               "' from the server");
      Log::info("Server says channel %u endianness is %s", channel,
                endianness.c_str());

      //
      // Get sample type
      //
      if (_client.receiveLine() != "SAMPLE TYPE")
        throw RecoverableError("Protocol error: expecting SAMPLE TYPE");

      string type = _client.receiveLine();
      if (type == "int8")
      {
        settings.sampleSize = 1;
        settings.sampleType = SampleType::INT8;
      }
      else if (type == "int16")
      {
        settings.sampleSize = 2;
        settings.sampleType = SampleType::INT16;
      }
      else if (type == "int32")
      {
        settings.sampleSize = 4;
        settings.sampleType = SampleType::INT32;
      }
      else if (type == "float32")
      {
        settings.sampleSize = 4;
        settings.sampleType = SampleType::FLOAT32;
      }
      else if (type == "float64")
      {
        settings.sampleSize = 8;
        settings.sampleType = SampleType::FLOAT64;
      }
      else
      {
        throw RecoverableError("Received invalid sample type '" + type +
                               "' from the server");
      }
      Log::info("Server says channel %u data type is %s", channel,
                type.c_str());

      _chSettings[channel] = settings;

      kv.second.msStreamer.reset(new MiniSeedStreamer(
          networkCode, stationCode, kv.second.locationCode,
          kv.second.channelCode, settings.samplingRate, mseedEncoding));
    }

    // start streaming data
    _client.sendLine("START");
    if (_client.receiveLine() != "STARTING")
      throw RecoverableError("Protocol error: expecting STARTING");
  }

  void feedSample(const unsigned char *ds,
                  const ChannelSettings &settings,
                  MiniSeedStreamer *msStreamer)
  {
    // ds: data sample, pointer to the sample in the data buffer

    switch (settings.sampleType)
    {

    case SampleType::INT8: {
      int32_t sample = static_cast<char>(ds[0]);
      msStreamer->feed(sample);
    }
    break;

    case SampleType::INT16: {
      int32_t sample = 0;
      if (settings.endianness == Endianness::BIG)
      {
        sample = static_cast<int16_t>(uint16_t(ds[0]) << 8 | uint16_t(ds[1]));
      }
      else if (settings.endianness == Endianness::LITTLE)
      {
        sample = static_cast<int16_t>(uint16_t(ds[1]) << 8 | uint16_t(ds[0]));
      }
      msStreamer->feed(sample);
    }
    break;

    case SampleType::INT32: {
      int32_t sample = 0;
      if (settings.endianness == Endianness::BIG)
      {
        sample =
            static_cast<int32_t>(uint32_t(ds[0]) << 24 |
                                 uint32_t(ds[1]) << 16 |
                                 uint32_t(ds[2]) << 8  |
                                 uint32_t(ds[3]));
      }
      else if (settings.endianness == Endianness::LITTLE)
      {
        sample =
            static_cast<int32_t>(uint32_t(ds[3]) << 24 |
                                 uint32_t(ds[2]) << 16 |
                                 uint32_t(ds[1]) << 8  |
                                 uint32_t(ds[0]));
      }
      msStreamer->feed(sample);
    }
    break;

    case SampleType::FLOAT32: {
      static const Endianness hostEndianness = floatEndianness();

      float sample                           = 0;
      unsigned char *sp                      = (unsigned char *)&sample;

      if (hostEndianness == Endianness::UNDEFINED)
      {
        throw UnrecoverableError("Could not determine host float endianness");
      }
      else if (settings.endianness == hostEndianness)
      {
        sp[0] = ds[0];
        sp[1] = ds[1];
        sp[2] = ds[2];
        sp[3] = ds[3];
      }
      else
      {
        sp[0] = ds[3];
        sp[1] = ds[2];
        sp[2] = ds[1];
        sp[3] = ds[0];
      }

      msStreamer->feed(sample);
    }
    break;

    case SampleType::FLOAT64: {
      static const Endianness hostEndianness = doubleEndianness();

      double sample     = 0;
      unsigned char *sp = (unsigned char *)&sample;

      if (hostEndianness == Endianness::UNDEFINED)
      {
        throw UnrecoverableError("Could not determine host double endianness");
      }
      else if (settings.endianness == hostEndianness)
      {
        sp[0] = ds[0];
        sp[1] = ds[1];
        sp[2] = ds[2];
        sp[3] = ds[3];
        sp[4] = ds[4];
        sp[5] = ds[5];
        sp[6] = ds[6];
        sp[7] = ds[7];
      }
      else
      {
        sp[0] = ds[7];
        sp[1] = ds[6];
        sp[2] = ds[5];
        sp[3] = ds[4];
        sp[4] = ds[3];
        sp[5] = ds[2];
        sp[6] = ds[1];
        sp[7] = ds[0];
      }
      msStreamer->feed(sample);
    }
    break;
    } // switch
  }

  static const unsigned MAX_SAMPLES = 1e6; // 1 seconds at 1MHz
                                           // no specific reason for this value
                                           // just to avoid unlimited data
                                           // transfer

  static Endianness floatEndianness()
  {
    static const float f = -1.7247773e-34; // 0x87654321, IEEE-754 binary32
    static const unsigned char f_big[4]    = {0x87, 0x65, 0x43, 0x21};
    static const unsigned char f_little[4] = {0x21, 0x43, 0x65, 0x87};

    if (sizeof(float) != 4)
    {
      return Endianness::UNDEFINED;
    }
    else if (memcmp(&f, f_big, 4) == 0)
    {
      return Endianness::BIG;
    }
    else if (memcmp(&f, f_little, 4) == 0)
    {
      return Endianness::LITTLE;
    }
    else
    {
      return Endianness::UNDEFINED;
    }
  }

  static Endianness doubleEndianness()
  {
    static const double d = -1.2312365908092656e+303; // 0xFEDCBA0987654321, IEEE-754 binary64
    static const unsigned char d_big[8]    = {0xFE, 0xDC, 0xBA, 0x09, 0x87, 0x65, 0x43, 0x21};
    static const unsigned char d_little[8] = {0x21, 0x43, 0x65, 0x87, 0x09, 0xBA, 0xDC, 0xFE};

    if (sizeof(double) != 8)
    {
      return Endianness::UNDEFINED;
    }
    else if (memcmp(&d, d_big, 8) == 0)
    {
      return Endianness::BIG;
    }
    else if (memcmp(&d, d_little, 8) == 0)
    {
      return Endianness::LITTLE;
    }
    else
    {
      return Endianness::UNDEFINED;
    }
  }

public:
  const std::string host;
  const int port;
  const int sleepTime;
  const string networkCode;
  const string stationCode;
  const string mseedEncoding;

  RawClient(const string &host_,
            int port_,
            int sleepTime_,
            const string &networkCode_,
            const string &stationCode_,
            const string &channelCodeMap,
            const string &mseedEncoding_)
      : _client(host_, port_), host(host_), port(port_), sleepTime(sleepTime_),
        networkCode(networkCode_), stationCode(stationCode_),
        mseedEncoding(mseedEncoding_)
  {
    static const regex re("^(\\w*)?.(\\w*):([0-9]+)(,)?", regex::optimize);
    smatch m;
    string input = channelCodeMap;
    while (regex_search(input, m, re))
    {
      string locationCode = m[1].str();
      string channelCode  = m[2].str();
      unsigned channel    = stoul(m[3].str());

      _stream[channel] = {locationCode, channelCode, nullptr};

      Log::info("Server channel %d -> %s.%s.%s.%s", channel,
                networkCode.c_str(), stationCode.c_str(), locationCode.c_str(),
                channelCode.c_str());

      input = input.substr(m.length());
    }
    if (_stream.empty() || !input.empty())
    {
      throw RecoverableError("Unsuitable stream map provided: '" +
                             channelCodeMap + "'");
    }
  }

  ~RawClient() = default;

  RawClient(const RawClient &other)            = delete;
  RawClient &operator=(const RawClient &other) = delete;

  RawClient(RawClient &&other)            = delete;
  RawClient &operator=(RawClient &&other) = delete;

  void run()
  {
    unsigned char hdrBuf[18];
    vector<unsigned char> dataBuf;

    for (;;)
    {
      try
      {
        if (!_client.connected())
        {
          _client.connect();
          handshake();
        }

        // Read header from the server
        _client.receive(hdrBuf, sizeof(hdrBuf));

        //
        // parse header
        //
        struct
        {
          struct ptime pt;
          uint8_t timeQuality;
          uint16_t channel;
          uint32_t numSamples;
        } header;

        auto bigToUint16 = [](unsigned char a[2]) -> uint16_t {
          return (uint16_t(a[0]) << 8) | a[1];
        };
        auto bigToUint32 = [](unsigned char a[4]) -> uint32_t {
          return (uint32_t(a[0]) << 24) | (uint32_t(a[1]) << 16) |
                 (uint32_t(a[2]) << 8) | a[3];
        };

        header.pt.year     = bigToUint16(&hdrBuf[0]);
        header.pt.yday     = bigToUint16(&hdrBuf[2]);
        header.pt.hour     = hdrBuf[4];
        header.pt.minute   = hdrBuf[5];
        header.pt.second   = hdrBuf[6];
        header.pt.usec     = bigToUint32(&hdrBuf[7]);
        header.timeQuality = hdrBuf[11];
        header.channel     = bigToUint16(&hdrBuf[12]);
        header.numSamples  = bigToUint32(&hdrBuf[14]);

        // Log::info("Header content: num samples %d channel %d (year %d"
        //           " day %d hour %d minute %d second %d usec %d)",
        //           header.numSamples, header.channel, header.pt.year,
        //           header.pt.yday, header.pt.hour, header.pt.minute,
        //           header.pt.second, header.pt.usec);

        if (header.numSamples > MAX_SAMPLES)
          throw RecoverableError("Too many samples in a single packet");

        if (_chSettings.find(header.channel) == _chSettings.end())
          throw RecoverableError("Received unrequested channel");

        const ChannelSettings settings = _chSettings[header.channel];
        MiniSeedStreamer *msStreamer =
            _stream.at(header.channel).msStreamer.get();

        //
        // Read samples from the server
        //
        unsigned dataSize = header.numSamples * settings.sampleSize;
        if (dataBuf.size() < dataSize)
        {
          dataBuf.resize(dataSize);
        }

        unsigned char *data = dataBuf.data();
        _client.receive(data, dataSize);

        //
        // Convert samples and pass them to Seedlink
        //
        msStreamer->prepare(header.numSamples);

        for (unsigned i = 0; i < header.numSamples; i++)
        {
          unsigned char *dataSample = &data[i * settings.sampleSize];
          feedSample(dataSample, settings, msStreamer);
        }

        // Log::info("Sending %d samples to seedlink", header.numSamples);
        msStreamer->send(&header.pt, header.timeQuality);
      }
      catch (RecoverableError &e)
      {
        Log::error("Exception: %s", e.what());
        _client.close();
        for (auto &kv : _stream) kv.second.msStreamer = nullptr;
        ::sleep(sleepTime);
        continue;
      }
      catch (UnrecoverableError &e)
      {
        Log::error("Unrecoverable exception: %s", e.what());
        break;
      }
      catch (exception &e)
      {
        Log::error("Unexpected exception: %s", e.what());
        break;
      }
    }
  }
};

void quit(int sig = 0)
{
  if (sig != 0) Log::info("Received signal %d", sig);
  Log::info("Exiting");
  if (Log::useSyslog)
    closelog(); // this is actually optional accordingly to man page
  exit(0);
}

void printUsageAndExit(int argc, char **argv)
{

  Log::error("usage: raw [options] -s host -p port -c stream -m chmap");
  Log::error("");
  Log::error("Required:");
  Log::error("    -s host    server address, required");
  Log::error("    -p port    server port");
  Log::error("    -c stream  the stream the data refers to (NET.STA)");
  Log::error("    -m chmap   map of location and channel codes to server "
             "channel numbers");
  Log::error("Options:");
  Log::error("    -r secs    sleep time in seonds before reconnecting");
  Log::error("                after an EOF or connect failure,");
  Log::error("                default is 30 seconds");

  exit(1);
}

} // namespace

int main(int argc, char **argv)
{
  string host;
  int port = -1;
  string stream;
  string channelCodeMap;
  string mseedEncoding = "STEIM2";
  int sleepTime        = 60;
  bool daemonMode      = false;

  if (argc < 2)
  {
    printUsageAndExit(argc, argv);
  }

  int opt;
  while ((opt = getopt(argc, argv, "Ds:p:c:r:m:e:")) != -1)
  {
    switch (opt)
    {
    case 'D': daemonMode = true; break;
    case 's': host = optarg; break;
    case 'p': port = atoi(optarg); break;
    case 'c': stream = optarg; break;
    case 'r': sleepTime = atoi(optarg); break;
    case 'm': channelCodeMap = optarg; break;
    case 'e': mseedEncoding = optarg; break;
    default: printUsageAndExit(argc, argv); break;
    }
  }

  if (host.empty() || stream.empty() || channelCodeMap.empty() || port <= 0)
  {
    printUsageAndExit(argc, argv);
  }

  static const std::regex dot("\\.", std::regex::optimize);
  std::vector<std::string> tokens(splitString(stream, dot));
  if (tokens.size() != 2)
  {
    Log::error("-c option should be in the format STA.NET");
    quit();
  }
  string networkCode = tokens[0];
  string stationCode = tokens[1];

  struct sigaction sa;
  sa.sa_handler = quit;
  sa.sa_flags   = SA_RESTART;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGHUP, &sa, nullptr);
  sigaction(SIGPIPE, &sa, nullptr);

  if (daemonMode)
  {
    sa.sa_handler = SIG_IGN;
    sigaction(SIGHUP, &sa, nullptr);
    sigaction(SIGPIPE, &sa, nullptr);
    Log::info("Starting in daemon mode...");
    Log::info("take a look into syslog files for more messages");
    openlog(::plugin_name, 0, SYSLOG_FACILITY);
    Log::useSyslog = true;
  }

  RawClient client(host, port, sleepTime, networkCode, stationCode,
                   channelCodeMap, mseedEncoding);
  client.run();
  quit();
}
