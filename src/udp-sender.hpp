#pragma once

#include "alpha-packer.hpp"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#endif

class UdpSender {
public:
	UdpSender();
	~UdpSender();

	UdpSender(const UdpSender &) = delete;
	UdpSender &operator=(const UdpSender &) = delete;
	UdpSender(UdpSender &&) = delete;
	UdpSender &operator=(UdpSender &&) = delete;

	void set_endpoint(const std::string &host, uint16_t port);
	void submit_frame(const AlphaFrame &frame);
	void submit_blank_frame();
	void shutdown(bool flush_blank);

private:
	void worker_loop();
	void close_socket();
	bool ensure_socket();
	bool resolve_endpoint(const std::string &host, uint16_t port, sockaddr_in &out_addr);
	void send_packet(const AlphaFrame &frame, const sockaddr_in &addr);

	std::mutex mutex_;
	std::condition_variable cv_;
	std::thread worker_;

	std::string target_host_ = "espressif.local";
	uint16_t target_port_ = 5005;
	uint64_t endpoint_gen_ = 1;

	AlphaFrame pending_frame_{};
	bool has_pending_ = false;
	bool stop_ = false;
	bool flush_blank_ = false;
	bool stopped_ = false;

#ifdef _WIN32
	SOCKET sock_ = INVALID_SOCKET;
	bool wsa_initialized_ = false;
#else
	int sock_ = -1;
#endif
};
