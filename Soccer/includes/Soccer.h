//
// Created by Fabrizio Paino on 2026-05-15.
//
// Umbrella header for the Soccer networking library. Pulls in every public
// type so user code can write a single `#include "Soccer.h"`.

#ifndef SOCCER_SOCCER_H
#define SOCCER_SOCCER_H

#include "SocketException.h"
#include "SocketAddress.h"
#include "TcpStream.h"
#include "TcpListener.h"
#include "TcpServer.h"
#include "UdpSocket.h"
#include "RawSocket.h"
#include "IcmpEcho.h"
#include "BufferedReader.h"
#include "HttpClient.h"
#include "HttpServer.h"

#ifdef SOCCER_HAS_TLS
#include "TlsStream.h"
#include "TlsListener.h"
#endif

#ifdef _WIN32
#include "Overlapped.h"
#endif

#endif // SOCCER_SOCCER_H
