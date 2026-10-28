#pragma once

#include <string>

// The numbers the engine and the application are tuned by, changed while it runs rather than rebuilt for: one registry
// for both, by group and name, one file the application names (setFile) under the content root, and one listing
// (panel(), which the engine shows in its window "Tweaks").
//
// A number becomes one by what it is declared as, and nothing that reads it changes:
//
//     static float& kFadeSeconds = SFE::Params::value("Fade", "seconds", 1.5f);
//
// or, kept where its owner keeps it, by a bind. It starts at the code's value; what differs from the code is kept in that
// file (save) and read back when the engine starts, before any system is made. Registered from any thread, at any time
// -- before main too, so nothing here touches a file, a library or the log before load() is called -- and read through
// the reference with no lock (a number is written whole, and a value changing under a frame is what dragging it means).
namespace SFE::Params {
	// What a number is besides its value. `help` is copied. `min`/`max` bound the panel's widget only (none when
	// min >= max); a value from the file, the bridge or the command line is never clamped. `transient`: this run's
	// only -- never read from the file, never saved (a value whose "unset" means "as the scene has it", a debug view).
	// `panel`: listed by panel().
	struct Meta {
		const char* help = nullptr;
		float min = 0.f;
		float max = 0.f;
		bool transient = false;
		bool panel = true;
	};

	// A number the registry keeps: the reference lasts the program and never moves. `initial` is the code's value;
	// the file's, once read, stands over it. Registering the same group and name with the same initial again gives
	// the same number; with another initial, a new one named "name #2" (#3, ...). The first non-null help is kept.
	float& value(const char* group, const char* name, float initial, const Meta& meta = {});
	int& value(const char* group, const char* name, int initial, const Meta& meta = {});
	bool& value(const char* group, const char* name, bool initial, const Meta& meta = {});

	// A number kept in storage of the caller's: its value now is the code's initial; the file's is written into it at
	// once if the file is read. A second bind of a bound key is ignored.
	void bind(const char* group, const char* name, float& storage, const Meta& meta = {});
	void bind(const char* group, const char* name, int& storage, const Meta& meta = {});
	void bind(const char* group, const char* name, bool& storage, const Meta& meta = {});

	// Storage in [begin, end) going away: each number bound inside it keeps its value in storage of the registry's
	// own until it is bound again (a later bind of the same key takes the new storage and writes the value into it).
	void unbind(const void* begin, const void* end);
	// Every number bound inside [begin, end) back to where it stands without this run's changes -- the file's value as
	// last read or saved, the code's where the file has none or for a transient number (a settings object's "reset
	// defaults"); a number held for the session keeps its session value and has its kept value reset so.
	void reset(const void* begin, const void* end);
	// Whether the number stands where it stands without this run's changes: its value, in its own type, is the file's as
	// last read or saved (the code's where the file has none, or for a transient). False for a name nobody registered.
	// The Tweaks window offers its reset when it does not.
	bool atStanding(const std::string& group, const std::string& name);

	// A value for this run that is never saved. Run: from the command line, survives reload. UntilReload: from the
	// bridge, dropped by load(). A panel drag on the number ends it. Set before the name is registered, it waits
	// for the registration. Returns false only for a value that cannot be the number's type (a registered key).
	enum class Session { Run, UntilReload };
	bool setSession(const std::string& group, const std::string& name, double value, Session session);

	enum class Type { Float, Int, Bool };
	struct Seen {
		Type type;
		double value;
		bool session;
	};
	// The number's type and value now; false if no such name is registered.
	bool see(const std::string& group, const std::string& name, Seen& out);
	// The float number itself, or nullptr when there is none or it is not a float.
	float* findFloat(const std::string& group, const std::string& name);
	// Names set with setSession before anything registered them, still waiting.
	std::string waiting();

	// The application's numbers file, a path as FileSystem takes it (under the content root): what load() reads and
	// save() writes. None (empty, as the engine starts): load() reads nothing, save() writes nothing. What was read
	// before stays where the numbers stand until the next load.
	void setFile(std::string path);
	std::string file();

	// The named file: read it and set every number to the file's value or its initial, keeping it for numbers
	// registered later; session values of Run stay, UntilReload go. False, changing nothing, when no file is named, it
	// is not there or it does not parse (parsed into a temporary first).
	bool load();
	// Writes the numbers that differ from their initial (a session number: its kept value), merged into the named file
	// as it is on disk: a key nothing registered this run keeps its value; transient numbers are never written. Same
	// style as before (jsoncpp's default writer: tabs, sorted keys); floats as doubles, ints as ints, bools as bools;
	// nothing without a file (logged). What it wrote is then where the numbers stand.
	void save();
	// The numbers' listing, its save and reload (the caller opens the window).
	void panel();
}
