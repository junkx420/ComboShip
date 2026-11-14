// combo/gui/ComboAnchorRoomWindow.h
//
// ComboShip: combo-native floating Anchor room window. Replaces soh's AnchorRoomWindow (hidden in
// combo builds). Reads the launcher-owned always-live roster (set via ComboUI_SetAnchorRosterProvider)
// so every player shows once with a correct area name in BOTH games — even when a game is dormant.
// Area names resolved via the stateless SOH_/MM_Anchor_ResolveScene exports.
// Teleport is same-game only (button rendered only for players in the locally-active game).
#pragma once

#include <ship/window/gui/GuiWindow.h>
#include <string>

namespace ComboRando {

// Registered in ComboUI_Register alongside the tracker windows; toggled from the combo Anchor panel.
void RegisterAnchorRoomWindow();

// Co-op hardening (2026-11-13): raw launcher roster snapshot (incl. "link" health), "{}" if the launcher
// has not registered its provider. For the network panel status line and the diagnostics bundle.
std::string AnchorRosterJson();

class ComboAnchorRoomWindow final : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;
    void Draw() override;

  protected:
    void InitElement() override {
    }
    void UpdateElement() override {
    }
    void DrawElement() override;
};

} // namespace ComboRando
