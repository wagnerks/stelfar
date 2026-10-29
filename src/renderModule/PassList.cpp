#include "renderModule/PassList.h"

#include <algorithm>
#include <cstddef>
#include <iterator>

namespace SFE::Render {
	namespace {
		using Entries = std::vector<PassEntry>;

		// Where the entry of that name is; entries.size() for none.
		size_t indexOf(const Entries& entries, std::string_view name) {
			const auto found = std::find_if(entries.begin(), entries.end(),
				[&](const PassEntry& entry) { return entry.name == name; });
			return static_cast<size_t>(found - entries.begin());
		}

		// What errors() keeps: the operation, the name it was given, and why it was not done, e.g.
		// `insertAfter "Geomtry": no pass of that name`.
		std::string refused(const char* operation, std::string_view name, std::string_view why) {
			return std::string(operation) + " \"" + std::string(name) + "\": " + std::string(why);
		}

		// Why `entry` cannot be one of `entries` (empty: it can). `leaving`: the index of the one it stands in for, whose
		// name it may take.
		std::string unfit(const Entries& entries, const PassEntry& entry, size_t leaving = static_cast<size_t>(-1)) {
			if (entry.name.empty()) {
				return "an entry with no name";
			}
			if (!entry.make) {
				return "no way to make it";
			}
			const size_t taken = indexOf(entries, entry.name);
			if (taken != entries.size() && taken != leaving) {
				return "already in the list";
			}
			return {};
		}

		// `run` into `entries` before index `at`, in its order -- checked whole first, so either all of it goes in or none
		// does and why is said.
		bool insertRun(Entries& entries, std::vector<std::string>& errors, const char* operation, size_t at, Entries run) {
			for (size_t i = 0; i < run.size(); ++i) {
				std::string why = unfit(entries, run[i]);
				if (why.empty() && std::any_of(run.begin(), run.begin() + static_cast<std::ptrdiff_t>(i),
					[&](const PassEntry& before) { return before.name == run[i].name; })) {
					why = "twice in the run";
				}
				if (!why.empty()) {
					errors.push_back(refused(operation, run[i].name, why));
					return false;
				}
			}
			entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(at), std::make_move_iterator(run.begin()),
				std::make_move_iterator(run.end()));
			return true;
		}

		Entries single(PassEntry entry) {
			Entries run;
			run.push_back(std::move(entry));
			return run;
		}
	}

	bool PassList::append(PassEntry entry) {
		return insertRun(mEntries, mErrors, "append", mEntries.size(), single(std::move(entry)));
	}

	bool PassList::insertAfter(std::string_view anchor, PassEntry entry) {
		return insertAfter(anchor, single(std::move(entry)));
	}

	bool PassList::insertAfter(std::string_view anchor, std::vector<PassEntry> run) {
		const size_t at = indexOf(mEntries, anchor);
		if (at == mEntries.size()) {
			mErrors.push_back(refused("insertAfter", anchor, "no pass of that name"));
			return false;
		}
		return insertRun(mEntries, mErrors, "insertAfter", at + 1, std::move(run));
	}

	bool PassList::insertBefore(std::string_view anchor, PassEntry entry) {
		return insertBefore(anchor, single(std::move(entry)));
	}

	bool PassList::insertBefore(std::string_view anchor, std::vector<PassEntry> run) {
		const size_t at = indexOf(mEntries, anchor);
		if (at == mEntries.size()) {
			mErrors.push_back(refused("insertBefore", anchor, "no pass of that name"));
			return false;
		}
		return insertRun(mEntries, mErrors, "insertBefore", at, std::move(run));
	}

	bool PassList::replace(std::string_view name, PassEntry entry) {
		const size_t at = indexOf(mEntries, name);
		if (at == mEntries.size()) {
			mErrors.push_back(refused("replace", name, "no pass of that name"));
			return false;
		}
		if (const size_t taken = indexOf(mEntries, entry.name); taken != mEntries.size() && taken != at) {
			mErrors.push_back(refused("replace", name, "\"" + entry.name + "\" is another pass's name"));
			return false;
		}
		if (const auto why = unfit(mEntries, entry, at); !why.empty()) {
			mErrors.push_back(refused("replace", name, why));
			return false;
		}
		mEntries[at] = std::move(entry);
		return true;
	}

	bool PassList::disable(std::string_view name) {
		const size_t at = indexOf(mEntries, name);
		if (at == mEntries.size()) {
			mErrors.push_back(refused("disable", name, "no pass of that name"));
			return false;
		}
		mEntries[at].enabled = false;
		return true;
	}

	bool PassList::enable(std::string_view name) {
		const size_t at = indexOf(mEntries, name);
		if (at == mEntries.size()) {
			mErrors.push_back(refused("enable", name, "no pass of that name"));
			return false;
		}
		mEntries[at].enabled = true;
		return true;
	}

	bool PassList::remove(std::string_view name) {
		const size_t at = indexOf(mEntries, name);
		if (at == mEntries.size()) {
			mErrors.push_back(refused("remove", name, "no pass of that name"));
			return false;
		}
		mEntries.erase(mEntries.begin() + static_cast<std::ptrdiff_t>(at));
		return true;
	}

	const PassEntry* PassList::find(std::string_view name) const {
		const size_t at = indexOf(mEntries, name);
		return at == mEntries.size() ? nullptr : &mEntries[at];
	}

	std::vector<std::string> PassList::errors() const {
		auto errors = mErrors;
		std::vector<std::pair<std::string, const PassEntry*>> given; // each key given so far, and by which entry
		const auto giver = [&](const std::string& key) {
			return std::find_if(given.begin(), given.end(), [&](const auto& g) { return g.first == key; });
		};
		for (const auto& entry : mEntries) {
			if (!entry.enabled) {
				continue;
			}
			for (const auto& key : entry.needs) {
				if (giver(key) == given.end()) {
					errors.push_back(refused("needs", entry.name, "\"" + key + "\" is given by no pass before it"));
				}
			}
			for (const auto& key : entry.gives) {
				if (const auto by = giver(key); by != given.end()) {
					errors.push_back(refused("gives", entry.name, "\"" + key + "\" is given by \"" + by->second->name + "\" too"));
					continue;
				}
				given.emplace_back(key, &entry);
			}
		}
		return errors;
	}

	std::vector<std::string> PassList::names() const {
		std::vector<std::string> enabled;
		for (const auto& entry : mEntries) {
			if (entry.enabled) {
				enabled.push_back(entry.name);
			}
		}
		return enabled;
	}
}
