# 
# A server that complements the RAW seedlink plugin. 
# The server listen to RAW clients (seelink plugins) on a TCP socket and
# send them the data of the channels they subscribes to.
# 
# This reference server can be used without modification to send your own data.
# See the main function for an example on how to use it
#
# Copyright (c) 2021 Swiss Seismological Service (SED)
# 
# Written by Luca Scarabello @ ETH Zuerich
#


import asyncio
import multiprocessing
import logging
import datetime
import time
import sys
import numpy as np

import math
from random import randrange


def get_logger(name):
    logger = logging.getLogger(name)
    logger.setLevel(logging.INFO)
    formatter = logging.Formatter(
        '[%(asctime)s] %(levelname)s {%(pathname)s:%(lineno)d} - %(message)s',
        datefmt="%d.%m.%Y %H:%M:%S")
    hdlr = logging.StreamHandler()  # RotatingFileHandler('raw_servers.log')
    hdlr.setFormatter(formatter)
    logger.addHandler(hdlr)
    return logger


logger = get_logger(__name__)  # logging.getLogger(__name__)


class Data:
    def __init__(self, channel_id, time, samples, num_samples):
        self.channel_id = channel_id
        self.time = time
        self.samples = samples
        self.num_samples = num_samples


class Client:
    def __init__(self, reader, writer):
        self.reader = reader
        self.writer = writer
        self.peername = writer.get_extra_info('peername')
        self.channel_ids = []
        self.data_available = asyncio.Event()
        self.data = []

    async def readline(self):
        line = await self.reader.readline()
        return line.decode()[:-1]

    async def writeline(self, line):
        self.writer.write((line + '\n').encode(encoding='UTF-8',
                                               errors='strict'))
        await self.writer.drain()

    async def close_connection(self):
        logger.info(f"Closing connection with {self.peername!r}")
        try:
            self.writer.close()
            await self.writer.wait_closed()
        except Exception as e:
            logger.info(f"{e}")

    def feed(self, data):
        if data.channel_id in self.channel_ids:
            self.data.append(data)
            self.data_available.set()

    async def handle_connection(self):
        if not self.channel_ids:
            return
        while True:
            if not self.data:
                await self.data_available.wait()
                self.data_available.clear()
            if self.data:
                data = self.data.pop(0)
                #
                # Send header
                #
                self.writer.write(
                    data.time.year.to_bytes(length=2,
                                            byteorder='big',
                                            signed=False))
                self.writer.write(data.time.timetuple().tm_yday.to_bytes(
                    length=2, byteorder='big', signed=False))
                self.writer.write(
                    data.time.hour.to_bytes(length=1,
                                            byteorder='big',
                                            signed=False))
                self.writer.write(
                    data.time.minute.to_bytes(length=1,
                                              byteorder='big',
                                              signed=False))
                self.writer.write(
                    data.time.second.to_bytes(length=1,
                                              byteorder='big',
                                              signed=False))
                self.writer.write(
                    data.time.microsecond.to_bytes(length=4,
                                                   byteorder='big',
                                                   signed=False))
                self.writer.write(
                    data.channel_id.to_bytes(length=4,
                                             byteorder='big',
                                             signed=False))
                self.writer.write(
                    data.num_samples.to_bytes(length=4,
                                              byteorder='big',
                                              signed=False))

                # logger.debug(f"Sending {data.num_samples} samples from channel "
                #        "{data.channel_id} (year {data.time.year} day "
                #        "{data.time.timetuple().tm_yday} hour "
                #        "{data.time.hour} min {data.time.minute} sec "
                #        "{data.time.second} usec {data.time.microsecond})")

                #
                # Send samples
                #
                self.writer.write(data.samples)

                await self.writer.drain()


class Channel:
    def __init__(self, id, samprate, endianness, sampsize):
        self.id = id
        self.samprate = samprate  # samples per second
        self.endianness = endianness  # big or little
        self.sampsize = sampsize  # bytes per sample


class Server:
    def __init__(self, channels, data_conn, host, port, backlog):
        self.clients = []
        self.channels = channels
        self.data_conn = data_conn
        self.data_conn_closed = None
        self.host = host
        self.port = port
        self.backlog = backlog

    async def run(self):
        data_task = asyncio.create_task(self.run_data_reader())
        server_task = asyncio.create_task(self.run_data_streamer())
        # Loop forever or until:
        # - the calling process closed the other end of the date connection
        # - the server encountered an exception
        # Either way, we finished our job and exit
        done, pending = await asyncio.wait([data_task, server_task],
                                           return_when=asyncio.FIRST_COMPLETED)
        if data_task in pending:
            logger.info("Closing data connection...")
            data_task.cancel()
            await data_task
        if server_task in pending:
            logger.info("Shutting down the server...")
            server_task.cancel()
            await server_task
        logger.info("Closing client connections...")
        tasks = []
        for client in self.clients:
            tasks.append(client.close_connection())
        await asyncio.gather(*tasks, return_exceptions=True)
        logger.info("Shutdown completed")

    async def run_data_reader(self):
        self.data_conn_closed = asyncio.Event()
        asyncio.get_event_loop().add_reader(self.data_conn.fileno(),
                                            self.data_ready)
        try:
            await self.data_conn_closed.wait()
        except asyncio.CancelledError:
            pass
        except Exception as e:
            logger.error(f"Unexpected Exception: {e}")
        finally:
            self.data_conn.close()
        logger.info("Data collection task terminated")

    def data_ready(self):
        try:
            data = self.data_conn.recv()
        except EOFError:
            pass
        except Exception as e:
            logger.error(f"Unexpected Exception: {e}")
        if not isinstance(data, Data):
            logger.info("Data connection closed")
            self.data_conn_closed.set()
            return
        for client in self.clients:
            if data.channel_id in client.channel_ids:
                client.feed(data)

    async def run_data_streamer(self):
        server = await asyncio.start_server(self.client_connected,
                                            host=self.host,
                                            port=self.port,
                                            backlog=self.backlog,
                                            start_serving=False)
        addrs = ', '.join(str(sock.getsockname()) for sock in server.sockets)
        logger.info(
            f"Server started on {addrs}, serving channels {[c for c in self.channels ]}"
        )
        async with server:
            try:
                await server.serve_forever()
            except asyncio.CancelledError:
                pass
            except Exception as e:
                logger.error(f"Unexpected Exception: {e}")
        logger.info(
            f"Server running on {self.host} port {self.port} terminated")

    async def client_connected(self, reader, writer):
        client = Client(reader, writer)
        logger.info(f"New connection from {client.peername!r}")
        try:
            performed = await self.client_handshake(client)
            if performed:
                logger.info(
                    f"Handshake completed with {client.peername!r}. Streaming data..."
                )
                self.clients.append(client)
                await client.handle_connection()
            else:
                logger.error(f"Handshake failed with {client.peername!r}")
        except Exception as e:
            logger.error(f"Exception with peer {client.peername!r}: {e}")
        finally:
            if client in self.clients:
                self.clients.remove(client)
            await client.close_connection()

    async def client_handshake(self, client):
        line = await client.readline()
        if line != "RAW 1.0":
            logger.error(f"Received {line}")
            return False
        await client.writeline("RAW 1.0")
        while True:
            line = await client.readline()

            if line == "CHANNEL":
                channel_id = await client.readline()
                try:
                    channel_id = int(channel_id)
                except ValueError as e:
                    pass
                if channel_id not in self.channels:
                    logger.error(
                        f"Client {client.peername!r} requested unknown channel {channel_id}"
                    )
                    return False
                logger.info(f"Client requested channel {channel_id}")
                client.channel_ids.append(channel_id)
                await client.writeline("SAMPLING RATE")
                await client.writeline(str(self.channels[channel_id].samprate))
                await client.writeline("SAMPLE ENDIANNESS")
                await client.writeline(self.channels[channel_id].endianness)
                await client.writeline("SAMPLE TYPE")
                await client.writeline(
                    "int%d" % (self.channels[channel_id].sampsize * 8))

            elif line == "START":
                if not client.channel_ids:
                    logger.error(
                        f"Client {client.peername!r} requested no channels")
                    return False
                await client.writeline("STARTING")
                return True

            else:
                logger.error(
                    f"Received unexpected data from {client.peername!r}: {line}"
                )
                return False


def _start_asyncio_server(channels, data_conn, host, port, backlog):

    global logger
    logger = logging.getLogger("streamer")
    logger.setLevel(logging.INFO)
    formatter = logging.Formatter(
        '[%(asctime)s] %(levelname)s [pid %(process)d %(pathname)s:%(lineno)d] - %(message)s',
        datefmt="%d.%m.%Y %H:%M:%S")
    hdlr = logging.StreamHandler()  # RotatingFileHandler('servers.log')
    hdlr.setFormatter(formatter)
    logger.addHandler(hdlr)

    logging.getLogger("asyncio").setLevel(logging.WARNING)

    server = Server(channels, data_conn, host, port, backlog)
    asyncio.run(server.run())


class Streamer():
    def __init__(self, channels, host="127.0.0.1", port=65535):
        self.channels = {c.id: c for c in channels}
        self.host = host
        self.port = port
        self.data_conn = None
        self.server_process = None
        self.last_restart = None

    def start(self):
        read_conn, write_conn = multiprocessing.Pipe(duplex=False)
        server_process = multiprocessing.Process(target=_start_asyncio_server,
                                                 args=(self.channels,
                                                       read_conn, self.host,
                                                       self.port,
                                                       len(self.channels) * 2))
        server_process.daemon = True
        self.server_process = server_process
        self.data_conn = write_conn
        self.server_process.start()
        self.last_start = datetime.datetime.utcnow()

    def feed_data(self, channel_id, samptime, samples):
        if channel_id not in self.channels:
            logger.warning(f"Channel id {channel_id} not configured")
            return
        data = Data(channel_id, samptime, samples.tobytes(), samples.size)
        try:
            self.data_conn.send(data)
        except Exception as e:
            logger.error(f"Exception while feeding data: {e}")
            minimum_elapsed_time = datetime.timedelta(seconds=60)
            now = datetime.datetime.utcnow()
            if now - self.last_start > minimum_elapsed_time:
                logger.info("Restarting server...")
                self.stop()
                self.start()

    def stop(self):
        # This will shut down the server process
        try:
            self.data_conn.send(b"STOP")
        except Exception:
            pass
        try:
            self.data_conn.close()
        except Exception:
            pass
        self.server_process.join(60)
        if self.server_process.exitcode is None:
            # in case the server didn't exit we force it
            self.server_process.kill()
            self.server_process.join(10)
        if self.server_process.exitcode is None:
            # in case the server didn't exit we force it
            self.server_process.terminate()
            self.server_process.join(10)
        try:
            self.server_process.close()
        except Exception as e:
            logger.error(f"Unexpected Exception: {e}")
        self.data_conn = None
        self.server_process = None


if __name__ == "__main__":
    #
    # Test with 3 streamer servers
    #
    channels1 = [Channel(i, 100, 'big', 2) for i in range(1, 5)]
    channels2 = [Channel(i, 4000, 'little', 1) for i in range(5, 10)]
    channels3 = [Channel(i, 200000, sys.byteorder, 4) for i in range(10, 15)]
    streamers = [
        Streamer(channels1, host="127.0.0.1", port=65535),
        Streamer(channels2, host="127.0.0.1", port=65534),
        Streamer(channels3, host="127.0.0.1", port=65533),
    ]
    #
    # start the servers
    #
    logger.info("Starting servers...")
    for streamer in streamers:
        streamer.start()
    #
    # simulate data
    #
    duration = datetime.timedelta(seconds=300)
    start = datetime.datetime.utcnow()
    next_samples = start

    while next_samples - start < duration:

        next_samples += datetime.timedelta(seconds=1)
        now = datetime.datetime.utcnow()
        sleep_time = (next_samples - now).total_seconds()

        if sleep_time > 0:
            time.sleep(sleep_time)

        for streamer in streamers:

            for channel in streamer.channels.values():

                simulate_pick = randrange(0, 50) == 0
                num_samples = channel.samprate
                samples = []
                for i in range(num_samples):
                    s = int(math.sin(i * math.pi * 2. / num_samples) * 60)
                    if simulate_pick:
                        s *= 2
                    samples.append(s)

                streamer.feed_data(
                    channel.id, next_samples,
                    np.ascontiguousarray(
                        samples,
                        dtype='%si%d' %
                        ('>' if channel.endianness == 'big' else '<',
                         channel.sampsize)))
    #
    # stop the servers
    #
    logger.info("Stopping servers...")
    for streamer in streamers:
        streamer.stop()
