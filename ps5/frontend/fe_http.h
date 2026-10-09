// Genesis Plus GX PS5 frontend: HTTPS GET through the console's own libSceHttp2 + libSceSsl, as PS5SX2's
// fe_ps5.cpp does it (and the payload SDK's http2_get sample).
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace fe
{
// What the cover code needs from an HTTP client: the app's (Http, the console's libSceHttp2) or the helper's
// (TlsHttp in fe_tlshttp.h, its own TLS: the console's libSceSsl fails outside the app's sandbox).
class HttpClient
{
public:
	virtual ~HttpClient() = default;
	// The HTTP status (200 on success), -1 when the request could not be made (no network...), -2 when the
	// answer is bigger than max_bytes. Only one thread may call Get.
	virtual int Get(const std::string& url, std::vector<uint8_t>& out, size_t max_bytes = 8u << 20) = 0;
	// The console reported no network at the last try.
	virtual bool Offline() const = 0;
	// Frees what the requests use (they start again on the next Get).
	virtual void Term() = 0;
};

class Http : public HttpClient
{
public:
	~Http() override { Term(); }
	int Get(const std::string& url, std::vector<uint8_t>& out, size_t max_bytes = 8u << 20) override;

private:
	int GetOnce(const std::string& url, std::vector<uint8_t>& out, size_t max_bytes, int attempt);

public:
	// Any thread: fails the request in flight (shutdown).
	void Abort();
	void Term() override;
	bool Offline() const override { return m_tried && !m_ok; }
	// No request (or retry) runs past this CLOCK_MONOTONIC time in seconds, and the timeouts of the last one
	// are cut to fit it; 0: no limit (the shelf's downloads).
	void SetDeadline(double t) { m_deadline = t; }

private:
	bool Init();
	double Remaining() const; // seconds left before the deadline (a large number when there is none)
	double m_deadline = 0;
	bool m_tried = false, m_ok = false, m_netctl = false;
	int m_pool = -1, m_ssl = -1, m_ctx = -1, m_tmpl = -1;
	std::atomic<int> m_active{-1};
	std::atomic<bool> m_stopping{false};
	std::mutex m_abort;
};

// "Super Metroid (Japan, USA) (En,Ja)" -> "Super%20Metroid%20%28Japan%2C%20USA%29%20%28En%2CJa%29"
std::string UrlEncode(const std::string& s);
} // namespace fe
