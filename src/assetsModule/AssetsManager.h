#pragma once
#include <cassert>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Asset.h"
#include "containersModule/Singleton.h"
#include "modelModule/Model.h"

namespace AssetsModule {

	// Every asset, by the path (or name) it was made for. A path is taken as the file it names, so "a/b.png",
	// "a\\b.png" and "a/./b.png" are one asset. Assets live until the end: pointers to them are held all over, and what
	// was made from one -- meshes on the GPU, pointers into it -- is not freed piecemeal. Any thread.
	class AssetsManager : public SFE::Singleton<AssetsManager> {
	public:
		AssetsManager() = default;
		~AssetsManager() override;

		// nullptr when there is none, or it is not a T.
		template <class T>
		T* getAsset(const std::string& path) {
			std::lock_guard lock(mMutex);
			return find<T>(key(path));
		}

		// The asset `path` names, made from `args` if there is none yet. Made with the lock let go: a model made on a
		// worker thread waits on the main thread to bind its meshes, and the main thread may be asking for an asset
		// meanwhile (a pass's texture) -- each waited on the other for good. Made twice at once, the first made is the one
		// kept; the other is kept aside till the end, as a replaced one is.
		template <class T, class... ARGS>
		T* createAsset(const std::string& path, ARGS&&... args) {
			auto id = key(path);
			{
				std::lock_guard lock(mMutex);
				if (mAssets.contains(id)) {
					return find<T>(id);
				}
			}
			auto* asset = new T(std::forward<ARGS>(args)...);
			asset->assetPath = path;
			stamp(*asset);
			std::lock_guard lock(mMutex);
			if (mAssets.contains(id)) {
				mReplaced.push_back(asset);
				return find<T>(id);
			}
			mAssets.emplace(std::move(id), asset);
			return asset;
		}

		// `asset` is what `path` names from now on (read again from a file edited while the engine runs). The one it
		// replaces is kept until the end, as every asset is.
		template <class T>
		void replaceAsset(const std::string& path, T* asset) {
			asset->assetPath = path;
			stamp(*asset);
			std::lock_guard lock(mMutex);
			auto& slot = mAssets[key(path)];
			if (slot) {
				mReplaced.push_back(slot);
			}
			slot = asset;
		}

		// The T assets read from files that have been written since.
		template <class T>
		std::vector<T*> changedOnDisk() {
			std::vector<T*> changed;
			std::lock_guard lock(mMutex);
			for (const auto& [id, asset] : mAssets) {
				if (auto* typed = dynamic_cast<T*>(asset); typed && changedOnDisk(*asset)) {
					changed.push_back(typed);
				}
			}
			return changed;
		}

		// Notes that the asset matches its file as written now (read again into the same asset).
		static void stamp(Asset& asset);

	private:
		static std::string key(const std::string& path);
		static bool changedOnDisk(const Asset& asset);

		template <class T>
		T* find(const std::string& id) {
			const auto it = mAssets.find(id);
			if (it == mAssets.end()) {
				return nullptr;
			}
			auto* typed = dynamic_cast<T*>(it->second);
			assert(typed && "an asset asked for as a type it is not");
			return typed;
		}

		std::mutex mMutex;
		std::unordered_map<std::string, Asset*> mAssets;
		std::vector<Asset*> mReplaced;
	};
}
