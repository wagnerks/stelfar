#include "ImGuiPass.h"
#include "core/GameModule.h"

#include "backends/imgui_impl_glfw.h"
#include "core/Engine.h"
#include "debugModule/imguiDecorator.h"
#include "debugModule/Profiler.h"
#include "imgui.h"
#include "systemsModule/GraphicsSettingsGui.h"
#include "systemsModule/systems/RenderSystem.h"
#include "windowsModule/Window.h"

#include <GLFW/glfw3.h>

namespace SFE::Render::RenderPasses {

	ImGuiPass::~ImGuiPass() {
		if (mContext) {
			ImGuiContext* restore = ImGui::GetCurrentContext();
			if (restore == mContext) {
				restore = nullptr;
			}
			ImGui::SetCurrentContext(mContext);
			if (mGlfw) {
				ImGui_ImplGlfw_Shutdown();
				mGlfw = false;
			}
			mRenderer.shutdown();
			ImGui::DestroyContext(mContext);
			mContext = nullptr;
			ImGui::SetCurrentContext(restore);
		}
		else {
			mRenderer.shutdown();
		}
	}

	void ImGuiPass::init(SystemsModule::RenderData&) {
		auto* rhi = Engine::instance()->rhi();
		if (!rhi) {
			return;
		}
		mRenderer.init(rhi);
		ensureContext();
		if (!mContext) {
			return;
		}
		ImGuiContext* restore = ImGui::GetCurrentContext();
		ImGui::SetCurrentContext(mContext);
		mRenderer.createFontsTexture();
		ImGui::SetCurrentContext(restore);
	}

	void ImGuiPass::ensureContext() {
		if (mContext) {
			return;
		}
		GLFWwindow* window = Engine::instance()->getMainWindow();
		if (!window) {
			return;
		}
		ImGuiContext* restore = ImGui::GetCurrentContext();
		IMGUI_CHECKVERSION();
		mContext = ImGui::CreateContext();
		ImGui::SetCurrentContext(mContext);
		// A capture must not depend on the window layout the previous run left in imgui.ini.
		if (Engine::instance()->capturingFrame()) {
			ImGui::GetIO().IniFilename = nullptr;
		}
		Debug::applyDefaultImGuiStyle();
		ImGui_ImplGlfw_InitForOther(window, true);
		mGlfw = true;
		// The backend's window hook finds its data through the current context, from the first message the window gets:
		// with no other context current, this one stays so. A frame drawn leaves it current too, but a window drawn off
		// screen (a background run) draws none.
		ImGui::SetCurrentContext(restore ? restore : mContext);
	}

	void ImGuiPass::render(SystemsModule::RenderData& renderDataHandle, CommandListHandle cmd) {
		if (!Engine::instance()->presentToScreen()) {
			return;
		}
		if (!mRenderer.ready()) {
			return;
		}

		ensureContext();
		if (!mContext) {
			return;
		}

		mPrevious = ImGui::GetCurrentContext();
		if (mPrevious == mContext) {
			mPrevious = nullptr;
		}
		else {
			ImGui::SetCurrentContext(mContext);
		}

		ImGui_ImplGlfw_NewFrame();
		Debug::freezeImGuiForCapture();
		ImGui::NewFrame();
		ImGui::SetNextWindowSize(ImVec2(380.f, 560.f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSizeConstraints(ImVec2(280.f, 200.f), ImVec2(FLT_MAX, FLT_MAX));
		if (ImGui::Begin("Graphics")) {
			SystemsModule::drawGraphicsSettings(renderDataHandle.graphics);
		}
		ImGui::End();
		// Whatever windows the game brought: the engine has none of its own past the graphics settings.
		for (const auto& panel : GameModule::panels()) {
			ImGui::SetNextWindowSize(ImVec2(panel.size.x, panel.size.y), ImGuiCond_FirstUseEver);
			if (ImGui::Begin(panel.title.c_str())) {
				panel.draw();
			}
			ImGui::End();
		}
		Debug::Profiler::drawWindow();
		ImGui::Render();

		// The frame is built either way, since windows elsewhere call into ImGui every frame and would find no
		// frame open; it just stays out of a capture, which has to come out the same on both backends and on
		// every run, and these windows show live frame times.
		if (!Engine::instance()->capturingFrame()) {
			const auto fb = renderDataHandle.frame.framebuffer(FrameId::Final);
			const auto& screen = Engine::instance()->getWindow()->getScreenData();
			mRenderer.render(cmd, ImGui::GetDrawData(), fb, screen.renderW, screen.renderH);
		}

		if (mPrevious) {
			ImGui::SetCurrentContext(mPrevious);
			mPrevious = nullptr;
		}
	}

}
