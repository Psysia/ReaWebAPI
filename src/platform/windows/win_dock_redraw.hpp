#pragma once
#include <windows.h>
#include <commctrl.h>
#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace reaweb {
class WinDockRedraw {
  struct Host {
    std::set<HWND> children;
    bool suppressed = false;
    bool native_disabled = false;
    unsigned transitions = 0;
  };
  HWND child_ = nullptr;
  std::vector<HWND> parents_;
  static std::map<HWND, std::shared_ptr<Host>>& hosts() {
    static std::map<HWND, std::shared_ptr<Host>> value;
    return value;
  }
  static void retire(HWND hwnd, const std::shared_ptr<Host>& host) {
    auto found = hosts().find(hwnd);
    if (found == hosts().end() || found->second != host ||
        !host->children.empty() || host->transitions) return;
    RemoveWindowSubclass(hwnd, procedure, 0);
    hosts().erase(found);
  }
  static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    auto found = hosts().find(hwnd);
    if (found != hosts().end()) {
      // Native handlers can destroy or reparent the host synchronously.
      auto host = found->second;
      if (message == WM_SETREDRAW) {
        // Structural Docker changes must reach REAPER's own batching logic.
        // A native FALSE can outlive a transition token, so its matching TRUE
        // (and repeated FALSE) must still be forwarded after the token ends.
        if (host->transitions || host->native_disabled) {
          host->suppressed = false;
          host->native_disabled = !wp;
          return DefSubclassProc(hwnd, message, wp, lp);
        }
        // DefWindowProc temporarily removes WS_VISIBLE. WebView2 then drops
        // its composed surface while REAPER is resizing the Docker.
        if (!wp && (host->suppressed || std::any_of(host->children.begin(), host->children.end(), [hwnd](HWND child) {
          return IsChild(hwnd, child) && IsWindowVisible(child);
        }))) {
          host->suppressed = true;
          return 0;
        }
        if (wp && host->suppressed) {
          host->suppressed = false;
          return 0;
        }
        host->native_disabled = !wp;
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
    auto host = found->second;
    host->children.erase(child_);
    retire(parent, host);
  }
public:
  // Suspend every shared ancestor before calling the host's DockWindowRemove.
  // Keep the token alive through reparenting/restoration (or window teardown).
  // Other WebViews in the same Docker then cannot accidentally re-enable the
  // resize workaround while the host is changing its child/accessibility tree.
  class Transition {
    std::vector<std::pair<HWND, std::shared_ptr<Host>>> parents_;
  public:
    explicit Transition(WinDockRedraw& guard) {
      parents_.reserve(guard.parents_.size());
      for (auto parent : guard.parents_) {
        auto found = hosts().find(parent);
        if (found != hosts().end()) parents_.emplace_back(parent, found->second);
      }
      // Mark every ancestor before sending anything that can reenter a WndProc.
      for (auto& item : parents_) ++item.second->transitions;
      for (auto& item : parents_) {
        auto found = hosts().find(item.first);
        if (found == hosts().end() || found->second != item.second || !item.second->suppressed) continue;
        // A previously intercepted FALSE is an outstanding host batch. Convert
        // it to native suspension now; only its actual matching TRUE resumes
        // redraw, even if that TRUE arrives after this token is destroyed.
        item.second->suppressed = false;
        SendMessageW(item.first, WM_SETREDRAW, FALSE, 0);
      }
    }
    Transition(const Transition&) = delete;
    Transition& operator=(const Transition&) = delete;
    ~Transition() {
      for (auto& item : parents_) {
        auto found = hosts().find(item.first);
        // HWND values can be reused; never release a different host's token.
        if (found == hosts().end() || found->second != item.second) continue;
        --item.second->transitions;
        retire(item.first, item.second);
      }
    }
  };
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
      auto found = hosts().find(parent);
      if (found == hosts().end()) {
        auto host = std::make_shared<Host>();
        found = hosts().emplace(parent, std::move(host)).first;
        if (!SetWindowSubclass(parent, procedure, 0, 0)) {
          hosts().erase(found); continue;
        }
      }
      found->second->children.insert(child);
    }
    parents_ = std::move(next);
  }
};
}
