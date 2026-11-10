#pragma once
#include <windows.h>
#include <commctrl.h>
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace reaweb {
class WinDockRedraw {
  struct Host {
    std::set<HWND> children;
    bool suppressed = false;
  };
  HWND child_ = nullptr;
  std::vector<HWND> parents_;
  static std::map<HWND, Host>& hosts() {
    static std::map<HWND, Host> value;
    return value;
  }
  static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    auto found = hosts().find(hwnd);
    if (found != hosts().end()) {
      auto& host = found->second;
      if (message == WM_SETREDRAW) {
        // DefWindowProc temporarily removes WS_VISIBLE. WebView2 then drops
        // its composed surface while REAPER is resizing the Docker.
        if (!wp && (host.suppressed || std::any_of(host.children.begin(), host.children.end(), [hwnd](HWND child) {
          return IsChild(hwnd, child) && IsWindowVisible(child);
        }))) {
          host.suppressed = true;
          return 0;
        }
        if (wp && host.suppressed) {
          host.suppressed = false;
          return 0;
        }
      } else if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, procedure, 0);
        hosts().erase(found);
      }
    }
    return DefSubclassProc(hwnd, message, wp, lp);
  }
  void remove(HWND parent) {
    auto found = hosts().find(parent);
    if (found == hosts().end()) return;
    found->second.children.erase(child_);
    if (found->second.children.empty()) {
      RemoveWindowSubclass(parent, procedure, 0);
      hosts().erase(found);
    }
  }
public:
  ~WinDockRedraw() { detach(); }
  void detach() {
    for (auto parent : parents_) remove(parent);
    parents_.clear(); child_ = nullptr;
  }
  void sync(HWND child) {
    if (child_ != child) { detach(); child_ = child; }
    std::vector<HWND> next;
    if (GetWindowLongPtrW(child, GWL_STYLE) & WS_CHILD) {
      for (auto parent = GetParent(child); parent; parent = GetParent(parent)) {
        // Docker child containers need the WebView surface workaround, but
        // the top-level DAW owns redraw batching for the entire project.
        // Swallowing its WM_SETREDRAW disables REAPER's UI-refresh suspension
        // whenever a visible WebView is docked (including PreventUIRefresh).
        if (!(GetWindowLongPtrW(parent, GWL_STYLE) & WS_CHILD)) break;
        next.push_back(parent);
      }
    }
    if (next == parents_) return;
    for (auto parent : parents_) if (std::find(next.begin(), next.end(), parent) == next.end()) remove(parent);
    for (auto parent : next) {
      auto& host = hosts()[parent];
      if (host.children.empty() && !SetWindowSubclass(parent, procedure, 0, 0)) {
        hosts().erase(parent); continue;
      }
      host.children.insert(child);
    }
    parents_ = std::move(next);
  }
};
}
