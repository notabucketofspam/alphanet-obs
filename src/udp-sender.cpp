#include "udp-sender.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <chrono>
#include <cstring>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

UdpSender::UdpSender()
{
#ifdef _WIN32
	WSADATA wsa_data{};
	if (WSAStartup(MAKEWORD(2, 2), &wsa_data) == 0) {
		wsa_initialized_ = true;
	} else {
		obs_log(LOG_WARNING, "WSAStartup failed");
	}
#endif
	worker_ = std::thread(&UdpSender::worker_loop, this);
}

UdpSender::~UdpSender()
{
	shutdown(false);
}

void UdpSender::set_endpoint(const std::string &host, uint16_t port)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (host == target_host_ && port == target_port_)
		return;

	target_host_ = host;
	target_port_ = port;
	++endpoint_gen_;
	cv_.notify_one();
}

void UdpSender::submit_frame(const AlphaFrame &frame)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (stop_)
		return;
	pending_frame_ = frame;
	has_pending_ = true;
	cv_.notify_one();
}

void UdpSender::submit_blank_frame()
{
	AlphaFrame blank{};
	submit_frame(blank);
}

void UdpSender::shutdown(bool flush_blank)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (stopped_)
			return;
		stopped_ = true;
		stop_ = true;
		flush_blank_ = flush_blank;
		if (flush_blank) {
			pending_frame_.fill(0);
			has_pending_ = true;
		}
	}
	cv_.notify_one();

	if (worker_.joinable())
		worker_.join();

	close_socket();

#ifdef _WIN32
	if (wsa_initialized_) {
		WSACleanup();
		wsa_initialized_ = false;
	}
#endif
}

void UdpSender::close_socket()
{
#ifdef _WIN32
	if (sock_ != INVALID_SOCKET) {
		closesocket(sock_);
		sock_ = INVALID_SOCKET;
	}
#else
	if (sock_ >= 0) {
		close(sock_);
		sock_ = -1;
	}
#endif
}

bool UdpSender::ensure_socket()
{
#ifdef _WIN32
	if (!wsa_initialized_)
		return false;
	if (sock_ != INVALID_SOCKET)
		return true;
	sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	return sock_ != INVALID_SOCKET;
#else
	if (sock_ >= 0)
		return true;
	sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	return sock_ >= 0;
#endif
}

bool UdpSender::resolve_endpoint(const std::string &host, uint16_t port, sockaddr_in &out_addr)
{
	if (host.empty() || port == 0)
		return false;

	struct addrinfo hints{};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;

	struct addrinfo *res = nullptr;
	const std::string port_str = std::to_string(port);
	const int err = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
	if (err != 0 || !res) {
		if (res)
			freeaddrinfo(res);
		return false;
	}

	bool found = false;
	for (struct addrinfo *it = res; it != nullptr; it = it->ai_next) {
		if (it->ai_family == AF_INET && it->ai_addr && it->ai_addrlen >= sizeof(sockaddr_in)) {
			std::memcpy(&out_addr, it->ai_addr, sizeof(sockaddr_in));
			found = true;
			break;
		}
	}

	freeaddrinfo(res);
	return found;
}

void UdpSender::send_packet(const AlphaFrame &frame, const sockaddr_in &addr)
{
	if (!ensure_socket())
		return;

#ifdef _WIN32
	sendto(sock_, reinterpret_cast<const char *>(frame.data()), static_cast<int>(frame.size()), 0,
	       reinterpret_cast<const struct sockaddr *>(&addr), static_cast<int>(sizeof(addr)));
#else
	sendto(sock_, frame.data(), frame.size(), 0, reinterpret_cast<const struct sockaddr *>(&addr),
	       static_cast<socklen_t>(sizeof(addr)));
#endif
}

void UdpSender::worker_loop()
{
	using clock = std::chrono::steady_clock;

	uint64_t local_gen = 0;
	std::string local_host;
	uint16_t local_port = 0;
	sockaddr_in resolved_addr{};
	bool has_resolved = false;
	bool attempted_gen = false;
	bool logged_fail = false;
	clock::time_point next_retry = (clock::time_point::min)();

	std::unique_lock<std::mutex> lock(mutex_);
	for (;;) {
		if (stop_) {
			if (flush_blank_ && has_pending_) {
				const AlphaFrame frame = pending_frame_;
				has_pending_ = false;
				const bool gen_matches = (local_gen == endpoint_gen_);
				const std::string host = target_host_;
				const uint16_t port = target_port_;
				const bool can_try_resolve = !gen_matches || !attempted_gen;
				sockaddr_in addr = resolved_addr;
				bool send_ready = (has_resolved && gen_matches);

				lock.unlock();
				if (!send_ready && can_try_resolve) {
					send_ready = resolve_endpoint(host, port, addr);
				}
				if (send_ready) {
					send_packet(frame, addr);
				}
				lock.lock();
			}
			break;
		}

		if (endpoint_gen_ != local_gen) {
			local_host = target_host_;
			local_port = target_port_;
			local_gen = endpoint_gen_;
			has_resolved = false;
			attempted_gen = false;
			logged_fail = false;
			next_retry = (clock::time_point::min)();
		}

		if (!has_resolved) {
			if (local_host.empty() || local_port == 0) {
				attempted_gen = true;
				cv_.wait(lock, [&] { return stop_ || endpoint_gen_ != local_gen; });
				continue;
			}

			if (attempted_gen) {
				const auto now = clock::now();
				if (now < next_retry) {
					cv_.wait_until(lock, next_retry,
						       [&] { return stop_ || endpoint_gen_ != local_gen; });
					continue;
				}
			}

			const std::string host = local_host;
			const uint16_t port = local_port;
			lock.unlock();

			sockaddr_in addr{};
			const bool ok = resolve_endpoint(host, port, addr);

			lock.lock();
			if (endpoint_gen_ != local_gen)
				continue;

			attempted_gen = true;
			if (ok) {
				resolved_addr = addr;
				has_resolved = true;
				logged_fail = false;
			} else {
				if (!logged_fail) {
					obs_log(LOG_WARNING, "Failed to resolve UDP target %s:%u; retrying every 2s",
						host.c_str(), static_cast<unsigned>(port));
					logged_fail = true;
				}
				next_retry = clock::now() + std::chrono::seconds(2);
				continue;
			}
		}

		if (!has_pending_) {
			cv_.wait(lock, [&] { return stop_ || endpoint_gen_ != local_gen || has_pending_; });
			continue;
		}

		const AlphaFrame frame = pending_frame_;
		has_pending_ = false;
		const sockaddr_in addr = resolved_addr;

		lock.unlock();
		send_packet(frame, addr);
		lock.lock();
	}
}
