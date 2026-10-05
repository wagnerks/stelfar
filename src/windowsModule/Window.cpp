#include "Window.h"

#include <algorithm>
#include <filesystem>
#include <functional>

#include <json/value.h>

#include "core/FileSystem.h"
#include "logsModule/logger.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

namespace SFE::Render {
	void errorCallback(int error, const char* description) {
		LogsModule::Logger::LOG_ERROR("GLFW Error: %d, %s\n", error, description);
	}

	WindowSystem::~WindowSystem() {
		for (auto& [title, window] : mWindows) {
			delete window;
		}

		mWindows.clear();
	}

	void WindowSystem::init() {
		if (mInited) {
			return;
		}
		mInited = true;

		if (!glfwInit()) {
			return;
		}

		glfwSetErrorCallback(errorCallback);
	}

	Window* WindowSystem::createWindow(const std::string& windowName, GLFWwindow* share, int w, int h, const WindowHints& hints) {
		const auto it = mWindows.find(windowName);
		if (it != mWindows.end()) {
			return it->second;
		}

		const auto window = new Window(w, h, windowName, share, hints);
		mWindows[windowName] = window;

		return window;
	}

	void WindowSystem::deleteWindow(const std::string& windowName) {
		const auto it = mWindows.find(windowName);
		if (it != mWindows.end()) {
			delete it->second;
			mWindows.erase(it);
		}
	}

	void WindowHints::apply() {
		for (auto& [hint, value] : bools) {
			glfwWindowHint(hint, value);
		}

		for (auto& [hint, value] : ints) {
			glfwWindowHint(hint, value);
		}

		for (auto& [hint, value] : strings) {
			glfwWindowHintString(hint, value.c_str());
		}
	}

	Window::~Window() {
		if (mWindow) {
			glfwDestroyWindow(mWindow);
			windows.erase(mWindow);
		}
	}

	Window::Window(int w, int h, const std::string& title, GLFWwindow* share, const WindowHints& hints) {
		create(w, h, title, share, hints);
	}

	Window::Window(int w, int h, const std::string& title) : mTitle(title), mHeadless(true) {
		data.width = w;
		data.height = h;
		data.renderW = w;
		data.renderH = h;
		data.backbufferW = w;
		data.backbufferH = h;
	}

	void Window::create(int w, int h, const std::string& title, GLFWwindow* share, WindowHints hints) {
		data.width = w;
		data.height = h;

		hints.apply();
		//glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
		mNoApi = hints.ints.contains(CLIENT_API) && hints.ints.at(CLIENT_API) == GLFW_NO_API;
		mTitle = title;
		mWindow = glfwCreateWindow(w, h, title.c_str(), nullptr, share);
		if (!mWindow) {
			LogsModule::Logger::LOG_FATAL(false, "Failed to create glfw window \"%s\"", title.c_str());
			return;
		}
		windows[mWindow] = this;
		
		if (!mNoApi) {
			glfwMakeContextCurrent(mWindow);
		}

		glfwGetWindowPos(mWindow, &data.posX, &data.posY);

		glfwGetWindowSize(mWindow, &data.width, &data.height);
		glfwGetWindowContentScale(mWindow, &data.renderScaleW, &data.renderScaleH);
		glfwGetFramebufferSize(mWindow, &data.backbufferW, &data.backbufferH);
		// Internal render resolution stays at requested size (matches editor shared-context window).
		data.renderW = w;
		data.renderH = h;
		data.width = w;
		data.height = h;
		glfwDefaultWindowHints();

		glfwSetWindowPosCallback(mWindow, [](GLFWwindow* window, int x, int y) {
			if (windows.contains(window)) {
				if (const auto win = windows[window]) {
					win->data.posX = x;
					win->data.posY = y;
					if (win->onPosChange) {
						win->onPosChange(x, y);
					}
				}
			}
		});

		glfwSetFramebufferSizeCallback(mWindow, [](GLFWwindow* window, int width, int height) {
			if (windows.contains(window)) {
				if (const auto win = windows[window]) {
					win->data.backbufferW = width;
					win->data.backbufferH = height;
					if (win->onFramebufferResize) {
						win->onFramebufferResize(width, height);
					}
				}
			}
		});

		glfwSetKeyCallback(mWindow, [](GLFWwindow* window, int key, int scancode, int action, int mods) {
			if (windows.contains(window)) {
				if (const auto win = windows[window]) {
					if (windows[window]->keyCallback) {
						windows[window]->keyCallback(key, scancode, action, mods);
					}
				}
			}
		});

		glfwSetMouseButtonCallback(mWindow, [](GLFWwindow* window, int btn, int act, int mode) {
			if (windows.contains(window)) {
				if (const auto win = windows[window]) {
					if (windows[window]->mouseButtonCallback) {
						windows[window]->mouseButtonCallback(btn, act, mode);
					}
				}
			}
		});

		glfwSetCursorPosCallback(mWindow, [](GLFWwindow* window, double xPos, double yPos) {
			if (windows.contains(window)) {
				if (const auto win = windows[window]) {
					if (windows[window]->cursorPosCallback) {
						windows[window]->cursorPosCallback(xPos, yPos);
					}
				}
			}
		});

		glfwSetScrollCallback(mWindow, [](GLFWwindow* window, double xOffset, double yOffset) {
			if (windows.contains(window)) {
				if (const auto win = windows[window]) {
					if (windows[window]->scrollCallback) {
						windows[window]->scrollCallback(xOffset, yOffset);
					}
				}
			}
		});

		glfwSetWindowCloseCallback(mWindow, [](GLFWwindow* window) {
			if (!glfwGetWindowAttrib(window, GLFW_VISIBLE)) {
				glfwSetWindowShouldClose(window, GLFW_FALSE);
				return;
			}
			if (windows.contains(window)) {
				if (const auto win = windows[window]) {
					if (win->onCloseCallback) {
						win->onCloseCallback();
					}
				}
			}
		});
	}

	bool Window::isClosing() const {
		if (mHeadless) {
			return mHeadlessClosing;
		}
		if (!mWindow) {
			return true;
		}
		return glfwWindowShouldClose(mWindow);
	}

	bool Window::isVisible() const {
		return mWindow && glfwGetWindowAttrib(mWindow, GLFW_VISIBLE);
	}

	void Window::show() const {
		if (mWindow) {
			glfwShowWindow(mWindow);
		}
	}

	void Window::hide() const {
		if (mWindow) {
			glfwHideWindow(mWindow);
		}
	}

	void Window::swapBuffers() const {
		if (!mWindow) {
			return;
		}
		glfwSwapBuffers(mWindow);
	}

	void Window::setInputMode(CursorType type) {
		if (mWindow) {
			glfwSetInputMode(mWindow, GLFW_CURSOR, type);
		}
	}

	void Window::close() {
		setShouldClose(true);
	}

	void Window::setShouldClose(const bool close) {
		if (mHeadless) {
			mHeadlessClosing = close;
		}
		else if (mWindow) {
			glfwSetWindowShouldClose(mWindow, close);
		}
	}

	namespace {
		std::string displayFile() {
			return (std::filesystem::current_path() / "display.json").string();
		}
	}

	DisplaySettings displayFromJson(const Json::Value& json, DisplaySettings settings) {
		if (!json.isObject()) {
			return settings;
		}
		const auto mode = json["mode"].isString() ? json["mode"].asString() : std::string();
		for (size_t i = 0; i < std::size(kDisplayModeNames); ++i) {
			if (mode == kDisplayModeNames[i]) {
				settings.mode = static_cast<DisplayMode>(i);
			}
		}
		if (json["width"].isInt() && json["height"].isInt()) {
			settings.width = json["width"].asInt();
			settings.height = json["height"].asInt();
		}
		if (json["frameInfo"].isBool()) {
			settings.frameInfo = json["frameInfo"].asBool();
		}
		return settings;
	}

	DisplaySettings loadDisplaySettings() {
		Json::Value json;
		std::error_code error;
		if (std::filesystem::exists(displayFile(), error)) {
			FileSystem::readJson(displayFile(), json);
		}
		return displayFromJson(json, {});
	}

	void saveDisplaySettings(const DisplaySettings& settings) {
		Json::Value json;
		json["mode"] = kDisplayModeNames[static_cast<size_t>(settings.mode)];
		json["width"] = settings.width;
		json["height"] = settings.height;
		json["frameInfo"] = settings.frameInfo;
		FileSystem::writeJson(displayFile(), json);
	}

	GLFWmonitor* Window::displayMonitor() const {
		if (auto* monitor = glfwGetWindowMonitor(mWindow)) {
			return monitor;
		}
		int x = 0, y = 0, w = 0, h = 0;
		glfwGetWindowPos(mWindow, &x, &y);
		glfwGetWindowSize(mWindow, &w, &h);
		int count = 0;
		auto** monitors = glfwGetMonitors(&count);
		for (int i = 0; i < count; ++i) {
			int monitorX = 0, monitorY = 0;
			glfwGetMonitorPos(monitors[i], &monitorX, &monitorY);
			const auto* mode = glfwGetVideoMode(monitors[i]);
			if (mode && x + w / 2 >= monitorX && x + w / 2 < monitorX + mode->width && y + h / 2 >= monitorY
				&& y + h / 2 < monitorY + mode->height) {
				return monitors[i];
			}
		}
		return glfwGetPrimaryMonitor();
	}

	std::vector<std::pair<int, int>> Window::displaySizes(const DisplayMode mode) const {
		std::vector<std::pair<int, int>> sizes;
		auto* monitor = mWindow ? displayMonitor() : nullptr;
		const auto* own = monitor ? glfwGetVideoMode(monitor) : nullptr;
		if (!own) {
			return sizes;
		}
		if (mode == DisplayMode::Borderless) {
			return { { own->width, own->height } };
		}
		int count = 0;
		const auto* modes = glfwGetVideoModes(monitor, &count);
		for (int i = 0; i < count; ++i) {
			sizes.emplace_back(modes[i].width, modes[i].height);
		}
		if (mode == DisplayMode::Windowed) {
			sizes.insert(sizes.end(), { { 2560, 1440 }, { 1920, 1080 }, { 1600, 900 }, { 1280, 720 } });
			// Lower than the monitor, so its title bar is on the screen too.
			std::erase_if(sizes, [own](const auto& size) { return size.first > own->width || size.second >= own->height; });
		}
		std::ranges::sort(sizes, std::greater{});
		sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
		return sizes;
	}

	void Window::setDisplay(DisplaySettings settings) {
		const auto sizes = displaySizes(settings.mode);
		if (!sizes.empty() && std::ranges::find(sizes, std::pair{ settings.width, settings.height }) == sizes.end()) {
			settings.width = sizes.front().first;
			settings.height = sizes.front().second;
		}
		mDisplayDirty |= !mDisplayOwned || settings.mode != mDisplay.mode || settings.width != mDisplay.width
			|| settings.height != mDisplay.height;
		mDisplay = settings;
		mDisplayOwned = true;
	}

	bool Window::applyDisplay() {
		if (!mDisplayDirty || !mWindow) {
			return false;
		}
		mDisplayDirty = false;
		auto* monitor = displayMonitor();
		const auto* own = glfwGetVideoMode(monitor);
		if (!own) {
			return false;
		}
		const int w = mDisplay.width;
		const int h = mDisplay.height;
		// The size is the player's to set here, not the frame's to drag.
		glfwSetWindowAttrib(mWindow, GLFW_RESIZABLE, GLFW_FALSE);
		if (mDisplay.mode == DisplayMode::Fullscreen) {
			glfwSetWindowMonitor(mWindow, monitor, 0, 0, w, h, own->refreshRate);
		}
		else if (mDisplay.mode == DisplayMode::Borderless) {
			int x = 0, y = 0;
			glfwGetMonitorPos(monitor, &x, &y);
			glfwSetWindowAttrib(mWindow, GLFW_DECORATED, GLFW_FALSE);
			glfwSetWindowMonitor(mWindow, nullptr, x, y, w, h, GLFW_DONT_CARE);
		}
		else {
			// In the middle of the monitor's free area, its title bar on it.
			int x = 0, y = 0, areaW = 0, areaH = 0, top = 0;
			glfwGetMonitorWorkarea(monitor, &x, &y, &areaW, &areaH);
			glfwSetWindowAttrib(mWindow, GLFW_DECORATED, GLFW_TRUE);
			glfwSetWindowMonitor(mWindow, nullptr, x, y, w, h, GLFW_DONT_CARE);
			glfwGetWindowFrameSize(mWindow, nullptr, &top, nullptr, nullptr);
			glfwSetWindowPos(mWindow, x + (std::max)(0, (areaW - w) / 2), y + top + (std::max)(0, (areaH - top - h) / 2));
		}
		if (w == data.renderW && h == data.renderH) {
			return false;
		}
		data.setSize(w, h);
		return true;
	}

#ifdef _WIN32
	void* Window::getNativeHandle() const
	{
		return mWindow ? reinterpret_cast<void*>(glfwGetWin32Window(mWindow)) : nullptr;
	}

	bool Window::embedInHost(void* ownerHwnd, int screenX, int screenY, int w, int h)
	{
		if (!mWindow || w <= 0 || h <= 0) {
			return false;
		}
		HWND hwnd = glfwGetWin32Window(mWindow);
		if (!hwnd || !IsWindow(hwnd)) {
			return false;
		}
		if (ownerHwnd) {
			HWND owner = static_cast<HWND>(ownerHwnd);
			if (!IsWindow(owner)) {
				return false;
			}
		}

		mEmbeddedInHost = true;
		SetWindowPos(hwnd, HWND_TOP, screenX, screenY, w, h, SWP_SHOWWINDOW | SWP_NOACTIVATE);
		ShowWindow(hwnd, SW_SHOWNA);
		return true;
	}

	void Window::refreshEmbeddedOverlay(void* ownerHwnd, int screenX, int screenY, int w, int h) const
	{
		if (!mEmbeddedInHost || !mWindow || w <= 0 || h <= 0) {
			return;
		}
		HWND hwnd = glfwGetWin32Window(mWindow);
		if (!hwnd || !IsWindow(hwnd)) {
			return;
		}
		if (ownerHwnd) {
			HWND owner = static_cast<HWND>(ownerHwnd);
			if (!IsWindow(owner)) {
				return;
			}
		}
		SetWindowPos(hwnd, HWND_TOP, screenX, screenY, w, h, SWP_SHOWWINDOW | SWP_NOACTIVATE);
	}

	void Window::detachFromHost()
	{
		if (!mEmbeddedInHost || !mWindow) {
			return;
		}

		glfwHideWindow(mWindow);
		mEmbeddedInHost = false;
	}
#endif
}
