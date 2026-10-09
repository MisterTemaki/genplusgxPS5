// Genesis Plus GX PS5: the covers downloaded in the background, by the helper payload.
//
// On the console the app itself can't download once it is out of its sandbox (fe_prefetch.h: every HTTPS
// GET fails after the jailbreak), so up to 1.4 it downloaded at the very start, before the jailbreak, up to 30 s at a
// time with the launch screen up, and restarted itself for new covers. The helper is a payload of its own
// that keeps running while the app runs. So it downloads them: the app writes covers/wanted.txt after each scan (and
// covers/priority.txt with the covers around the selection), the helper fetches them one by one into
// /data/genplus/covers, and the shelf shows each cover as it lands. covers/progress.txt says how far it got.
//
// 1.6: the console's libSceSsl fails in the helper too (1.5 on a PS5: every GET -> send 8095f00c in 20 ms; the
// helper runs outside any sandbox, like the app after its jailbreak), so the helper brings its own HTTPS:
// TlsHttp (fe_tlshttp.h, Mbed TLS with Mozilla's CA list, plain sockets).
//
// SPDX-License-Identifier: MIT
#pragma once

#include "fe_http.h"

#include <memory>
#include <string>

namespace fe
{
// The worker's HTTP client: the helper's TlsHttp (fe_tlshttp.cpp); a build without it (the app, which never runs
// the worker) gets the libSceHttp2 one.
std::unique_ptr<HttpClient> MakeCoverWorkerHttp();

// The helper, once it serves requests: starts the download thread (once).
void StartCoverWorker();

// covers/progress.txt, as the worker writes it: covers left, fetched since the helper started, and its state.
struct CoverProgress
{
	bool valid = false; // a progress file the worker updated in the last minute
	int left = 0;
	int fetched = 0;
	std::string state; // "busy", "idle", "offline"
};
CoverProgress ReadCoverProgress();

std::string PriorityListPath();
} // namespace fe
