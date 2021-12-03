/*
 * A SeedLink plugin that collects data from a TCP socket and passes it on
 * to SeedLink.
 *
 * This plugin requires a barebone minimalistic server on the trasmitter
 * side and allow to easily add data to Seelink without much effort.
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
      fprintf((isError ? stderr : stdout), "%.*s - %s: %s\n", strlen(p) - 1, p,
              ::plugin_name, msg);
    }
  }

public:
  static bool useSyslog;

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
  MSRecord *_pmsr = nullptr;
  int _sequence_number;

public:
  const string networkCode;
  const string stationCode;
  const string locationCode;
  const string channelCode;
  const unsigned samprate;

  MiniSeedStreamer(const string &networkCode_,
                   const string &stationCode_,
                   const string &locationCode_,
                   const string &channelCode_,
                   unsigned _samprate)
      : networkCode(networkCode_), stationCode(stationCode_),
        locationCode(locationCode_), channelCode(channelCode_),
        samprate(_samprate)
  {
    _pmsr = ::msr_init(nullptr);
    if (!_pmsr)
    {
      throw UnrecoverableError("msr_init failed");
    }
    _sequence_number = 1;
  }

  ~MiniSeedStreamer()
  {
    // Disconnect datasamples pointer, otherwise mst_free() will free it
    _pmsr->datasamples = nullptr;
    ::msr_free(&_pmsr);
  }

  void sendtoSeedlink(const struct ptime *pt, int32_t *dataptr, int numSamples)
  {
    // Disconnect datasamples pointer, otherwise mst_init() will free it
    _pmsr->datasamples = nullptr;
    ::msr_init(_pmsr);

    // Set header, always the same (I wish I could avoid re-initialization)
    _pmsr->reclen = PLUGIN_MSEED_SIZE;
    strcpy(_pmsr->network, networkCode.c_str());
    strcpy(_pmsr->station, stationCode.c_str());
    strcpy(_pmsr->location, locationCode.c_str());
    strcpy(_pmsr->channel, channelCode.c_str());
    _pmsr->dataquality = 'D';
    _pmsr->samprate    = samprate;
    _pmsr->sampletype  = 'i';
    _pmsr->encoding    = DE_STEIM2;
    _pmsr->byteorder   = 1;

    _pmsr->sequence_number = _sequence_number;
    _pmsr->datasamples     = dataptr;
    _pmsr->numsamples      = numSamples;
    _pmsr->starttime = ms_time2hptime(pt->year, pt->yday, pt->hour, pt->minute,
                                      pt->second, pt->usec);

    // The SEED format handles down to 100μsecs, but adding blocket 1001 allow
    // the resolution to go down to the microsecond. msr_pack will do that if it
    // founds a blkt_1001_s
    struct blkt_1001_s blkt1001 = {0};
    blkt1001.timing_qual        = 100; // best quality, not really useful
    if (!::msr_addblockette(_pmsr, reinterpret_cast<char *>(&blkt1001),
                            sizeof(struct blkt_1001_s), 1001, 0))
    {
      Log::error("Error adding 1001 blockette: msr_addblockette failed");
      return;
    }

    const auto recordHandler = [](char *record, int reclen, void *handlerdata) {
      MiniSeedStreamer *THIS =
          reinterpret_cast<MiniSeedStreamer *>(handlerdata);

      // Log::info("Sending packet to seedlink, reclen=%d", reclen);

      int code =
          send_mseed((THIS->networkCode + "." + THIS->stationCode).c_str(),
                     record, reclen);

      if (code < 0)
        throw UnrecoverableError(string("Error passing data to SeedLink: ") +
                                 ::strerror(errno));
      else if (code == 0)
        throw UnrecoverableError("Error passing data to SeedLink");

      THIS->_sequence_number = (THIS->_sequence_number + 1) % 1000000;
    };

    int64_t packedSamples;
    if (::msr_pack(_pmsr, recordHandler, this, &packedSamples, 1, 0) < 0)
    {
      Log::error("msr_pack failed: dropping data");
    }

    // int64_t remaining_samples = numSamples - packedSamples;
  }
};

class RawClient
{
private:
  struct Component
  {
    string name;
    string fullChannelCode;
    unique_ptr<MiniSeedStreamer> msStreamer;
  };

  enum class Endianness
  {
    UNDEFINED,
    BIG,
    LITTLE
  };
  struct ChannelSettings
  {
    unsigned samplingRate;
    Endianness endianness;
    unsigned sampleSize;
  };

  static const unsigned MAX_SAMPLES = 1e6;

  TcpClient _client;
  unordered_map<unsigned, Component> _components;
  unordered_map<unsigned, ChannelSettings> _chSettings;

  unsigned char _hdrBuf[19];
  vector<unsigned char> _dataBuf;
  vector<int32_t> _sampleBuf;

  void handshake()
  {
    // agree on the protocol
    _client.sendLine("RAW 1.0");
    if (_client.receiveLine() != "RAW 1.0")
      throw RecoverableError("Protocol error");

    _chSettings.clear();

    for (auto &kv : _components)
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
        settings.sampleSize = 1;
      else if (type == "int16")
        settings.sampleSize = 2;
      else if (type == "int32")
        settings.sampleSize = 4;
      else
        throw RecoverableError("Received invalid sample type '" + type +
                               "' from the server");
      Log::info("Server says channel %u data type is %s", channel,
                type.c_str());

      _chSettings[channel] = settings;

      kv.second.msStreamer.reset(new MiniSeedStreamer(
          networkCode, stationCode, locationCode, kv.second.fullChannelCode,
          settings.samplingRate));
    }

    // start streaming data
    _client.sendLine("START");
    if (_client.receiveLine() != "STARTING")
      throw RecoverableError("Protocol error: expecting STARTING");
  }

public:
  const std::string host;
  const int port;
  const int sleepTime;
  const string networkCode;
  const string stationCode;
  const string locationCode;
  const string channelCode;

  RawClient(const string &host_,
            int port_,
            int sleepTime_,
            const string &networkCode_,
            const string &stationCode_,
            const string &locationCode_,
            const string &channelCode_,
            const string &componentMap)
      : _client(host_, port_), host(host_), port(port_), sleepTime(sleepTime_),
        networkCode(networkCode_), stationCode(stationCode_),
        locationCode(locationCode_), channelCode(channelCode_)
  {
    static const regex re("^([0-9]+):(\\w)(,)?", regex::optimize);
    smatch m;
    string input = componentMap;
    while (regex_search(input, m, re))
    {
      unsigned channel = stoul(m[1].str());
      string name      = m[2].str();

      _components[channel] = {name, channelCode + name, nullptr};

      input = input.substr(m.length());
    }
    if (_components.empty() || !input.empty())
    {
      throw RecoverableError("Unsuitable components map provided: '" +
                             componentMap + "'");
    }
  }

  ~RawClient() {}

  void run()
  {
    auto bigToUint16 = [](unsigned char a[2]) -> uint16_t {
      return (uint16_t(a[0]) << 8) | a[1];
    };
    auto bigToUint32 = [](unsigned char a[4]) -> uint32_t {
      return (uint32_t(a[0]) << 24) | (uint32_t(a[1]) << 16) |
             (uint32_t(a[2]) << 8) | a[3];
    };
    auto littleToUint16 = [](unsigned char a[2]) -> uint16_t {
      return (uint16_t(a[1]) << 8) | a[0];
    };
    auto littleToUint32 = [](unsigned char a[4]) -> uint32_t {
      return (uint32_t(a[3]) << 24) | (uint32_t(a[2]) << 16) |
             (uint32_t(a[1]) << 8) | a[0];
    };

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
        _client.receive(_hdrBuf, sizeof(_hdrBuf));

        //
        // parse header
        //
        struct
        {
          struct ptime pt;
          uint32_t channel;
          uint32_t numSamples;
        } header;

        header.pt.year    = bigToUint16(&_hdrBuf[0]);
        header.pt.yday    = bigToUint16(&_hdrBuf[2]);
        header.pt.hour    = _hdrBuf[4];
        header.pt.minute  = _hdrBuf[5];
        header.pt.second  = _hdrBuf[6];
        header.pt.usec    = bigToUint32(&_hdrBuf[7]);
        header.channel    = bigToUint32(&_hdrBuf[11]);
        header.numSamples = bigToUint32(&_hdrBuf[15]);

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

        //
        // Read samples from the server
        //
        unsigned dataSize = header.numSamples * settings.sampleSize;
        if (_dataBuf.size() < dataSize) _dataBuf.resize(dataSize);

        unsigned char *data = _dataBuf.data();
        _client.receive(data, dataSize);

        //
        // Convert samples and pass them to Seedlink
        // 
        if (_sampleBuf.size() < header.numSamples)
          _sampleBuf.resize(header.numSamples);

        int32_t *samples = _sampleBuf.data();
        for (unsigned i = 0; i < header.numSamples; i++)
        {
          int32_t sample;
          if (settings.sampleSize == 1)
            sample = static_cast<char>(data[i]);
          else if (settings.sampleSize == 2)
          {
            if (settings.endianness == Endianness::BIG)
              sample = static_cast<int16_t>(
                  bigToUint16(&data[i * settings.sampleSize]));
            else if (settings.endianness == Endianness::LITTLE)
              sample = static_cast<int16_t>(
                  littleToUint16(&data[i * settings.sampleSize]));
          }
          else if (settings.sampleSize == 4)
          {
            if (settings.endianness == Endianness::BIG)
              sample = static_cast<int32_t>(
                  bigToUint32(&data[i * settings.sampleSize]));
            else if (settings.endianness == Endianness::LITTLE)
              sample = static_cast<int32_t>(
                  littleToUint32(&data[i * settings.sampleSize]));
          }
          samples[i] = sample;
        }

        // Log::info("Sending %d samples to seedlink", header.numSamples);
        _components.at(header.channel)
            .msStreamer->sendtoSeedlink(&header.pt, samples, header.numSamples);
      }
      catch (RecoverableError &e)
      {
        Log::error("Exception: %s", e.what());
        _client.close();
        for (auto &kv : _components) kv.second.msStreamer = nullptr;
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

  Log::error("usage: raw [options] -s host -p port -c stream");
  Log::error("");
  Log::error("Required:");
  Log::error("    -s host    server address, required");
  Log::error("    -p port    server port");
  Log::error("    -c stream  the stream the data refers to (NET.STA.LOC.CHAN)");
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
  string componentMap;
  int sleepTime   = 60;
  bool daemonMode = false;

  if (argc < 2)
  {
    printUsageAndExit(argc, argv);
  }

  char c;
  while ((c = getopt(argc, argv, "Ds:p:c:r:m:")) != EOF)
  {
    switch (c)
    {
    case 'D': daemonMode = true; break;
    case 's': host = optarg; break;
    case 'p': port = atoi(optarg); break;
    case 'c': stream = optarg; break;
    case 'r': sleepTime = atoi(optarg); break;
    case 'm': componentMap = optarg; break;
    default: printUsageAndExit(argc, argv); break;
    }
  }

  if (host.empty() || stream.empty())
  {
    printUsageAndExit(argc, argv);
  }

  static const std::regex dot("\\.", std::regex::optimize);
  std::vector<std::string> tokens(splitString(stream, dot));
  if (tokens.size() != 4)
  {
    Log::error("-c option should be in the format STA.NET.LOC.CHA");
    quit();
  }
  string networkCode  = tokens[0];
  string stationCode  = tokens[1];
  string locationCode = tokens[2];
  string channelCode  = tokens[3];

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
                   locationCode, channelCode, componentMap);
  client.run();
  quit();
}
