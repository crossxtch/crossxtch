#include "screens/WifiListScreen.h"

#include <Gfx.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>

#include "core/UiList.h"
#include "core/UiText.h"
#include "core/fontIds.h"
#include "network/WifiCredentialStore.h"
#include "network/WifiSession.h"
#include "screens/FileTransferScreen.h"
#include "screens/KeyboardScreen.h"

void WifiListScreen::onEnter() {
  Screen::onEnter();
  WifiSession::begin();
  state = State::Scanning;
  networks.clear();
  index = 0;
  window = 0;
  pendingSsid.clear();
  enteredPassword.clear();
  usedSavedPassword = false;
  autoConnecting = false;
  skipAutoJoin = false;
  wifiManager.startScan();
  requestUpdate();
}

void WifiListScreen::onExit() {
  Screen::onExit();
  WifiSession::end(gfx);
}

bool WifiListScreen::promptPassword() {
  auto keyboard = makeUniqueNoThrow<KeyboardScreen>(gfx, input, *this, uiText::wifiPassword,
                                                    WifiCredentialStore::kPasswordLen - 1, /*passwordMode=*/false);
  if (!keyboard) {
    LOG_ERR("WIFI", "OOM: keyboard");
    setNotice(uiText::outOfMemory);
    return false;
  }
  notice = nullptr;
  push(std::move(keyboard));
  return true;
}

void WifiListScreen::showNetworkList() {
  autoConnecting = false;
  usedSavedPassword = false;
  state = State::NetworkList;
  requestUpdate();
}

void WifiListScreen::selectNetwork(const bool fromAutoJoin) {
  if (networks.empty()) {
    return;
  }
  const auto& net = networks[static_cast<size_t>(index)];
  pendingSsid = net.ssid;
  enteredPassword.clear();
  usedSavedPassword = false;
  autoConnecting = fromAutoJoin;

  if (!net.encrypted) {
    startConnecting(nullptr);
    return;
  }

  if (const auto* cred = wifiCredentials.find(net.ssid.c_str())) {
    enteredPassword = cred->password;
    usedSavedPassword = true;
    startConnecting(cred->password);
    return;
  }

  promptPassword();
}

bool WifiListScreen::tryAutoJoin() {
  if (skipAutoJoin || networks.empty()) {
    return false;
  }

  int best = -1;
  const char* last = wifiCredentials.lastConnected();
  if (last[0] != '\0' && wifiCredentials.find(last)) {
    for (size_t i = 0; i < networks.size(); ++i) {
      if (networks[i].ssid == last) {
        best = static_cast<int>(i);
        break;
      }
    }
  }
  if (best < 0) {
    int32_t bestRssi = 0;
    for (size_t i = 0; i < networks.size(); ++i) {
      if (!wifiCredentials.find(networks[i].ssid.c_str())) {
        continue;
      }
      if (best < 0 || networks[i].rssi > bestRssi) {
        best = static_cast<int>(i);
        bestRssi = networks[i].rssi;
      }
    }
  }
  if (best < 0) {
    return false;
  }

  index = best;
  LOG_INF("WIFI", "Auto-joining '%s'", networks[static_cast<size_t>(best)].ssid.c_str());
  selectNetwork(true);
  return true;
}

void WifiListScreen::startConnecting(const char* password) {
  const unsigned long timeout =
      autoConnecting ? WifiManager::kAutoConnectTimeoutMs : WifiManager::kConnectTimeoutMs;
  wifiManager.connect(pendingSsid.c_str(), password, timeout);
  state = State::Connecting;
  requestUpdate();
}

void WifiListScreen::onPasswordEntered(const std::string& password) {
  enteredPassword = password;
  usedSavedPassword = false;
  autoConnecting = false;
  startConnecting(password.c_str());
}

void WifiListScreen::onPasswordCancelled() { showNetworkList(); }

void WifiListScreen::onConnectFailed() {
  if (usedSavedPassword) {
    state = State::ClearPassword;
  } else {
    state = State::Failed;
  }
  autoConnecting = false;
  requestUpdate();
}

void WifiListScreen::goToFileTransfer() {
  if (!enteredPassword.empty()) {
    wifiCredentials.addOrUpdate(pendingSsid.c_str(), enteredPassword.c_str());
  }
  wifiCredentials.setLastConnected(pendingSsid.c_str());

  auto transfer = makeUniqueNoThrow<FileTransferScreen>(gfx, input, pendingSsid);
  if (!transfer) {
    LOG_ERR("WIFI", "OOM: file transfer");
    setNotice(uiText::outOfMemory);
    // Stay on Connecting and pollConnect() would call this every tick.
    showNetworkList();
    return;
  }
  notice = nullptr;
  push(std::move(transfer));
}

void WifiListScreen::loop() {
  if (state != State::Failed && state != State::ClearPassword && input.wasReleased(MappedInput::Button::Back)) {
    finish();
    return;
  }

  switch (state) {
    case State::Scanning:
      if (input.wasReleased(MappedInput::Button::Confirm)) {
        skipAutoJoin = true;
      }
      if (wifiManager.scanComplete(networks)) {
        std::sort(networks.begin(), networks.end(), [](const WifiManager::Network& a, const WifiManager::Network& b) {
          const bool aSaved = wifiCredentials.find(a.ssid.c_str()) != nullptr;
          const bool bSaved = wifiCredentials.find(b.ssid.c_str()) != nullptr;
          if (aSaved != bSaved) {
            return aSaved;
          }
          return a.rssi > b.rssi;
        });
        index = 0;
        window = 0;
        if (tryAutoJoin()) {
          return;
        }
        showNetworkList();
      }
      return;
    case State::NetworkList: {
      const int count = static_cast<int>(networks.size());
      if (ui::applyDelta(index, input.consumeNavigationDelta(), count)) {
        requestUpdate();
      } else if (count > 0 && input.wasReleased(MappedInput::Button::Confirm)) {
        selectNetwork();
      }
      return;
    }
    case State::Connecting:
      if (autoConnecting && input.wasReleased(MappedInput::Button::Confirm)) {
        wifiManager.abortConnect();
        skipAutoJoin = true;
        showNetworkList();
        return;
      }
      {
        const WifiManager::ConnectState result = wifiManager.pollConnect();
        if (result == WifiManager::ConnectState::Connected) {
          goToFileTransfer();
        } else if (result == WifiManager::ConnectState::Failed) {
          onConnectFailed();
        }
      }
      return;
    case State::Failed:
      if (input.wasReleased(MappedInput::Button::Confirm) || input.wasReleased(MappedInput::Button::Back)) {
        showNetworkList();
      }
      return;
    case State::ClearPassword:
      if (input.wasReleased(MappedInput::Button::Confirm)) {
        wifiCredentials.remove(pendingSsid.c_str());
        usedSavedPassword = false;
        enteredPassword.clear();
        LOG_INF("WIFI", "Cleared saved password for '%s'", pendingSsid.c_str());
        if (!promptPassword()) {
          showNetworkList();
        }
      } else if (input.wasReleased(MappedInput::Button::Back)) {
        showNetworkList();
      }
      return;
  }
}

void WifiListScreen::render() {
  gfx.clear(false);
  gfx.drawCenteredText(FONT_UI_BOLD, 24, uiText::wifi);

  switch (state) {
    case State::Scanning:
      gfx.drawCenteredText(FONT_UI, gfx.height() / 2,
                           wifiCredentials.hasAny() ? uiText::lookingForSavedWifi : uiText::scanning);
      if (wifiCredentials.hasAny()) {
        gfx.drawCenteredText(FONT_UI, gfx.height() / 2 + 36, uiText::confirmToPickNetwork);
      }
      break;
    case State::NetworkList: {
      if (networks.empty()) {
        gfx.drawCenteredText(FONT_UI, gfx.height() / 2, uiText::noNetworks);
        break;
      }
      const int rowH = gfx.lineHeight(FONT_UI) + 8;
      const int top = 64;
      const int rows = (gfx.height() - top - (notice ? 48 : 24)) / rowH;
      ui::followWindow(window, index, rows);
      const int last = std::min(window + rows, static_cast<int>(networks.size()));
      char label[80];
      for (int i = window; i < last; ++i) {
        const auto& net = networks[static_cast<size_t>(i)];
        const bool saved = wifiCredentials.find(net.ssid.c_str()) != nullptr;
        const char* mark = saved ? uiText::savedMark : (net.encrypted ? uiText::lockedMark : "");
        snprintf(label, sizeof(label), "%s%s", net.ssid.c_str(), mark);
        ui::drawRow(gfx, top + (i - window) * rowH, rowH, label, i == index);
      }
      break;
    }
    case State::Connecting: {
      char msg[80];
      snprintf(msg, sizeof(msg), uiText::connectingTo, pendingSsid.c_str());
      gfx.drawCenteredText(FONT_UI, gfx.height() / 2, msg);
      if (autoConnecting) {
        gfx.drawCenteredText(FONT_UI, gfx.height() / 2 + 36, uiText::confirmToPickNetwork);
      }
      break;
    }
    case State::Failed:
      gfx.drawCenteredText(FONT_UI, gfx.height() / 2 - 20, uiText::connectionFailed);
      gfx.drawCenteredText(FONT_UI, gfx.height() / 2 + 20, uiText::confirmRetry);
      break;
    case State::ClearPassword:
      gfx.drawCenteredText(FONT_UI_BOLD, gfx.height() / 2 - 48, uiText::connectionFailed);
      gfx.drawCenteredText(FONT_UI, gfx.height() / 2 - 12, pendingSsid.c_str());
      gfx.drawCenteredText(FONT_UI, gfx.height() / 2 + 28, uiText::confirmClearPassword);
      gfx.drawCenteredText(FONT_UI, gfx.height() / 2 + 56, uiText::backToKeepPassword);
      break;
  }

  drawNotice();
  presentUi();
}
