// Real WebView2 and process-owned windows. Most cases stay offscreen; the
// maximize regression briefly occupies the CI desktop. Never launches REAPER.
#include "../src/platform/windows/platform_win.cpp"
#include <chrono>
#include <fstream>
#include <iostream>

using namespace reaweb;
#define CHECK(value) do { if (!(value)) throw std::runtime_error("Check failed at " + std::to_string(__LINE__) + ": " #value); } while (false)

namespace {
struct NativeWindow {
  HWND value = nullptr;
  NativeWindow(HWND parent, const wchar_t* name, bool edit = false) {
    value = CreateWindowExW(parent ? 0 : WS_EX_TOOLWINDOW,
      edit ? L"EDIT" : L"STATIC", name,
      WS_VISIBLE | (parent ? WS_CHILD : WS_POPUP),
      parent ? 0 : -32000, parent ? 0 : -32000, 900, 700,
      parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    CHECK(value);
  }
  ~NativeWindow() { if (value && IsWindow(value)) DestroyWindow(value); }
  NativeWindow(const NativeWindow&) = delete;
  NativeWindow& operator=(const NativeWindow&) = delete;
};

void dispatch_slice() {
  MSG message{};
  // Bounded even if Chromium or an adversarial callback keeps posting work.
  for (int i = 0; i < 256 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++i) {
    CHECK(message.message != WM_QUIT);
    TranslateMessage(&message); DispatchMessageW(&message);
  }
}

bool style_visible(HWND hwnd) { return (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_VISIBLE) != 0; }
void offscreen(HWND hwnd, int width, int height) {
  CHECK(SetWindowPos(hwnd, nullptr, -32000, -32000, width, height,
    SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED));
}
}

int main() {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  try {
    NativeWindow host(nullptr, L"Dock transition host");
    NativeWindow sentinel(host.value, L"Focus sentinel", true);
    const auto root = fs::current_path() / ".cache" / ("dock-transition-" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(root);
    const auto entry = root / "index.html";
    std::ofstream(entry) << "<!doctype html><style>html{background:#182838}body{margin:0;overflow:hidden}</style>"
      "<input id=state value=initial>";
    bool ready = false;
    std::string error;
    Json observation;
    unsigned sequence = 0;
    WindowOptions options;
    options.entry = entry; options.title = "Dock transition regression";
    options.parent = host.value; options.hidden = true;
    options.script = R"JS(
      window.__testDragStates = [];
      (() => {
        const define = Object.defineProperty;
        Object.defineProperty = function(target, name, descriptor) {
          if (target === window && name === '__reawebDragRegionsActive') {
            Object.defineProperty = define;
            const update = descriptor.value;
            descriptor.value = value => { window.__testDragStates.push(!!value); return update(value); };
          }
          return define(target, name, descriptor);
        };
      })();
      addEventListener('DOMContentLoaded',()=>chrome.webview.postMessage('ready'));
    )JS";
    options.on_message = [&](std::string text) {
      if (text == "ready") ready = true;
      else if (text.rfind("snapshot:", 0) == 0) {
        try { observation = Json::parse(text.substr(9)); }
        catch (const std::exception& e) { error = e.what(); }
      }
    };
    options.on_error = [&](std::string text) { error = std::move(text); };
    auto platform = make_platform(root / "profile");
    auto window = platform->open(options);
    const auto hwnd = static_cast<HWND>(window->native_handle());
    CHECK(hwnd);
    offscreen(hwnd, 640, 420); // Move while hidden, before first presentation.
    const auto pump = [&](const std::function<bool()>& done, const char* phase, int seconds = 30) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
      do {
        dispatch_slice(); platform->pump();
        if (window) window->tick();
        if (!error.empty()) throw std::runtime_error(std::string(phase) + ": " + error);
        if (done()) return;
        Sleep(2);
      } while (std::chrono::steady_clock::now() < deadline);
      throw std::runtime_error(std::string("Timed out: ") + phase);
    };
    pump([&] { return ready; }, "WebView2 initialization");
    window->show();
    pump([&] { return !window->presentation_pending() && window->visible(); }, "first presentation");
    window->evaluate("document.querySelector('#state').value='retained-page-state'");
    const auto snapshot = [&] {
      const auto wanted = ++sequence;
      window->evaluate("chrome.webview.postMessage('snapshot:'+JSON.stringify({sequence:" + std::to_string(wanted) +
        ",width:innerWidth,height:innerHeight,scale:devicePixelRatio,value:document.querySelector('#state').value,"
        "dragStates:window.__testDragStates.slice()}));");
      pump([&] { return observation.is_object() && observation.value("sequence", 0u) == wanted; }, "page observation");
      CHECK(observation["value"] == "retained-page-state");
      return observation;
    };
    const auto stable_viewport = [&] {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      do {
        const auto view = snapshot();
        RECT client{}; CHECK(GetClientRect(hwnd, &client));
        const auto scale = view.at("scale").get<double>();
        if (std::abs(view.at("width").get<double>() * scale - client.right) <= 1 &&
            std::abs(view.at("height").get<double>() * scale - client.bottom) <= 1) return view;
        Sleep(2);
      } while (std::chrono::steady_clock::now() < deadline);
      throw std::runtime_error("Final WebView2 viewport did not match its native client");
    };
    stable_viewport();

    for (int cycle = 0; cycle < 6; ++cycle) {
      NativeWindow pane(host.value, L"Last-tab Docker fixture");
      window->dock_transition([&] {
        window->prepare_dock();
        ShowWindow(hwnd, SW_HIDE);
        SetParent(hwnd, pane.value);
        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_CHILD | WS_VISIBLE);
        CHECK(SetWindowPos(hwnd, nullptr, 0, 0, 390 + cycle * 17, 250 + cycle * 11,
          SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED));
      });
      CHECK(window->dock_transition_pending());
      pump([&] { return !window->dock_transition_pending(); }, "dock completion");
      CHECK(GetParent(hwnd) == pane.value);
      const auto before = stable_viewport();
      SetFocus(sentinel.value);
      CHECK(GetFocus() == sentinel.value);

      // Leave a finish notification queued, then enter a new host removal.
      // A nested native message pump must not complete the newer transition.
      window->dock_transition([] {});
      window->dock_transition([&] {
        CHECK(window->dock_transition_pending());
        window->prepare_undock();
        SendMessageW(pane.value, WM_SETREDRAW, FALSE, 0);
        CHECK(!style_visible(pane.value));
        CHECK(SetWindowPos(hwnd, nullptr, 0, 0, 730, 480,
          SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED));
        SendMessageW(hwnd, WM_MOVE, 0, 0);
        window->focus(); window->focus();
        CHECK(GetFocus() == sentinel.value);
        // ExecuteScript is test observation only. Its nested message pump also
        // delivers the older finish message while the host callback is active.
        const auto during = snapshot();
        CHECK(window->dock_transition_pending());
        CHECK(during["width"] == before["width"] && during["height"] == before["height"]);
        CHECK(GetFocus() == sentinel.value);

        ShowWindow(hwnd, SW_HIDE);
        SetParent(hwnd, nullptr);
        SendMessageW(pane.value, WM_SETREDRAW, TRUE, 0);
        CHECK(style_visible(pane.value));
        CHECK(DestroyWindow(pane.value)); // Last tab: container dies before token release.
        pane.value = nullptr;
        window->restore_floating();
        CHECK(!style_visible(hwnd)); // Focus and presentation are still deferred.
        offscreen(hwnd, 610 + cycle * 19, 410 + cycle * 13);
        window->focus();
        CHECK(GetFocus() == sentinel.value);
      });
      CHECK(window->dock_transition_pending());
      CHECK(GetFocus() == sentinel.value);
      pump([&] { return !window->dock_transition_pending() && window->focused(); }, "floating focus recovery");
      CHECK(!(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD));
      CHECK(GetWindow(hwnd, GW_OWNER) == host.value);
      CHECK(window->visible());
      stable_viewport();
    }

    // Frameless drag-region scripts must follow the completed native parent
    // change, even when a host callback pumps browser messages while removing.
    CHECK(window->window_capabilities()["frameless"] == true);
    window->set_frameless(true);
    CHECK(window->frameless_active());
    const auto floating_drag = snapshot()["dragStates"];
    CHECK(!floating_drag.empty() && floating_drag.back() == true);
    {
      NativeWindow pane(host.value, L"Frameless Docker fixture");
      window->dock_transition([&] {
        window->prepare_dock(); ShowWindow(hwnd, SW_HIDE);
        SetParent(hwnd, pane.value);
        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_CHILD | WS_VISIBLE);
        CHECK(SetWindowPos(hwnd, nullptr, 0, 0, 480, 300, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED));
        CHECK(snapshot()["dragStates"] == floating_drag);
      });
      pump([&] { return !window->dock_transition_pending(); }, "frameless dock completion");
      CHECK(!window->frameless_active());
      const auto docked_drag = stable_viewport()["dragStates"];
      CHECK(docked_drag.size() == floating_drag.size() + 1 && docked_drag.back() == false);
      SetFocus(sentinel.value); CHECK(GetFocus() == sentinel.value);
      window->dock_transition([&] {
        window->prepare_undock(); ShowWindow(hwnd, SW_HIDE);
        SetParent(hwnd, nullptr);
        window->restore_floating(); offscreen(hwnd, 670, 430);
        CHECK(snapshot()["dragStates"] == docked_drag);
        CHECK(GetFocus() == sentinel.value && !style_visible(hwnd));
      });
      pump([&] { return !window->dock_transition_pending() && window->focused(); }, "frameless floating completion");
      const auto restored_drag = stable_viewport()["dragStates"];
      CHECK(window->frameless_active());
      CHECK(restored_drag.size() == docked_drag.size() + 1 && restored_drag.back() == true);
    }
    window->set_frameless(false);

    // Maximizing is intentionally visible on the isolated CI desktop. Returning
    // to a maximized floating window must not activate it inside host removal.
    for (const bool frameless : {false, true}) {
      window->set_frameless(frameless);
      ShowWindow(hwnd, SW_SHOWMAXIMIZED);
      pump([&] { return IsZoomed(hwnd) != FALSE; }, "initial maximization");
      stable_viewport();
      NativeWindow pane(host.value, L"Maximized Docker fixture");
      window->dock_transition([&] {
        window->prepare_dock(); ShowWindow(hwnd, SW_HIDE);
        SetParent(hwnd, pane.value);
        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_CHILD | WS_VISIBLE);
        CHECK(SetWindowPos(hwnd, nullptr, 0, 0, 510, 330, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED));
      });
      pump([&] { return !window->dock_transition_pending(); }, "maximized dock completion");
      CHECK(!IsZoomed(hwnd));
      stable_viewport();
      SetFocus(sentinel.value); CHECK(GetFocus() == sentinel.value);
      window->dock_transition([&] {
        window->prepare_undock(); ShowWindow(hwnd, SW_HIDE);
        SetParent(hwnd, nullptr); window->restore_floating();
        CHECK(!IsZoomed(hwnd) && !style_visible(hwnd));
        CHECK(GetFocus() == sentinel.value);
        snapshot(); // Nested dispatch cannot reveal or maximize the window early.
        CHECK(window->dock_transition_pending());
        CHECK(!IsZoomed(hwnd) && !style_visible(hwnd));
        CHECK(GetFocus() == sentinel.value);
      });
      CHECK(!IsZoomed(hwnd) && !style_visible(hwnd));
      pump([&] { return !window->dock_transition_pending() && IsZoomed(hwnd) && window->focused(); }, "maximized floating completion");
      CHECK(window->visible() && GetWindow(hwnd, GW_OWNER) == host.value);
      stable_viewport();
      ShowWindow(hwnd, SW_RESTORE); offscreen(hwnd, 680, 440);
      CHECK(!IsZoomed(hwnd));
      stable_viewport();
    }
    window->set_frameless(false);

    bool caught = false;
    try {
      window->dock_transition([] { throw std::runtime_error("injected host failure"); });
    } catch (const std::runtime_error& e) { caught = std::string(e.what()) == "injected host failure"; }
    CHECK(caught && window->dock_transition_pending());
    pump([&] { return !window->dock_transition_pending(); }, "exception cleanup");
    stable_viewport();

    // Close while the final notification is still queued: no stale HWND access,
    // no native callback may resurrect the browser or its destroyed container.
    NativeWindow closing_pane(host.value, L"Closing Docker fixture");
    window->dock_transition([&] {
      window->prepare_dock(); ShowWindow(hwnd, SW_HIDE);
      SetParent(hwnd, closing_pane.value);
      SetWindowLongPtrW(hwnd, GWL_STYLE, WS_CHILD | WS_VISIBLE);
    });
    pump([&] { return !window->dock_transition_pending(); }, "final dock completion");
    window->dock_transition([&] {
      window->prepare_undock();
      ShowWindow(hwnd, SW_HIDE); SetParent(hwnd, nullptr);
      CHECK(DestroyWindow(closing_pane.value)); closing_pane.value = nullptr;
    });
    CHECK(window->dock_transition_pending());
    window.reset();
    CHECK(!IsWindow(hwnd));
    dispatch_slice();
    platform.reset();
    std::cout << "PASS: real WebView2, six dock/float cycles, deferred viewport/focus, nested message pump, "
      "page state, owner, frameless scripts, framed/frameless maximized restoration, last-tab host destruction, "
      "exception recovery and close with queued completion\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
