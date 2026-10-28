#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace SFE::BinaryCache {
	// What was slow to build and is the same every run for the same inputs, kept in cache/ under the application's root
	// (FileSystem::root).
	// A file is named by a hash of everything it was built from, so changed inputs just miss; a change to the code that
	// builds it needs that code's own version bumped into the hash.

	uint64_t hash(std::string_view text, uint64_t seed = 14695981039346656037ull);
	// cache/<name>_<key hex>.bin under FileSystem::root(), never looked for behind it.
	std::string path(std::string_view name, uint64_t key);

	class Writer {
	public:
		// Writes to a temporary file; commit() moves it into place, so a run killed halfway leaves no torn cache.
		explicit Writer(std::string path);
		~Writer();

		template<class T> requires std::is_trivially_copyable_v<T>
		void value(const T& v) { mOut.write(static_cast<const char*>(static_cast<const void*>(&v)), sizeof(T)); }
		template<class T> requires std::is_trivially_copyable_v<T>
		void vector(const std::vector<T>& v) {
			value(static_cast<uint64_t>(v.size()));
			mOut.write(static_cast<const char*>(static_cast<const void*>(v.data())), static_cast<std::streamsize>(v.size() * sizeof(T)));
		}
		void text(std::string_view s) {
			value(static_cast<uint64_t>(s.size()));
			mOut.write(s.data(), static_cast<std::streamsize>(s.size()));
		}
		bool commit();

	private:
		std::string mPath;
		std::string mTemp;
		std::ofstream mOut;
		bool mCommitted = false;
	};

	class Reader {
	public:
		explicit Reader(const std::string& path);

		// False once anything was missing or short; whatever was read by then is not to be used.
		explicit operator bool() const { return mOk; }
		// What was read makes no sense to the caller: false from here on, as if it were short.
		void fail() { mOk = false; }

		template<class T> requires std::is_trivially_copyable_v<T>
		void value(T& v) { read(&v, sizeof(T)); }
		template<class T> requires std::is_trivially_copyable_v<T>
		void vector(std::vector<T>& v) {
			uint64_t size = 0;
			value(size);
			if (!mOk || size > mRemaining / sizeof(T)) {
				mOk = false;
				return;
			}
			v.resize(static_cast<size_t>(size));
			read(v.data(), v.size() * sizeof(T));
		}
		void text(std::string& s) {
			uint64_t size = 0;
			value(size);
			if (!mOk || size > mRemaining) {
				mOk = false;
				return;
			}
			s.resize(static_cast<size_t>(size));
			read(s.data(), s.size());
		}

	private:
		void read(void* data, size_t bytes);

		std::ifstream mIn;
		uint64_t mRemaining = 0;
		bool mOk = false;
	};
}
