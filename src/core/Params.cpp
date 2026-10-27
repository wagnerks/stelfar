#include "Params.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include <imgui.h>
#include <json/value.h>

#include "core/FileSystem.h"
#include "logsModule/logger.h"

namespace SFE::Params {
	namespace {
		constexpr const char* kFile = "tweaks.json";

		// A number: where it is kept -- here (own), or where its owner keeps it (bind) -- what the code starts it at, and what
		// it is for. The live value, through `at`, is always the one in force: the code reads it, and may write it, directly.
		struct Entry {
			Type type = Type::Float;
			void* at = nullptr;
			float ownFloat = 0.f;
			int ownInt = 0;
			bool ownBool = false;
			double initial = 0.0;
			// `bindable`: it came from a bind, so a later bind may give it storage again; `bound`: that storage is there now.
			bool bindable = false;
			bool bound = false;
			Meta meta;   // its help is kept in `help`, a copy
			std::string help;
			bool hasHelp = false;
			// A session holds it at a value for this run: one from the command line (`run`, kept under one from the bridge),
			// one from the bridge, or both. Meanwhile `kept` is the value the number has besides -- what save writes, and what
			// the session falls back to when it goes -- and `held` what the session set the live value to. Anything else that
			// writes the live value (a drag in a panel, the code itself) ends the session: what it wrote is the number's own.
			bool hasRun = false;
			double run = 0.0;
			bool hasBridge = false;
			double held = 0.0;
			double kept = 0.0;
		};
		// By group, then by name. A map's entries stay where they are and none is ever taken out, so the references handed
		// out stay good.
		std::map<std::string, std::map<std::string, Entry>>& entries() {
			static std::map<std::string, std::map<std::string, Entry>> all;
			return all;
		}
		// Values set for the session before anything registered their name: from the command line, which is read before
		// the engine starts, while numbers of a function's own come into being on its first call.
		struct Waiting {
			std::optional<double> run;
			std::optional<double> bridge;
		};
		std::map<std::string, std::map<std::string, Waiting>>& waitingSets() {
			static std::map<std::string, std::map<std::string, Waiting>> all;
			return all;
		}
		// What the file held, for the numbers that come into being after it was read: a function's own, on its first
		// call. Not so much as made before then: most numbers are registered before main, and jsoncpp is a library
		// loaded late, once main has said where it is.
		bool gLoaded = false;
		Json::Value& saved() {
			static Json::Value root;
			return root;
		}
		// A function's own numbers come into being on its first call, on whatever thread that is.
		std::mutex& guard() {
			static std::mutex mutex;
			return mutex;
		}

		void* own(Entry& entry) {
			switch (entry.type) {
			case Type::Int: return &entry.ownInt;
			case Type::Bool: return &entry.ownBool;
			default: return &entry.ownFloat;
			}
		}

		double get(const Entry& entry) {
			switch (entry.type) {
			case Type::Int: return *static_cast<const int*>(entry.at);
			case Type::Bool: return *static_cast<const bool*>(entry.at) ? 1.0 : 0.0;
			default: return *static_cast<const float*>(entry.at);
			}
		}

		// Whether a number of the type can be the value at all: anything finite in its range (an int's after rounding).
		bool fits(Type type, double value) {
			if (!std::isfinite(value)) {
				return false;
			}
			switch (type) {
			case Type::Int: {
				const double rounded = std::round(value);
				return rounded >= static_cast<double>((std::numeric_limits<int>::min)())
					&& rounded <= static_cast<double>((std::numeric_limits<int>::max)());
			}
			case Type::Bool: return true;
			default: return std::abs(value) <= static_cast<double>((std::numeric_limits<float>::max)());
			}
		}

		// A value written as the type has it: an int rounded, a bool whether it is anything but nothing. Only a value that
		// fits comes here.
		void put(void* at, Type type, double value) {
			switch (type) {
			case Type::Int: *static_cast<int*>(at) = static_cast<int>(std::round(value)); break;
			case Type::Bool: *static_cast<bool*>(at) = value != 0.0; break;
			default: *static_cast<float*>(at) = static_cast<float>(value); break;
			}
		}

		void put(Entry& entry, double value) {
			put(entry.at, entry.type, value);
		}

		Json::Value toJson(Type type, double value) {
			switch (type) {
			case Type::Int: return Json::Value(static_cast<int>(value));
			case Type::Bool: return Json::Value(value != 0.0);
			default: return Json::Value(value);
			}
		}

		bool inSession(const Entry& entry) {
			return entry.hasRun || entry.hasBridge;
		}

		// A session holds only while the number has the value it set: whatever wrote it since -- a drag, a click, a reset,
		// the code, another panel over the same storage -- ended it, and what it wrote is the number's own now.
		void settle(Entry& entry) {
			if (inSession(entry) && get(entry) != entry.held) {
				entry.hasRun = false;
				entry.hasBridge = false;
			}
		}

		void hold(Entry& entry, double value, Session session) {
			settle(entry);
			if (!inSession(entry)) {
				entry.kept = get(entry);
			}
			put(entry, value);
			entry.held = get(entry);
			if (session == Session::Run) {
				entry.hasRun = true;
				entry.run = entry.held;
			}
			else {
				entry.hasBridge = true;
			}
		}

		// What the file held for one number, if anything it can be, without adding to what it held: a number or true and
		// false, as the number's type has it.
		std::optional<double> savedFor(const std::string& group, const std::string& name, Type type) {
			if (!gLoaded) {
				return std::nullopt;
			}
			const Json::Value& root = saved();
			if (!root.isObject() || !root.isMember(group) || !root[group].isObject() || !root[group].isMember(name)) {
				return std::nullopt;
			}
			const Json::Value& found = root[group][name];
			if (!found.isNumeric() && !found.isBool()) {
				return std::nullopt;
			}
			const double value = found.asDouble();
			return fits(type, value) ? std::optional<double>(value) : std::nullopt;
		}

		// Called with the guard held.
		Entry* find(const std::string& group, const std::string& name) {
			const auto inGroup = entries().find(group);
			if (inGroup == entries().end()) {
				return nullptr;
			}
			const auto found = inGroup->second.find(name);
			return found == inGroup->second.end() ? nullptr : &found->second;
		}

		// A value set before its name was registered, handed to the registration and no longer waiting.
		std::optional<Waiting> takeWaiting(const std::string& group, const std::string& name) {
			auto& all = waitingSets();
			const auto inGroup = all.find(group);
			if (inGroup == all.end()) {
				return std::nullopt;
			}
			const auto found = inGroup->second.find(name);
			if (found == inGroup->second.end()) {
				return std::nullopt;
			}
			const Waiting taken = found->second;
			inGroup->second.erase(found);
			if (inGroup->second.empty()) {
				all.erase(inGroup);
			}
			return taken;
		}

		// The entry for a number, new or already there; two of one name that start differently (locals of different
		// functions, say) are kept apart. A new one takes the file's value, then whatever the session set for it before.
		// `storage` is the owner's, for a bind; null for a number kept here. Called with the guard held.
		Entry& entry(const char* group, const char* name, Type type, double initial, void* storage, const Meta& meta) {
			auto& inGroup = entries()[group];
			std::string key = name;
			for (int n = 2;; ++n) {
				const auto found = inGroup.find(key);
				if (found == inGroup.end() || (found->second.type == type && found->second.initial == initial)) {
					break;
				}
				key = std::string(name) + " #" + std::to_string(n);
			}
			auto [it, made] = inGroup.try_emplace(key);
			Entry& number = it->second;
			if (made) {
				number.type = type;
				number.initial = initial;
				number.meta = meta;
				number.meta.help = nullptr;
				put(own(number), type, initial);
				number.at = storage ? storage : own(number);
				number.bindable = storage != nullptr;
				number.bound = storage != nullptr;
				if (!meta.transient) {
					if (const auto found = savedFor(group, key, type)) {
						put(number, *found);
					}
				}
				if (const auto pending = takeWaiting(group, key)) {
					if (pending->run && fits(type, *pending->run)) {
						hold(number, *pending->run, Session::Run);
					}
					if (pending->bridge && fits(type, *pending->bridge)) {
						hold(number, *pending->bridge, Session::UntilReload);
					}
				}
			}
			else if (storage && number.bindable && !number.bound) {
				// Bound again once its storage went away: the new storage takes the value the number kept meanwhile.
				put(storage, type, get(number));
				number.at = storage;
				number.bound = true;
			}
			if (!number.hasHelp && meta.help) {
				number.help = meta.help;
				number.hasHelp = true;
			}
			return number;
		}

		bool inside(const void* at, const void* begin, const void* end) {
			const std::less<const void*> before{};
			return !before(at, begin) && before(at, end);
		}
	}

	float& value(const char* group, const char* name, float initial, const Meta& meta) {
		std::lock_guard lock(guard());
		return *static_cast<float*>(entry(group, name, Type::Float, initial, nullptr, meta).at);
	}

	int& value(const char* group, const char* name, int initial, const Meta& meta) {
		std::lock_guard lock(guard());
		return *static_cast<int*>(entry(group, name, Type::Int, initial, nullptr, meta).at);
	}

	bool& value(const char* group, const char* name, bool initial, const Meta& meta) {
		std::lock_guard lock(guard());
		return *static_cast<bool*>(entry(group, name, Type::Bool, initial ? 1.0 : 0.0, nullptr, meta).at);
	}

	void bind(const char* group, const char* name, float& storage, const Meta& meta) {
		std::lock_guard lock(guard());
		entry(group, name, Type::Float, storage, &storage, meta);
	}

	void bind(const char* group, const char* name, int& storage, const Meta& meta) {
		std::lock_guard lock(guard());
		entry(group, name, Type::Int, storage, &storage, meta);
	}

	void bind(const char* group, const char* name, bool& storage, const Meta& meta) {
		std::lock_guard lock(guard());
		entry(group, name, Type::Bool, storage ? 1.0 : 0.0, &storage, meta);
	}

	void unbind(const void* begin, const void* end) {
		std::lock_guard lock(guard());
		for (auto& [group, inGroup] : entries()) {
			for (auto& [name, entry] : inGroup) {
				if (entry.bound && inside(entry.at, begin, end)) {
					put(own(entry), entry.type, get(entry));
					entry.at = own(entry);
					entry.bound = false;
				}
			}
		}
	}

	void reset(const void* begin, const void* end) {
		std::lock_guard lock(guard());
		for (auto& [group, inGroup] : entries()) {
			for (auto& [name, entry] : inGroup) {
				if (!entry.bound || !inside(entry.at, begin, end)) {
					continue;
				}
				settle(entry);
				if (inSession(entry)) {
					entry.kept = entry.initial;
				}
				else {
					put(entry, entry.initial);
				}
			}
		}
	}

	bool setSession(const std::string& group, const std::string& name, double value, Session session) {
		std::lock_guard lock(guard());
		if (auto* found = find(group, name)) {
			if (!fits(found->type, value)) {
				return false;
			}
			hold(*found, value, session);
			return true;
		}
		auto& pending = waitingSets()[group][name];
		(session == Session::Run ? pending.run : pending.bridge) = value;
		return true;
	}

	bool see(const std::string& group, const std::string& name, Seen& out) {
		std::lock_guard lock(guard());
		auto* found = find(group, name);
		if (!found) {
			return false;
		}
		settle(*found);
		out = { found->type, get(*found), inSession(*found) };
		return true;
	}

	float* findFloat(const std::string& group, const std::string& name) {
		std::lock_guard lock(guard());
		auto* found = find(group, name);
		return found && found->type == Type::Float ? static_cast<float*>(found->at) : nullptr;
	}

	std::string waiting() {
		std::lock_guard lock(guard());
		std::string names;
		for (const auto& [group, inGroup] : waitingSets()) {
			for (const auto& [name, set] : inGroup) {
				names += (names.empty() ? "" : ", ") + group + " / " + name;
			}
		}
		return names;
	}

	bool load() {
		// Read whole before anything changes: a file that is not there, or does not parse, leaves every number as it was.
		// None is no news (an application that tunes nothing has none), so it is looked for first rather than missed.
		if (!FileSystem::isFileExists(kFile)) {
			return false;
		}
		Json::Value read;
		if (!FileSystem::readJson(kFile, read) || !read.isObject()) {
			return false;
		}
		std::lock_guard lock(guard());
		saved() = std::move(read);
		gLoaded = true;
		// What the bridge set for the session goes, waiting or not; what the command line set stays.
		auto& all = waitingSets();
		for (auto group = all.begin(); group != all.end();) {
			for (auto name = group->second.begin(); name != group->second.end();) {
				name->second.bridge.reset();
				name = name->second.run ? std::next(name) : group->second.erase(name);
			}
			group = group->second.empty() ? all.erase(group) : std::next(group);
		}
		for (auto& [group, inGroup] : entries()) {
			for (auto& [name, entry] : inGroup) {
				settle(entry);
				// What the number has besides a session: the file's, or the code's where the file has none. One of this run
				// only has nothing in the file: what it had before the session, or has now.
				double standing = 0.0;
				if (entry.meta.transient) {
					standing = inSession(entry) ? entry.kept : get(entry);
				}
				else {
					standing = savedFor(group, name, entry.type).value_or(entry.initial);
				}
				entry.hasBridge = false;
				if (entry.hasRun) {
					entry.kept = standing;
					put(entry, entry.run);
					entry.held = get(entry);
				}
				else {
					put(entry, standing);
				}
			}
		}
		return true;
	}

	void save() {
		// Into the file as it is: what it holds for a number nothing registered this run (a function's own, not called
		// yet) stays. A file that does not parse is left alone rather than written over with what is known here.
		Json::Value root(Json::objectValue);
		if (FileSystem::isFileExists(kFile) && (!FileSystem::readJson(kFile, root) || !root.isObject())) {
			LogsModule::Logger::LOG_WARNING("%s is not groups of numbers as it stands: not saved over", kFile);
			return;
		}
		{
			std::lock_guard lock(guard());
			for (auto& [group, inGroup] : entries()) {
				for (auto& [name, entry] : inGroup) {
					settle(entry);
					const double value = inSession(entry) ? entry.kept : get(entry);
					if (!entry.meta.transient && value != entry.initial) {
						if (!root[group].isObject()) {
							root[group] = Json::Value(Json::objectValue);
						}
						root[group][name] = toJson(entry.type, value);
					}
					else if (root.isMember(group) && root[group].isObject()) {
						root[group].removeMember(name);
					}
				}
			}
		}
		for (const auto& group : root.getMemberNames()) {
			if (root[group].isObject() && root[group].empty()) {
				root.removeMember(group);
			}
		}
		FileSystem::writeJson(kFile, root);
	}

	void panel() {
		static char filter[64] = "";
		if (ImGui::Button("save")) {
			save();
		}
		ImGui::SetItemTooltip("Keep what differs from the code in content/tweaks.json, read back at the next start.");
		ImGui::SameLine();
		if (ImGui::Button("reload")) {
			load();
		}
		ImGui::SetItemTooltip("Every number as content/tweaks.json now has it (edited outside the game), the rest as the code has it.");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(200.f);
		ImGui::InputTextWithHint("##filter", "filter", filter, sizeof(filter));
		const std::string wanted = filter;
		std::lock_guard lock(guard());
		const auto matches = [&](const std::string& text) {
			if (wanted.empty()) {
				return true;
			}
			const auto lower = [](std::string s) {
				for (auto& c : s) {
					c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				}
				return s;
			};
			return lower(text).find(lower(wanted)) != std::string::npos;
		};
		for (auto& [group, inGroup] : entries()) {
			const bool groupMatches = matches(group);
			bool any = false;
			for (const auto& [name, entry] : inGroup) {
				any = any || (entry.meta.panel && (groupMatches || matches(name)));
			}
			if (!any || !ImGui::CollapsingHeader(group.c_str())) {
				continue;
			}
			ImGui::PushID(group.c_str());
			for (auto& [name, entry] : inGroup) {
				if (!entry.meta.panel || (!groupMatches && !matches(name))) {
					continue;
				}
				ImGui::PushID(name.c_str());
				settle(entry);
				const bool held = inSession(entry);
				const bool changed = get(entry) != entry.initial;
				// Held for the session in blue, differing from the code in orange.
				if (held || changed) {
					ImGui::PushStyleColor(ImGuiCol_Text, held ? ImVec4(0.55f, 0.8f, 1.f, 1.f) : ImVec4(1.f, 0.8f, 0.3f, 1.f));
				}
				const bool bounded = entry.meta.min < entry.meta.max;
				switch (entry.type) {
				case Type::Float: {
					// Dragged in steps a hundredth of its size, so a millimetre and a kilometre both move sensibly; one that starts
					// at nothing (an offset) a millimetre.
					const float initial = static_cast<float>(entry.initial);
					const float step = initial != 0.f ? (std::max)(std::abs(initial), 0.001f) * 0.01f : 0.001f;
					ImGui::DragFloat(name.c_str(), static_cast<float*>(entry.at), step,
						bounded ? entry.meta.min : 0.f, bounded ? entry.meta.max : 0.f, "%.4g");
					break;
				}
				case Type::Int: {
					// A hundredth of its size a pixel too, and at least a step every ten pixels.
					const float step = (std::max)(std::abs(static_cast<float>(entry.initial)) * 0.01f, 0.1f);
					ImGui::DragInt(name.c_str(), static_cast<int*>(entry.at), step,
						bounded ? static_cast<int>(entry.meta.min) : 0, bounded ? static_cast<int>(entry.meta.max) : 0);
					break;
				}
				case Type::Bool:
					ImGui::Checkbox(name.c_str(), static_cast<bool*>(entry.at));
					break;
				}
				if ((entry.hasHelp || held) && ImGui::BeginItemTooltip()) {
					ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.f);
					if (entry.hasHelp) {
						ImGui::TextUnformatted(entry.help.c_str());
					}
					if (held) {
						ImGui::TextUnformatted("held for this session; not saved");
					}
					ImGui::PopTextWrapPos();
					ImGui::EndTooltip();
				}
				if (held || changed) {
					ImGui::PopStyleColor();
				}
				if (entry.meta.transient) {
					ImGui::SameLine();
					ImGui::TextDisabled("(this run)");
				}
				if (changed) {
					ImGui::SameLine();
					if (ImGui::SmallButton("reset")) {
						put(entry, entry.initial);
					}
					switch (entry.type) {
					case Type::Int: ImGui::SetItemTooltip("Back to %d, as the code has it.", static_cast<int>(entry.initial)); break;
					case Type::Bool: ImGui::SetItemTooltip("Back to %s, as the code has it.", entry.initial != 0.0 ? "on" : "off"); break;
					default: ImGui::SetItemTooltip("Back to %.4g, as the code has it.", entry.initial); break;
					}
				}
				// A drag, a click or a reset of a number held for the session ends the session: what it was set to here is
				// its own value now, the one saved.
				settle(entry);
				ImGui::PopID();
			}
			ImGui::PopID();
		}
	}
}
