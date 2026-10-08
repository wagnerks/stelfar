#include "Profiler.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <json/value.h>
#include <json/writer.h>

#include "core/Engine.h"
#include "core/LoadTrace.h"
#include "imgui.h"
#include "systemsModule/systems/RenderSystem.h"

namespace SFE::Debug::Profiler {
	namespace {
		using Clock = std::chrono::steady_clock;

		constexpr std::string_view kMainGroup = "Main";
		constexpr const char* kGpuGroup = "GPU";
		// What holds the frame back (Overview::bound).
		constexpr const char* kByGpu = "GPU";
		constexpr const char* kByCpu = "CPU";
		constexpr const char* kByFpsLimit = "fps limit";

		double toMs(int64_t ticks) {
			return std::chrono::duration<double, std::milli>(Clock::duration(ticks)).count();
		}

		struct Event {
			const char* name = nullptr;
			int64_t begin = 0;
			int64_t end = 0;
		};

		// One thread's zones on their way to the main thread: written by that thread alone, taken by the main thread
		// alone. A thread this far ahead of the main thread's frames, loading say, loses what does not fit and counts it.
		struct Lane {
			static constexpr uint32_t kSize = 2048;
			Event events[kSize];
			std::atomic<uint32_t> written{ 0 };
			std::atomic<uint32_t> taken{ 0 };
			std::atomic<uint32_t> dropped{ 0 };
			// Under the lanes' mutex.
			int number = 0;
			const char* group = "Thread";
			const char* label = "Thread";
			int index = 0; // within the group; none for the main thread
			bool main = false;
		};

		// Every lane, kept past the end of the program: a pool thread may still close a zone while the statics go.
		struct Lanes {
			std::mutex mutex;
			std::vector<Lane*> all;
		};

		Lanes& lanes() {
			static auto* kept = new Lanes;
			return *kept;
		}

		// What nameThread() said, for the lane the thread's first zone makes: a thread that never records costs nothing.
		thread_local const char* tGroup = "Thread";
		thread_local int tIndex = 0;
		thread_local Lane* tLane = nullptr;

		// Under the lanes' mutex. A thread no one named is numbered by when it first recorded.
		void label(Lane& lane) {
			lane.group = tGroup;
			lane.main = tGroup == kMainGroup;
			lane.index = tIndex > 0 ? tIndex : lane.main ? 0 : lane.number;
			lane.label = lane.index > 0 ? intern(std::string(tGroup) + " " + std::to_string(lane.index)) : tGroup;
		}

		Lane& ownLane() {
			if (!tLane) {
				auto* lane = new Lane;
				auto& kept = lanes();
				std::lock_guard lock(kept.mutex);
				kept.all.push_back(lane);
				lane->number = static_cast<int>(kept.all.size());
				label(*lane);
				tLane = lane;
			}
			return *tLane;
		}

		void record(Lane& lane, const Event& event) {
			const uint32_t written = lane.written.load(std::memory_order_relaxed);
			if (written - lane.taken.load(std::memory_order_acquire) >= Lane::kSize) {
				lane.dropped.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			lane.events[written % Lane::kSize] = event;
			lane.written.store(written + 1, std::memory_order_release);
		}

		// What one zone costs the thread that records it, measured once: the clock read twice and a write into a lane.
		double zoneNs() {
			static const double measured = [] {
				auto lane = std::make_unique<Lane>();
				const int64_t from = now();
				for (uint32_t i = 0; i < Lane::kSize; ++i) {
					const int64_t begin = now();
					record(*lane, { "", begin, now() });
				}
				return toMs(now() - from) * 1e6 / Lane::kSize;
			}();
			return measured;
		}

		constexpr int kKept = 600;

		// One zone's name on one group of threads, or one render pass on the GPU, frame by frame.
		// A pipeline's on the GPU has the pass it was drawn in as its group.
		struct Stat {
			std::string name;
			std::string group;
			bool gpu = false;
			float total[kKept]{};
			float own[kKept]{};
			uint16_t calls[kKept]{};
			double frameTotal = 0.0;
			double frameOwn = 0.0;
			int frameCalls = 0;
		};

		struct Frame {
			int64_t begin = 0;
			int64_t end = 0;
			float gpuMs = 0.f;
			float mainZonedMs = 0.f; // what the main thread's outermost zones cover
			int zones = 0;
		};

		struct PointerHash {
			size_t operator()(const std::pair<const char*, const char*>& key) const {
				return std::hash<const void*>{}(key.first) * 31 ^ std::hash<const void*>{}(key.second);
			}
		};

		// The main thread's alone.
		struct State {
			std::vector<std::unique_ptr<Stat>> stats;
			std::unordered_map<std::pair<const char*, const char*>, Stat*, PointerHash> byPointer;
			std::unordered_map<std::string, Stat*> byText;
			std::unordered_map<const char*, std::string> shownNames;
			Frame frames[kKept];
			int64_t counted = 0;
			int64_t frameBegin = 0;
			uint64_t dropped = 0;
			std::vector<Event> nesting;

			bool open = false;
			bool paused = false;
			int averaged = 120;
			float zoom = 1.f;
			// A zone of the last frame, and the thread it was on.
			struct Shown {
				const char* label;
				const char* group;
				int index;
				bool main;
				Event event;
			};
			std::vector<Shown> timeline;
			int64_t timelineBegin = 0;
			int64_t timelineEnd = 0;
			std::string saved;
		};

		State& state() {
			static State kept;
			return kept;
		}

		// A zone's name as shown: a type's as MSVC gives it ("class SFE::SystemsModule::RenderSystem") without its
		// namespaces.
		const std::string& shown(State& s, const char* name) {
			auto [it, added] = s.shownNames.try_emplace(name);
			if (added) {
				const std::string_view text(name);
				it->second = text.rfind("class ", 0) == 0 || text.rfind("struct ", 0) == 0 ? LoadTrace::plainName(text) : std::string(text);
			}
			return it->second;
		}

		Stat& statFor(State& s, const char* name, const char* group, bool gpu = false) {
			const auto key = std::make_pair(name, group);
			if (const auto it = s.byPointer.find(key); it != s.byPointer.end()) {
				return *it->second;
			}
			const auto& text = shown(s, name);
			auto& found = s.byText[text + '\n' + group];
			if (!found) {
				found = s.stats.emplace_back(std::make_unique<Stat>()).get();
				found->name = text;
				found->group = group;
				found->gpu = gpu;
			}
			s.byPointer.emplace(key, found);
			return *found;
		}

		struct Row {
			const Stat* stat = nullptr;
			double mean = 0.0;
			double own = 0.0;
			double worst = 0.0;
			double calls = 0.0;
		};

		int framesIn(const State& s, int frames) {
			const auto kept = static_cast<int>((std::min<int64_t>)(s.counted, kKept));
			return frames > 0 ? (std::min)(frames, kept) : kept;
		}

		// The slot of the frame `back` frames before the last one counted.
		int slotOf(const State& s, int back) {
			return static_cast<int>((s.counted - 1 - back) % kKept);
		}

		// The zones seen in the last `n` frames, the CPU's or the GPU's, by their own time per frame, most first.
		std::vector<Row> rows(const State& s, int n, bool gpu) {
			std::vector<Row> found;
			for (const auto& stat : s.stats) {
				if (stat->gpu != gpu) {
					continue;
				}
				Row row{ stat.get() };
				for (int back = 0; back < n; ++back) {
					const int slot = slotOf(s, back);
					row.mean += stat->total[slot];
					row.own += stat->own[slot];
					row.calls += stat->calls[slot];
					row.worst = (std::max)(row.worst, static_cast<double>(stat->total[slot]));
				}
				if (row.calls > 0.0) {
					row.mean /= n;
					row.own /= n;
					row.calls /= n;
					found.push_back(row);
				}
			}
			std::sort(found.begin(), found.end(), [](const Row& a, const Row& b) { return a.own > b.own; });
			return found;
		}

		// A pass's pipelines on the GPU: the GPU's rows that have the pass as their group.
		std::vector<const Row*> partsOf(const std::vector<Row>& gpu, const Row& pass) {
			std::vector<const Row*> parts;
			for (const auto& row : gpu) {
				if (row.stat->group == pass.stat->name) {
					parts.push_back(&row);
				}
			}
			return parts;
		}

		bool waiting(const std::string& name) {
			return name == kWaitGpu || name == kIdle || name == kPresent;
		}

		struct Overview {
			double seconds = 0.0;
			double frameMs = 0.0;
			double worstMs = 0.0;
			double gpuMs = 0.0;
			double gpuWorstMs = 0.0;
			double waitGpuMs = 0.0;
			double idleMs = 0.0;
			double presentMs = 0.0;
			double unzonedMs = 0.0; // the main thread's time in no zone
			double gpuBusy = 0.0;   // the part of the frame the GPU works
			double workMs = 0.0;    // the main thread's own work: the frame less its waits
			double cpuBusy = 0.0;   // the part of the frame the main thread works
			const char* bound = kByCpu;
			double zones = 0.0;
			double drainMs = 0.0;
			double costMs = 0.0;
		};

		Overview overview(const State& s, int n, const std::vector<Row>& cpu) {
			Overview o;
			if (n == 0) {
				return o;
			}
			for (int back = 0; back < n; ++back) {
				const auto& frame = s.frames[slotOf(s, back)];
				const double ms = toMs(frame.end - frame.begin);
				o.seconds += ms / 1000.0;
				o.frameMs += ms;
				o.worstMs = (std::max)(o.worstMs, ms);
				o.gpuMs += frame.gpuMs;
				o.gpuWorstMs = (std::max)(o.gpuWorstMs, static_cast<double>(frame.gpuMs));
				o.unzonedMs += ms - frame.mainZonedMs;
				o.zones += frame.zones;
			}
			o.frameMs /= n;
			o.gpuMs /= n;
			o.unzonedMs /= n;
			o.zones /= n;
			for (const auto& row : cpu) {
				if (row.stat->group != kMainGroup) {
					continue;
				}
				const auto& name = row.stat->name;
				if (name == kWaitGpu) {
					o.waitGpuMs += row.mean;
				}
				else if (name == kIdle) {
					o.idleMs += row.mean;
				}
				else if (name == kPresent) {
					o.presentMs += row.mean;
				}
				else if (name == "Profiler") {
					o.drainMs += row.mean;
				}
			}
			// What holds the frame back: the fps limit when the main thread waits a tenth of the frame for it, the GPU when it
			// works nine tenths of the frame (one that keeps up idles between frames), the CPU otherwise. Not from where the
			// main thread waits for the GPU: that is the backend's, and on OpenGL the driver blocks inside Submit as well.
			o.gpuBusy = o.frameMs > 0.0 ? o.gpuMs / o.frameMs : 0.0;
			o.workMs = (std::max)(o.frameMs - o.waitGpuMs - o.presentMs - o.idleMs, 0.0);
			o.cpuBusy = o.frameMs > 0.0 ? o.workMs / o.frameMs : 0.0;
			o.bound = o.idleMs >= 0.1 * o.frameMs ? kByFpsLimit : o.gpuBusy >= 0.9 ? kByGpu : kByCpu;
			o.costMs = o.zones * zoneNs() * 1e-6 + o.drainMs;
			return o;
		}

		// Each group of threads: how many have worked so far, and their work per frame in all, waits left out.
		struct Busy {
			std::string group;
			int threads = 0;
			double ms = 0.0;
		};

		std::vector<Busy> busy(const std::vector<Row>& cpu) {
			std::vector<Busy> groups;
			for (const auto& row : cpu) {
				auto it = std::find_if(groups.begin(), groups.end(), [&](const Busy& group) { return group.group == row.stat->group; });
				if (it == groups.end()) {
					it = groups.insert(groups.end(), Busy{ row.stat->group });
				}
				if (!waiting(row.stat->name)) {
					it->ms += row.own;
				}
			}
			auto& kept = lanes();
			std::lock_guard lock(kept.mutex);
			for (const auto* lane : kept.all) {
				for (auto& group : groups) {
					group.threads += group.group == lane->group ? 1 : 0;
				}
			}
			std::sort(groups.begin(), groups.end(), [](const Busy& a, const Busy& b) { return a.ms > b.ms; });
			return groups;
		}

		std::string summaryText(int frames) {
			auto& s = state();
			const int n = framesIn(s, frames);
			const auto cpu = rows(s, n, false);
			const auto gpu = rows(s, n, true);
			const auto o = overview(s, n, cpu);
			std::string text;
			const auto put = [&text](const char* format, auto... values) {
				char line[512];
				std::snprintf(line, sizeof(line), format, values...);
				text += line;
			};
			put("The last %d frames, %.1f s\n", n, o.seconds);
			if (n == 0) {
				return text;
			}
			put("Frame  %.2f ms, worst %.2f (%.0f fps)\n", o.frameMs, o.worstMs, o.frameMs > 0.0 ? 1000.0 / o.frameMs : 0.0);
			put("GPU    %.2f ms, worst %.2f: busy %.0f%% of the frame\n", o.gpuMs, o.gpuWorstMs, o.gpuBusy * 100.0);
			put("Main   works %.2f ms: busy %.0f%% of the frame\n", o.workMs, o.cpuBusy * 100.0);
			put("Main   waits for the GPU %.2f ms, on present %.2f, for the fps limit %.2f; in no zone %.2f\n", o.waitGpuMs,
				o.presentMs, o.idleMs, o.unzonedMs);
			put("Held back by the %s\n", o.bound);
			put("\nCPU, ms per frame, by own time (less the zones inside):\n  %-36s %-10s %8s %8s %8s %7s\n", "zone", "thread", "own", "mean", "worst", "calls");
			for (const auto& row : cpu) {
				put("  %-36s %-10s %8.3f %8.3f %8.3f %7.1f\n", row.stat->name.c_str(), row.stat->group.c_str(), row.own, row.mean, row.worst, row.calls);
			}
			put("\nGPU, ms per frame:\n  %-46s %8s %8s\n", "pass, and the pipelines drawn in it", "mean", "worst");
			for (const auto& row : gpu) {
				if (row.stat->group != kGpuGroup) {
					continue;
				}
				put("  %-46s %8.3f %8.3f\n", row.stat->name.c_str(), row.mean, row.worst);
				for (const auto* part : partsOf(gpu, row)) {
					put("    %-44s %8.3f %8.3f\n", part->stat->name.c_str(), part->mean, part->worst);
				}
			}
			put("\nThreads, ms of work per frame:\n");
			for (const auto& group : busy(cpu)) {
				put("  %-10s %8.3f on %d threads\n", group.group.c_str(), group.ms, group.threads);
			}
			put("\nProfiler: %.0f zones per frame at %.0f ns, taking them in %.3f ms: %.3f ms per frame (%.2f%%); %llu zones lost since the start\n",
				o.zones, zoneNs(), o.drainMs, o.costMs, o.frameMs > 0.0 ? o.costMs / o.frameMs * 100.0 : 0.0,
				static_cast<unsigned long long>(s.dropped));
			return text;
		}

		// Where the window's Save writes: beside the game, a file a press.
		std::string savedPath() {
			const std::time_t time = std::time(nullptr);
			std::tm local{};
#ifdef _WIN32
			localtime_s(&local, &time);
#else
			localtime_r(&time, &local);
#endif
			char name[64];
			std::strftime(name, sizeof(name), "profiles/profile_%Y%m%d_%H%M%S.txt", &local);
			return name;
		}

		// The last frame: a line of each thread that worked in it -- the main thread, the tick systems', the pools' -- and
		// each zone on it as deep as it sits.
		void drawTimeline(State& s) {
			if (s.timelineEnd <= s.timelineBegin) {
				ImGui::TextDisabled("No frame yet");
				return;
			}
			const float font = ImGui::GetFontSize();
			ImGui::SetNextItemWidth(font * 10.f);
			ImGui::SliderFloat("zoom", &s.zoom, 1.f, 64.f, "x%.1f", ImGuiSliderFlags_Logarithmic);
			ImGui::SameLine();
			const double spanMs = toMs(s.timelineEnd - s.timelineBegin);
			ImGui::TextDisabled("the last frame, %.2f ms; Pause holds it", spanMs);

			struct Line {
				const State::Shown* thread; // its first zone: which thread it is
				std::vector<std::pair<const Event*, int>> zones;
				int depth = 0;
			};
			std::vector<Line> lines;
			for (const auto& zoneShown : s.timeline) {
				if (zoneShown.event.end < s.timelineBegin || zoneShown.event.begin > s.timelineEnd) {
					continue;
				}
				auto it = std::find_if(lines.begin(), lines.end(), [&](const Line& line) { return line.thread->label == zoneShown.label; });
				if (it == lines.end()) {
					it = lines.insert(lines.end(), Line{ &zoneShown });
				}
				it->zones.emplace_back(&zoneShown.event, 0);
			}
			// The main thread, then the tick systems', then each pool's in their numbers' order.
			const auto rank = [](const State::Shown& thread) { return thread.main ? 0 : std::strcmp(thread.group, "Tick") == 0 ? 1 : 2; };
			std::sort(lines.begin(), lines.end(), [&](const Line& a, const Line& b) {
				const auto& x = *a.thread;
				const auto& y = *b.thread;
				if (rank(x) != rank(y)) {
					return rank(x) < rank(y);
				}
				const int byGroup = std::strcmp(x.group, y.group);
				return byGroup != 0 ? byGroup < 0 : x.index < y.index;
			});
			float labelWidth = 0.f;
			for (auto& line : lines) {
				labelWidth = (std::max)(labelWidth, ImGui::CalcTextSize(line.thread->label).x);
				std::sort(line.zones.begin(), line.zones.end(), [](const auto& a, const auto& b) {
					return a.first->begin != b.first->begin ? a.first->begin < b.first->begin : a.first->end > b.first->end;
				});
				std::vector<int64_t> open;
				for (auto& [event, depth] : line.zones) {
					while (!open.empty() && open.back() <= event->begin) {
						open.pop_back();
					}
					depth = static_cast<int>(open.size());
					open.push_back(event->end);
					line.depth = (std::max)(line.depth, depth);
				}
			}
			labelWidth += font;

			const auto& style = ImGui::GetStyle();
			const float row = ImGui::GetTextLineHeight() + 3.f;
			const float gap = 3.f;
			float height = row;
			for (const auto& line : lines) {
				height += row * static_cast<float>(line.depth + 1) + gap;
			}
			// Scrolled past some lines of threads, so the window keeps its tables in sight.
			ImGui::BeginChild("timeline", ImVec2(0.f, (std::min)(height + style.ScrollbarSize + style.WindowPadding.y * 2.f, font * 18.f)),
				ImGuiChildFlags_Border, ImGuiWindowFlags_HorizontalScrollbar);
			const float width = (std::max)(ImGui::GetContentRegionAvail().x - labelWidth, 50.f) * s.zoom;
			const ImVec2 origin = ImGui::GetCursorScreenPos();
			auto* draw = ImGui::GetWindowDrawList();
			const auto xOf = [&](int64_t at) {
				return origin.x + labelWidth + static_cast<float>(toMs(at - s.timelineBegin) / spanMs) * width;
			};

			// A mark every so many milliseconds, as many as fit apart.
			double step = 1000.0;
			for (const double candidate : { 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 50.0, 100.0, 200.0, 500.0 }) {
				if (candidate / spanMs * width >= font * 4.f) {
					step = candidate;
					break;
				}
			}
			for (double at = 0.0; at <= spanMs; at += step) {
				const float x = origin.x + labelWidth + static_cast<float>(at / spanMs) * width;
				draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + height), IM_COL32(255, 255, 255, 28));
				char mark[24];
				std::snprintf(mark, sizeof(mark), step < 0.1 ? "%.2f" : step < 1.0 ? "%.1f" : "%.0f", at);
				draw->AddText(ImVec2(x + 2.f, origin.y), IM_COL32(200, 200, 200, 160), mark);
			}

			const ImVec2 mouse = ImGui::GetIO().MousePos;
			const bool hovering = ImGui::IsWindowHovered();
			float y = origin.y + row;
			for (const auto& line : lines) {
				for (const auto& [event, depth] : line.zones) {
					const float x0 = xOf((std::max)(event->begin, s.timelineBegin));
					const float x1 = (std::max)(xOf((std::min)(event->end, s.timelineEnd)), x0 + 1.f);
					const float y0 = y + row * static_cast<float>(depth);
					const float y1 = y0 + row - 1.f;
					const auto& name = shown(s, event->name);
					const auto hue = static_cast<float>(std::hash<std::string>{}(name) % 1000) / 1000.f;
					draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), ImColor::HSV(hue, 0.45f, 0.75f));
					if (x1 - x0 > font * 1.5f) {
						draw->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
						draw->AddText(ImVec2(x0 + 2.f, y0 + 1.f), IM_COL32(0, 0, 0, 255), name.c_str());
						draw->PopClipRect();
					}
					if (hovering && mouse.x >= x0 && mouse.x < x1 && mouse.y >= y0 && mouse.y < y1) {
						ImGui::SetTooltip("%s\n%.3f ms", name.c_str(), toMs(event->end - event->begin));
					}
				}
				y += row * static_cast<float>(line.depth + 1) + gap;
			}
			// The threads' names, kept at the left however far the zones are scrolled.
			const float left = ImGui::GetWindowPos().x + 1.f;
			y = origin.y + row;
			for (const auto& line : lines) {
				const float lineHeight = row * static_cast<float>(line.depth + 1);
				draw->AddRectFilled(ImVec2(left, y), ImVec2(left + labelWidth - 4.f, y + lineHeight), ImGui::GetColorU32(ImGuiCol_WindowBg));
				draw->AddText(ImVec2(left + 4.f, y + 1.f), ImGui::GetColorU32(ImGuiCol_Text), line.thread->label);
				y += lineHeight + gap;
			}
			ImGui::Dummy(ImVec2(labelWidth + width, height));
			ImGui::EndChild();
		}

		// The zones, `most` rows in sight and the rest scrolled to; the numbers' columns as wide as they are.
		void drawZones(const std::vector<Row>& cpu, size_t most) {
			const float height = ImGui::GetTextLineHeightWithSpacing() * (static_cast<float>((std::min)(cpu.size(), most)) + 1.5f);
			if (!ImGui::BeginTable("cpu", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY
				| ImGuiTableFlags_Resizable, ImVec2(0.f, height))) {
				return;
			}
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("zone", ImGuiTableColumnFlags_WidthStretch);
			for (const char* column : { "thread", "own ms", "mean ms", "worst ms", "calls" }) {
				ImGui::TableSetupColumn(column, ImGuiTableColumnFlags_WidthFixed);
			}
			ImGui::TableHeadersRow();
			for (const auto& row : cpu) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(row.stat->name.c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(row.stat->group.c_str());
				ImGui::TableNextColumn();
				ImGui::Text("%.3f", row.own);
				ImGui::TableNextColumn();
				ImGui::Text("%.3f", row.mean);
				ImGui::TableNextColumn();
				ImGui::Text("%.3f", row.worst);
				ImGui::TableNextColumn();
				ImGui::Text("%.1f", row.calls);
			}
			ImGui::EndTable();
		}

		// The passes on the GPU, each opened to the pipelines drawn in it where it was marked by pipeline.
		void drawPasses(const std::vector<Row>& gpu, size_t most) {
			const float height = ImGui::GetTextLineHeightWithSpacing() * (static_cast<float>((std::min)(gpu.size(), most)) + 1.5f);
			if (!ImGui::BeginTable("gpu", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY
				| ImGuiTableFlags_Resizable, ImVec2(0.f, height))) {
				return;
			}
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("pass, and the pipelines drawn in it", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("mean ms", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("worst ms", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableHeadersRow();
			const auto times = [](const Row& row) {
				ImGui::TableNextColumn();
				ImGui::Text("%.3f", row.mean);
				ImGui::TableNextColumn();
				ImGui::Text("%.3f", row.worst);
			};
			for (const auto& pass : gpu) {
				if (pass.stat->group != kGpuGroup) {
					continue;
				}
				const auto parts = partsOf(gpu, pass);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				const bool open = ImGui::TreeNodeEx(pass.stat->name.c_str(), ImGuiTreeNodeFlags_SpanFullWidth
					| (parts.empty() ? ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen : ImGuiTreeNodeFlags_None));
				times(pass);
				if (!open || parts.empty()) {
					continue;
				}
				for (const auto* part : parts) {
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TreeNodeEx(part->stat->name.c_str(), ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_Leaf
						| ImGuiTreeNodeFlags_NoTreePushOnOpen);
					times(*part);
				}
				ImGui::TreePop();
			}
			ImGui::EndTable();
		}
	}

	int64_t now() {
		return Clock::now().time_since_epoch().count();
	}

	void zone(const char* name, int64_t begin, int64_t end) {
		if (gOn.load(std::memory_order_relaxed)) {
			record(ownLane(), { name, begin, end });
		}
	}

	void nameThread(const char* group, int index) {
		tGroup = group;
		tIndex = index;
		if (tLane) {
			auto& kept = lanes();
			std::lock_guard lock(kept.mutex);
			label(*tLane);
		}
	}

	const char* intern(const std::string& name) {
		struct Names {
			std::mutex mutex;
			std::unordered_set<std::string> kept; // a node each: the text stays where it is
		};
		static auto* names = new Names;
		std::lock_guard lock(names->mutex);
		return names->kept.insert(name).first->c_str();
	}

	void frame() {
		auto& s = state();
		const int64_t began = now();
		const int64_t frameBegin = std::exchange(s.frameBegin, began);
		const bool counting = frameBegin != 0 && !s.paused;
		const bool showing = counting && s.open;
		if (showing) {
			s.timeline.clear();
			s.timelineBegin = frameBegin;
			s.timelineEnd = began;
		}
		int zones = 0;
		int64_t mainZoned = 0;
		{
			auto& kept = lanes();
			std::lock_guard lock(kept.mutex);
			for (auto* lane : kept.all) {
				s.dropped += lane->dropped.exchange(0, std::memory_order_relaxed);
				const uint32_t written = lane->written.load(std::memory_order_acquire);
				uint32_t taken = lane->taken.load(std::memory_order_relaxed);
				if (taken == written) {
					continue;
				}
				// Each zone's own time, what the zones inside it leave: a zone is written as it closes, so the ones inside
				// it come before it, and are those still waiting for an outer zone that began no later than they did.
				s.nesting.clear();
				for (; taken != written; ++taken) {
					const Event event = lane->events[taken % Lane::kSize];
					int64_t inside = 0;
					while (!s.nesting.empty() && s.nesting.back().begin >= event.begin) {
						inside += s.nesting.back().end - s.nesting.back().begin;
						s.nesting.pop_back();
					}
					s.nesting.push_back(event);
					if (counting) {
						auto& stat = statFor(s, event.name, lane->group);
						stat.frameTotal += toMs(event.end - event.begin);
						stat.frameOwn += toMs(event.end - event.begin - inside);
						++stat.frameCalls;
					}
					if (showing) {
						s.timeline.push_back({ lane->label, lane->group, lane->index, lane->main, event });
					}
					++zones;
				}
				lane->taken.store(written, std::memory_order_release);
				if (lane->main) {
					for (const auto& outer : s.nesting) {
						mainZoned += outer.end - outer.begin;
					}
				}
			}
		}
		if (counting) {
			// The GPU's side as the render system read it back, on Vulkan two frames late, on OpenGL several: each pass,
			// and each pipeline drawn in it where it was marked by pipeline.
			const auto* info = SystemsModule::RenderDebugInfo::instance();
			for (const auto& time : info->gpuTimes) {
				auto& stat = time.shader ? statFor(s, time.shader, time.pass, true) : statFor(s, time.pass, kGpuGroup, true);
				stat.frameTotal += time.ms;
				stat.frameOwn += time.ms;
				++stat.frameCalls;
			}
			const int slot = static_cast<int>(s.counted % kKept);
			s.frames[slot] = { frameBegin, began, info->gpuMs, static_cast<float>(toMs(mainZoned)), zones };
			for (auto& stat : s.stats) {
				stat->total[slot] = static_cast<float>(std::exchange(stat->frameTotal, 0.0));
				stat->own[slot] = static_cast<float>(std::exchange(stat->frameOwn, 0.0));
				stat->calls[slot] = static_cast<uint16_t>((std::min)(std::exchange(stat->frameCalls, 0), 0xffff));
			}
			++s.counted;
		}
		zone("Profiler", began, now());
	}

	void showWindow(bool shown) {
		state().open = shown;
	}

	void drawWindow() {
		auto& s = state();
		if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
			s.open = !s.open;
		}
		if (!s.open) {
			return;
		}
		// The first time at the right edge, off the Graphics window at the left, and as tall as the screen.
		const auto& io = ImGui::GetIO();
		const float width = ImGui::GetFontSize() * 46.f;
		ImGui::SetNextWindowPos(ImVec2((std::max)(io.DisplaySize.x - width - 20.f, 0.f), 20.f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(width, (std::max)(io.DisplaySize.y - 40.f, 300.f)), ImGuiCond_FirstUseEver);
		if (ImGui::Begin("Profiler (F2)", &s.open)) {
			bool on = gOn.load(std::memory_order_relaxed);
			if (ImGui::Checkbox("Record", &on)) {
				gOn.store(on, std::memory_order_relaxed);
			}
			ImGui::SameLine();
			ImGui::Checkbox("Pause", &s.paused);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.f);
			ImGui::SliderInt("frames", &s.averaged, 10, kKept);
			ImGui::SameLine();
			if (ImGui::Button("Save")) {
				const auto path = savedPath();
				s.saved = writeSummary(path, s.averaged) ? "Saved to " + std::filesystem::absolute(path).string() : "Could not write " + path;
			}
			if (!s.saved.empty()) {
				ImGui::TextDisabled("%s", s.saved.c_str());
			}

			const int n = framesIn(s, s.averaged);
			const auto cpu = rows(s, n, false);
			const auto gpu = rows(s, n, true);
			const auto o = overview(s, n, cpu);
			ImGui::Text("Frame %.2f ms, worst %.2f (%.0f fps)", o.frameMs, o.worstMs, o.frameMs > 0.0 ? 1000.0 / o.frameMs : 0.0);
			ImGui::Text("GPU %.2f ms, worst %.2f, busy %.0f%% of the frame", o.gpuMs, o.gpuWorstMs, o.gpuBusy * 100.0);
			ImGui::Text("Main thread %.2f ms, busy %.0f%% of the frame", o.workMs, o.cpuBusy * 100.0);
			ImGui::Text("Main thread waits: GPU %.2f ms, present %.2f, fps limit %.2f", o.waitGpuMs, o.presentMs, o.idleMs);
			ImGui::TextColored(o.bound == kByGpu ? ImVec4(1.f, 0.6f, 0.3f, 1.f) : o.bound == kByCpu ? ImVec4(0.4f, 0.8f, 1.f, 1.f)
				: ImVec4(0.6f, 0.9f, 0.5f, 1.f), "Held back by the %s", o.bound);
			ImGui::TextDisabled("Profiler: %.0f zones a frame, %.3f ms a frame (%.2f%%), %llu lost", o.zones, o.costMs,
				o.frameMs > 0.0 ? o.costMs / o.frameMs * 100.0 : 0.0, static_cast<unsigned long long>(s.dropped));

			if (ImGui::CollapsingHeader("CPU", ImGuiTreeNodeFlags_DefaultOpen)) {
				drawZones(cpu, 14);
			}
			if (ImGui::CollapsingHeader("GPU", ImGuiTreeNodeFlags_DefaultOpen)) {
				// By pipeline too: a timestamp at each change of one (RenderHardwareInterface::gpuShaderMarks).
				if (auto* rhi = Engine::instance()->rhi()) {
					bool byShader = rhi->gpuShaderMarks.load(std::memory_order_relaxed);
					if (ImGui::Checkbox("by shader", &byShader)) {
						rhi->gpuShaderMarks.store(byShader, std::memory_order_relaxed);
					}
				}
				drawPasses(gpu, 14);
			}
			if (ImGui::CollapsingHeader("Threads")) {
				for (const auto& group : busy(cpu)) {
					ImGui::Text("%-10s %8.3f ms on %d threads", group.group.c_str(), group.ms, group.threads);
				}
			}
			if (ImGui::CollapsingHeader("Timeline", ImGuiTreeNodeFlags_DefaultOpen)) {
				drawTimeline(s);
			}
		}
		ImGui::End();
	}

	Load load() {
		auto& s = state();
		// The last second's frames, recounted every so many: the shares change no faster, and a frame has many zones.
		constexpr int kFrames = 60;
		constexpr int64_t kEvery = 15;
		static Load kept;
		static int64_t keptAt = -kEvery;
		if (s.counted - keptAt >= kEvery) {
			keptAt = s.counted;
			const int n = framesIn(s, kFrames);
			const auto o = overview(s, n, rows(s, n, false));
			kept = { static_cast<float>(o.gpuBusy), static_cast<float>(o.cpuBusy), static_cast<float>(o.gpuMs), static_cast<float>(o.workMs) };
		}
		return kept;
	}

	std::string summaryJson(int frames) {
		auto& s = state();
		const int n = framesIn(s, frames);
		const auto cpu = rows(s, n, false);
		const auto gpu = rows(s, n, true);
		const auto o = overview(s, n, cpu);
		Json::Value out;
		out["frames"] = n;
		out["seconds"] = o.seconds;
		out["frameMs"]["mean"] = o.frameMs;
		out["frameMs"]["worst"] = o.worstMs;
		out["fps"] = o.frameMs > 0.0 ? 1000.0 / o.frameMs : 0.0;
		out["gpuMs"]["mean"] = o.gpuMs;
		out["gpuMs"]["worst"] = o.gpuWorstMs;
		out["gpuMs"]["busyPercent"] = o.gpuBusy * 100.0;
		auto& main = out["main"];
		main["workMs"] = o.workMs;
		main["busyPercent"] = o.cpuBusy * 100.0;
		main["waitGpuMs"] = o.waitGpuMs;
		main["idleMs"] = o.idleMs;
		main["presentMs"] = o.presentMs;
		main["unzonedMs"] = o.unzonedMs;
		out["bound"] = o.bound;
		auto& zones = out["cpu"] = Json::Value(Json::arrayValue);
		for (const auto& row : cpu) {
			Json::Value entry;
			entry["zone"] = row.stat->name;
			entry["thread"] = row.stat->group;
			entry["ownMs"] = row.own;
			entry["meanMs"] = row.mean;
			entry["worstMs"] = row.worst;
			entry["calls"] = row.calls;
			zones.append(entry);
		}
		auto& passes = out["gpu"] = Json::Value(Json::arrayValue);
		for (const auto& row : gpu) {
			if (row.stat->group != kGpuGroup) {
				continue;
			}
			Json::Value entry;
			entry["pass"] = row.stat->name;
			entry["meanMs"] = row.mean;
			entry["worstMs"] = row.worst;
			for (const auto* part : partsOf(gpu, row)) {
				Json::Value shader;
				shader["shader"] = part->stat->name;
				shader["meanMs"] = part->mean;
				shader["worstMs"] = part->worst;
				entry["shaders"].append(shader);
			}
			passes.append(entry);
		}
		auto& threads = out["threads"] = Json::Value(Json::arrayValue);
		for (const auto& group : busy(cpu)) {
			Json::Value entry;
			entry["group"] = group.group;
			entry["threads"] = group.threads;
			entry["busyMs"] = group.ms;
			threads.append(entry);
		}
		auto& cost = out["profiler"];
		cost["zonesPerFrame"] = o.zones;
		cost["zoneNs"] = zoneNs();
		cost["drainMs"] = o.drainMs;
		cost["costMs"] = o.costMs;
		cost["costPercent"] = o.frameMs > 0.0 ? o.costMs / o.frameMs * 100.0 : 0.0;
		cost["lost"] = static_cast<Json::UInt64>(s.dropped);
		Json::StreamWriterBuilder writer;
		writer["indentation"] = "";
		// To the thousandth: a millisecond's digits past the microsecond are only the float's.
		writer["precision"] = 3;
		writer["precisionType"] = "decimal";
		return Json::writeString(writer, out);
	}

	bool writeSummary(const std::string& path, int frames) {
		const std::filesystem::path file(path);
		if (file.has_parent_path()) {
			std::error_code error;
			std::filesystem::create_directories(file.parent_path(), error);
		}
		std::ofstream out(file, std::ios::binary);
		out << summaryText(frames);
		return static_cast<bool>(out);
	}
}
