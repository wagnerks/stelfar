#include "LongFrameSampler.h"

#if defined(_WIN32) && defined(_M_X64)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace SFE::Debug::LongFrameSampler {
#if defined(_WIN32) && defined(_M_X64)
	namespace {
		// A thread of its own that wakes every millisecond, stops the main thread long enough to walk its stack (the
		// unwind tables of x64, so nothing it calls has to be built for it), and keeps what it found until the frame is
		// over: written out with the symbols' names and lines if the frame lasted 40 ms or more, dropped if not.
		struct Sampler {
			std::atomic<long long> began{ 0 };
			std::atomic<bool> run{ true };
			std::thread thread;
			HANDLE main = nullptr;

			void start(const char* path) {
				// One sampler, started once.
				if (thread.joinable()) {
					return;
				}
				run.store(true);
				DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &main, 0, FALSE, DUPLICATE_SAME_ACCESS);
				thread = std::thread([this, file = std::string(path)] {
					SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
					SymInitialize(GetCurrentProcess(), nullptr, TRUE);
					struct Sample { float at; int count; DWORD64 pcs[48]; };
					std::vector<Sample> samples;
					long long sampled = 0;
					const auto write = [&](float lasted) {
						FILE* out = nullptr;
						if (fopen_s(&out, file.c_str(), "a") != 0 || !out) {
							return;
						}
						std::fprintf(out, "== frame of at least %.1f ms, %zu samples\n", lasted, samples.size());
						alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
						for (size_t k = 0; k < samples.size(); ++k) {
							const auto& sample = samples[k];
							if (k > 0 && samples[k - 1].count == sample.count && std::equal(sample.pcs, sample.pcs + sample.count, samples[k - 1].pcs)) {
								std::fprintf(out, "  at %.1f ms: same\n", sample.at);
								continue;
							}
							std::fprintf(out, "  at %.1f ms:\n", sample.at);
							for (int f = 0; f < sample.count && f < 40; ++f) {
								auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
								symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
								symbol->MaxNameLen = 255;
								DWORD64 offset = 0;
								IMAGEHLP_LINE64 line{ sizeof(IMAGEHLP_LINE64) };
								DWORD column = 0;
								const bool named = SymFromAddr(GetCurrentProcess(), sample.pcs[f], &offset, symbol);
								const bool lined = SymGetLineFromAddr64(GetCurrentProcess(), sample.pcs[f], &column, &line);
								std::fprintf(out, "    %s %s:%lu\n", named ? symbol->Name : "?", lined ? line.FileName : "", lined ? line.LineNumber : 0ul);
							}
						}
						std::fclose(out);
					};
					while (run.load(std::memory_order_relaxed)) {
						std::this_thread::sleep_for(std::chrono::milliseconds(1));
						const long long now = std::chrono::steady_clock::now().time_since_epoch().count();
						const long long frame = began.load(std::memory_order_relaxed);
						if (frame != sampled && !samples.empty()) {
							if (samples.back().at >= 40.f) {
								write(samples.back().at);
							}
							samples.clear();
						}
						const float ms = static_cast<float>(now - frame) * 1e3f * std::chrono::steady_clock::period::num / std::chrono::steady_clock::period::den;
						if (frame == 0) {
							continue;
						}
						sampled = frame;
						Sample sample{ ms, 0, {} };
						if (SuspendThread(main) == static_cast<DWORD>(-1)) {
							continue;
						}
						CONTEXT context{};
						context.ContextFlags = CONTEXT_FULL;
						if (GetThreadContext(main, &context)) {
							while (sample.count < 48 && context.Rip) {
								sample.pcs[sample.count++] = context.Rip;
								DWORD64 base = 0;
								auto* entry = RtlLookupFunctionEntry(context.Rip, &base, nullptr);
								if (!entry) {
									context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
									context.Rsp += 8;
									continue;
								}
								void* handler = nullptr;
								DWORD64 frameAt = 0;
								RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, entry, &context, &handler, &frameAt, nullptr);
							}
						}
						ResumeThread(main);
						samples.push_back(sample);
					}
				});
			}
			~Sampler() { stop(); }
			void frame() { began.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed); }
			void stop() {
				run.store(false);
				if (thread.joinable()) {
					thread.join();
				}
				if (main) {
					CloseHandle(main);
					main = nullptr;
				}
			}
		};
		Sampler gSampler;
	}

	void start(const char* path) {
		gSampler.start(path);
	}

	void frame() {
		gSampler.frame();
	}

	void stop() {
		gSampler.stop();
	}
#else
	void start(const char*) {}

	void frame() {}

	void stop() {}
#endif
}
